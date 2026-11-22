/* opna_ymfm.cpp - OPNA (YM2608) rendering via ymfm (third_party/ymfm).
 *   Mirrors the driving pattern proven in RetroDOS Player's renderer:
 *   OPN_FIDELITY_MED, prescaler writes (2Dh-2Fh) refresh the chip's
 *   native rate, and the chip-rate stream is box-filter averaged down
 *   to the host rate (44.1 kHz). Output mix is FM L/R plus the SSG
 *   output at 0.20 (at 1:1 the SSG measures 5-18 dB hot vs hardware).
 *   ADPCM-B gets a real 256 KiB RAM behind the chip; the ADPCM-A
 *   rhythm ROM reads as zero until a later phase supplies it.
 *   Audio is rendered only when P8PWAV names an output WAV file.
 */
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cstdint>
#include <algorithm>
#include <string>
#include <vector>
#include "ymfm.h"
#include "ymfm_opn.h"
#include "opna.h"
#include "vessel.h"

namespace {

const uint32_t CHIP_CLOCK = 7987200;    /* OPNA master clock on SB2 */
const int HOST_RATE = 44100;
long g_cpu_clock = 4000000;             /* vessel CPU clock (opna_set_cpu_clock);
                                         * CHIP_CLOCK does not follow it: the
                                         * sound board has its own oscillator */

unsigned long g_ext_reads_b = 0, g_ext_writes_b = 0;   /* diagnostics */
unsigned long g_ext_reads_a = 0;

struct Iface : ymfm::ymfm_interface {
    /* ADPCM-B: the SB2's 256 KiB sample DRAM (guest-writable; FFDE
     * service 1 and bank-1 register writes fill it). ADPCM-A: the
     * chip's rhythm ROM image - read-only to the guest, all zeros
     * until a host-side supply fills it through
     * opna_adpcm_a_write. ymfm's YM2608 only reads its fixed rhythm
     * layout at the start of this buffer. */
    std::vector<uint8_t> adpcm_b_ram;
    std::vector<uint8_t> adpcm_a_ram;
    Iface() : adpcm_b_ram(256 * 1024, 0), adpcm_a_ram(1024 * 1024, 0) {}
    uint8_t ymfm_external_read(ymfm::access_class type, uint32_t offset) override
    {
        if (type == ymfm::ACCESS_ADPCM_B && offset < adpcm_b_ram.size()) {
            g_ext_reads_b++;
            return adpcm_b_ram[offset];
        }
        if (type == ymfm::ACCESS_ADPCM_A && offset < adpcm_a_ram.size()) {
            g_ext_reads_a++;
            return adpcm_a_ram[offset];
        }
        return 0;
    }
    void ymfm_external_write(ymfm::access_class type, uint32_t offset, uint8_t data) override
    {
        if (type == ymfm::ACCESS_ADPCM_B && offset < adpcm_b_ram.size()) {
            g_ext_writes_b++;
            adpcm_b_ram[offset] = data;
        }
    }
};

Iface iface;
ymfm::ym2608 chip(iface);

/* Chip-sample block buffer. push_frame() consumes chip-rate samples
 * from here; an empty buffer is refilled by ONE chip.generate() call.
 * A refill never generates more samples than the frames being rendered
 * right now will consume (block_need), so the chip is never clocked
 * past a point where a register write, a status read or an ADPCM
 * memory write could still land: the buffer is always empty again
 * when render_frames() returns. */
const int BLOCK_SAMPLES = 2048;
ymfm::ym2608::output_data block[BLOCK_SAMPLES];
int block_pos = 0, block_fill = 0;
long block_need = 0;            /* chip samples the current render still needs */

/* Output frames opna_advance() has accounted for but not rendered yet.
 * They are rendered (render_pending) before anything that changes or
 * observes chip state, whenever PENDING_MAX of them have piled up, and
 * as soon as they would complete a 512-frame output chunk. */
const long PENDING_MAX = 128;
long pending_frames = 0;
void render_pending(void);

int enabled = -1;               /* -1 unknown, 0 off, 1 on */
FILE *wav = nullptr;
opna_sink_fn sink_fn = nullptr;
void *sink_ctx = nullptr;
uint32_t chip_rate = 0;
uint32_t total_frames = 0;
double res_pos = 0;             /* box-filter resampler position */
double last_l = 0, last_r = 0;
long long frame_acc = 0;        /* cycles * HOST_RATE not yet rendered */
int16_t outbuf[4096 * 2];
int outfill = 0;
int trace_audio = 0;            /* P8P_TRACE_AUDIO: per-10 s render log */
int sec_peak = 0;

void wav_header(uint32_t data_bytes)
{
    unsigned char h[44];
    memset(h, 0, sizeof(h));
    memcpy(h + 0, "RIFF", 4);
    auto put32 = [&](int off, uint32_t v) {
        h[off] = v & 0xFF; h[off + 1] = (v >> 8) & 0xFF;
        h[off + 2] = (v >> 16) & 0xFF; h[off + 3] = (v >> 24) & 0xFF;
    };
    auto put16 = [&](int off, uint16_t v) {
        h[off] = v & 0xFF; h[off + 1] = (v >> 8) & 0xFF;
    };
    put32(4, 36 + data_bytes);
    memcpy(h + 8, "WAVE", 4);
    memcpy(h + 12, "fmt ", 4);
    put32(16, 16);
    put16(20, 1);               /* PCM */
    put16(22, 2);               /* stereo */
    put32(24, HOST_RATE);
    put32(28, HOST_RATE * 4);
    put16(32, 4);
    put16(34, 16);
    memcpy(h + 36, "data", 4);
    put32(40, data_bytes);
    fseek(wav, 0, SEEK_SET);
    fwrite(h, 1, 44, wav);
    fseek(wav, 0, SEEK_END);
}

void finalize(void)
{
    render_pending();
    if (outfill) {
        if (wav) fwrite(outbuf, 2, (size_t)outfill * 2, wav);
        if (sink_fn) sink_fn(outbuf, outfill, sink_ctx);
        outfill = 0;
    }
    if (!wav) return;
    wav_header(total_frames * 4);
    fclose(wav);
    wav = nullptr;
}

bool ensure_init(void)
{
    if (enabled >= 0) return enabled == 1;
    enabled = 0;
    const char *path = getenv("P8PWAV");
    if (path && *path) {
        wav = fopen(path, "wb");
        if (wav) {
            unsigned char zero[44] = {0};
            fwrite(zero, 1, 44, wav);   /* patched by finalize() */
            atexit(finalize);
        }
    }
    if (!wav && !sink_fn) return false;
    chip.reset();
    chip.set_fidelity(ymfm::OPN_FIDELITY_MED);
    chip_rate = chip.sample_rate(CHIP_CLOCK);
    trace_audio = getenv("P8P_TRACE_AUDIO") != nullptr;
    enabled = 1;
    return true;
}

int16_t clamp16(double v)
{
    if (v > 32767) return 32767;
    if (v < -32768) return -32768;
    return (int16_t)v;
}

void refill_block(void)
{
    /* With P8P_TRACE_AUDIO the chip is clocked one sample at a time so
     * the ADPCM-A read counter in the 10 s log line is exactly what the
     * unbatched renderer printed. */
    long n = trace_audio ? 1 : block_need;
    if (n > BLOCK_SAMPLES) n = BLOCK_SAMPLES;
    if (n < 1) n = 1;           /* unreachable: block_need is an exact count */
    chip.generate(block, (uint32_t)n);
    block_pos = 0;
    block_fill = (int)n;
    block_need -= n;
}

void push_frame(void)
{
    res_pos += (double)chip_rate / HOST_RATE;
    double al = 0, ar = 0;
    int n = 0;
    while (res_pos >= 1.0) {
        res_pos -= 1.0;
        if (block_pos == block_fill) refill_block();
        const ymfm::ym2608::output_data &out = block[block_pos++];
        al += out.data[0] + 0.20 * out.data[2];
        ar += out.data[1] + 0.20 * out.data[2];
        n++;
    }
    /* ymfm voices are full-amplitude each; the 6-channel digital sum
     * overflows int16 where the analog original would not, so the
     * harness WAV takes 0.5 of master headroom. */
    if (n) { last_l = al / n * 0.5; last_r = ar / n * 0.5; }
    outbuf[outfill * 2] = clamp16(last_l);
    outbuf[outfill * 2 + 1] = clamp16(last_r);
    if (trace_audio) {
        int p = outbuf[outfill * 2]; if (p < 0) p = -p;
        if (p > sec_peak) sec_peak = p;
        if ((total_frames + 1) % (HOST_RATE * 10) == 0) {
            fprintf(stderr, "AUDIO %u s: peak %d, keyons %lu, ssg %lu, pcma %lu, fm %lu, extA r %lu\n",
                    (total_frames + 1) / HOST_RATE, sec_peak,
                    vessel_opn_keyons(),
                    vessel_opn_class(0x00, 0x0F), vessel_opn_class(0x10, 0x1F),
                    vessel_opn_class(0x20, 0xFF), g_ext_reads_a);
            sec_peak = 0;
        }
    }
    outfill++;
    total_frames++;
    if (outfill == 512) {
        if (wav) fwrite(outbuf, 2, 512 * 2, wav);
        if (sink_fn) sink_fn(outbuf, 512, sink_ctx);
        outfill = 0;
    }
}

/* Render `frames` output frames. The chip samples they consume are
 * counted first by running the resampler accumulator dry (the same
 * double operations push_frame performs), so the refills can ask the
 * chip for exactly that many samples and no more. */
void render_frames(long frames)
{
    double pos = res_pos;
    long need = 0;
    for (long i = 0; i < frames; i++) {
        pos += (double)chip_rate / HOST_RATE;
        while (pos >= 1.0) { pos -= 1.0; need++; }
    }
    block_need = need;
    for (long i = 0; i < frames; i++) push_frame();
    block_need = 0;
}

void render_pending(void)
{
    long frames = pending_frames;
    pending_frames = 0;
    if (frames > 0) render_frames(frames);
}

/* ---- external rhythm sample supply (WAV -> ADPCM-A) ---- */

const size_t RHYTHM_REGION_END = 0x2000;        /* BD..RIM, see ym2608 reset */
const size_t WAV_MAX_BYTES = 64u * 1024 * 1024;
/* ymfm's ym2608 clocks ADPCM-A channels 4 and 5 (TOM, RIM) every
 * second envelope clock, i.e. at half the rate of channels 0-3. */
const double ADPCM_A_RATE = 7987200.0 / 432.0;
const double ADPCM_A_RATE_HALF = 7987200.0 / 864.0;

struct RhythmSlot {
    uint32_t start, end;        /* ADPCM-A memory bytes, end inclusive */
    double rate;                /* playback rate of the slot's channel */
    const char *names[5];
};
const RhythmSlot rhythm_slots[6] = {
    { 0x0000, 0x01BF, ADPCM_A_RATE, { "2608_BD.wav", "2608_BD.WAV", "2608_bd.wav", nullptr } },
    { 0x01C0, 0x043F, ADPCM_A_RATE, { "2608_SD.wav", "2608_SD.WAV", "2608_sd.wav", nullptr } },
    { 0x0440, 0x1B7F, ADPCM_A_RATE, { "2608_TOP.wav", "2608_TOP.WAV", "2608_top.wav", nullptr } },
    { 0x1B80, 0x1CFF, ADPCM_A_RATE, { "2608_HH.wav", "2608_HH.WAV", "2608_hh.wav", nullptr } },
    { 0x1D00, 0x1F7F, ADPCM_A_RATE_HALF, { "2608_TOM.wav", "2608_TOM.WAV", "2608_tom.wav", nullptr } },
    { 0x1F80, 0x1FFF, ADPCM_A_RATE_HALF, { "2608_RIM.wav", "2608_RIM.WAV", "2608_rim.wav", nullptr } },
};

uint32_t rd16(const uint8_t *p) { return p[0] | (uint32_t)p[1] << 8; }
uint32_t rd32(const uint8_t *p)
{
    return p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

bool read_file(FILE *f, std::vector<uint8_t> &buf)
{
    static uint8_t tmp[65536];
    size_t n;
    buf.clear();
    while ((n = fread(tmp, 1, sizeof(tmp), f)) > 0) {
        if (buf.size() + n > WAV_MAX_BYTES) return false;
        buf.insert(buf.end(), tmp, tmp + n);
    }
    return !ferror(f);
}

/* RIFF/WAVE: PCM 8-bit unsigned / 16-bit signed, IEEE float 32-bit;
 * mono or stereo (averaged). Decodes no more input frames than
 * `out_samples` samples at `out_rate` can use. */
bool wav_decode(const std::vector<uint8_t> &buf, size_t out_samples,
                double out_rate, std::vector<float> &mono, uint32_t &rate)
{
    /* KSDATAFORMAT_SUBTYPE_PCM / _IEEE_FLOAT after the format tag */
    static const uint8_t guid_tail[14] = {
        0x00, 0x00, 0x00, 0x00, 0x10, 0x00,
        0x80, 0x00, 0x00, 0xAA, 0x00, 0x38, 0x9B, 0x71
    };
    const uint8_t *d = buf.data();
    size_t size = buf.size();
    if (size < 12 || memcmp(d, "RIFF", 4) || memcmp(d + 8, "WAVE", 4)) return false;
    /* chunks must lie inside both the declared RIFF extent and the file */
    uint64_t extent = (uint64_t)rd32(d + 4) + 8;
    size_t limit = extent < size ? (size_t)extent : size;
    uint32_t fmt = 0, ch = 0, bits = 0, align = 0;
    const uint8_t *data = nullptr;
    size_t data_len = 0;
    rate = 0;
    size_t pos = 12;
    while (pos < limit && limit - pos >= 8) {
        size_t clen = rd32(d + pos + 4);
        const uint8_t *body = d + pos + 8;
        if (clen > limit - pos - 8) return false;
        if (!memcmp(d + pos, "fmt ", 4)) {
            if (clen < 16) return false;
            fmt = rd16(body);
            ch = rd16(body + 2);
            rate = rd32(body + 4);
            align = rd16(body + 12);
            bits = rd16(body + 14);
            if (fmt == 0xFFFE) {                /* WAVE_FORMAT_EXTENSIBLE */
                if (clen < 40 || rd16(body + 16) < 22) return false;
                fmt = rd16(body + 24);
                if (fmt != 1 && fmt != 3) return false;
                if (memcmp(body + 26, guid_tail, sizeof(guid_tail))) return false;
            }
        } else if (!memcmp(d + pos, "data", 4)) {
            data = body;
            data_len = clen;
            break;
        }
        pos += 8 + clen + (clen & 1);
    }
    if (!data || !fmt) return false;
    if (ch != 1 && ch != 2) return false;
    if (rate < 1 || rate > 768000) return false;
    if (!((fmt == 1 && (bits == 8 || bits == 16)) || (fmt == 3 && bits == 32)))
        return false;
    uint32_t bytes = bits / 8;
    if (align < bytes * ch) align = bytes * ch;
    if (data_len % align) return false;
    size_t frames = data_len / align;
    size_t max_frames = (size_t)((double)out_samples * rate / out_rate) + 2;
    if (frames > max_frames) frames = max_frames;
    if (frames == 0) return false;
    mono.resize(frames);
    for (size_t i = 0; i < frames; i++) {
        double acc = 0;
        for (uint32_t c = 0; c < ch; c++) {
            const uint8_t *p = data + i * align + c * bytes;
            double v;
            if (fmt == 3) {
                uint32_t u = rd32(p);
                float fv;
                memcpy(&fv, &u, 4);
                v = fv;
                if (v != v) v = 0.0;            /* NaN */
                if (v < -1.0) v = -1.0;
                if (v > 1.0) v = 1.0;
            } else if (bits == 8) {
                v = ((int)p[0] - 128) / 128.0;
            } else {
                v = (int16_t)rd16(p) / 32768.0;
            }
            acc += v;
        }
        mono[i] = (float)(acc / ch);
    }
    return true;
}

/* Resample (linear, endpoint-clamped) to `out_rate` and encode
 * greedily against the decoder recurrence of ymfm's
 * adpcm_a_channel::clock(); high nibble first. The chip plays the
 * whole slot and a zero nibble is not silence (it adds step/8), so
 * past the sample's end the encoder keeps tracking a zero target
 * until all `cap` bytes of `out` are written. Returns false, with
 * `out` untouched, when there is nothing to encode. */
bool adpcm_a_encode(const std::vector<float> &in, double in_rate,
                    double out_rate, uint8_t *out, size_t cap)
{
    static const uint16_t s_steps[49] = {
         16,  17,   19,   21,   23,   25,   28,
         31,  34,   37,   41,   45,   50,   55,
         60,  66,   73,   80,   88,   97,  107,
        118, 130,  143,  157,  173,  190,  209,
        230, 253,  279,  307,  337,  371,  408,
        449, 494,  544,  598,  658,  724,  796,
        876, 963, 1060, 1166, 1282, 1411, 1552
    };
    static const int8_t s_step_inc[8] = { -1, -1, -1, -1, 2, 5, 7, 9 };
    if (in.empty() || in_rate <= 0 || out_rate <= 0 || cap == 0) return false;
    double total = (double)in.size() * out_rate / in_rate;
    size_t count = total >= (double)(cap * 2) ? cap * 2 : (size_t)total;
    if (count == 0) count = 1;  /* a nonempty input yields a sample */
    int acc = 0, step = 0;      /* decoder state at key-on */
    size_t bytes = 0;
    for (size_t i = 0; i < cap * 2; i++) {
        long target = 0;        /* past the sample: silence */
        if (i < count) {
            double pos = (double)i * in_rate / out_rate;
            size_t i0 = (size_t)pos;
            if (i0 >= in.size()) i0 = in.size() - 1;
            size_t i1 = i0 + 1 < in.size() ? i0 + 1 : i0;
            double frac = pos - (double)i0;
            if (frac < 0) frac = 0;
            if (frac > 1) frac = 1;
            double s = in[i0] + (in[i1] - in[i0]) * frac;
            target = (long)(s * 2047.0);
            if (target > 2047) target = 2047;
            if (target < -2048) target = -2048;
        }

        int best_n = 0, best_acc = acc;
        long best_err = -1;
        for (int n = 0; n < 16; n++) {
            int delta = (2 * (n & 7) + 1) * s_steps[step] / 8;
            if (n & 8) delta = -delta;
            int a = (acc + delta) & 0xFFF;
            long sv = (a & 0x800) ? a - 0x1000 : a;
            long err = sv > target ? sv - target : target - sv;
            if (best_err < 0 || err < best_err) {
                best_err = err; best_n = n; best_acc = a;
            }
        }
        acc = best_acc;
        step += s_step_inc[best_n & 7];
        if (step < 0) step = 0;
        if (step > 48) step = 48;

        if ((i & 1) == 0) {
            out[bytes] = (uint8_t)(best_n << 4);
        } else {
            out[bytes] |= (uint8_t)best_n;
            bytes++;
        }
    }
    return true;
}

bool rhythm_load_file(FILE *f, const RhythmSlot &slot)
{
    size_t cap = slot.end - slot.start + 1;
    std::vector<uint8_t> buf;
    std::vector<float> mono;
    std::vector<uint8_t> enc(cap, 0);
    uint32_t rate = 0;
    if (!read_file(f, buf)) return false;
    if (!wav_decode(buf, cap * 2, slot.rate, mono, rate)) return false;
    if (!adpcm_a_encode(mono, (double)rate, slot.rate, enc.data(), cap)) return false;
    memcpy(&iface.adpcm_a_ram[slot.start], enc.data(), cap);
    return true;
}

} // namespace

extern "C" void opna_adpcm_b_write(unsigned long addr, int value)
{
    render_pending();           /* earlier frames read the old contents */
    if (addr < iface.adpcm_b_ram.size())
        iface.adpcm_b_ram[(size_t)addr] = (uint8_t)value;
}

extern "C" int opna_adpcm_b_read(unsigned long addr)
{
    return addr < iface.adpcm_b_ram.size() ? iface.adpcm_b_ram[(size_t)addr] : 0;
}

/* Not called today: the ADPCM-A memory is the chip's rhythm ROM,
 * which no guest path can write. Kept for a future host-side
 * rhythm ROM supply. */
extern "C" void opna_adpcm_a_write(unsigned long addr, int value)
{
    render_pending();
    if (addr < iface.adpcm_a_ram.size())
        iface.adpcm_a_ram[(size_t)addr] = (uint8_t)value;
}

extern "C" int opna_adpcm_a_read(unsigned long addr)
{
    return addr < iface.adpcm_a_ram.size() ? iface.adpcm_a_ram[(size_t)addr] : 0;
}

extern "C" void opna_clear_rhythm(void)
{
    auto first = iface.adpcm_a_ram.begin();
    auto last = first + RHYTHM_REGION_END;
    if (std::all_of(first, last, [](uint8_t b) { return b == 0; }))
        return;                 /* nothing to change, nothing to flush */
    render_pending();           /* earlier frames read the old contents */
    std::fill(first, last, 0);
}

/* External rhythm sample supply: encode <dir>/<instrument>.wav into
 * the fixed rhythm layout. Returns the number of instruments loaded. */
extern "C" int opna_load_rhythm_dir(const char *dir)
{
    int loaded = 0;
    opna_clear_rhythm();
    if (!dir || !*dir) return 0;
    render_pending();           /* earlier frames read the old contents */
    for (const RhythmSlot &slot : rhythm_slots) {
        FILE *f = nullptr;
        try {
            for (int i = 0; slot.names[i] && !f; i++) {
                std::string path = std::string(dir) + "/" + slot.names[i];
                f = fopen(path.c_str(), "rb");
            }
            if (f && rhythm_load_file(f, slot)) loaded++;
        } catch (...) {
            /* allocation failure: this instrument stays zero */
        }
        if (f) fclose(f);
    }
    return loaded;
}

extern "C" void opna_set_sink(opna_sink_fn fn, void *ctx)
{
    /* Render the owed frames while the outgoing sink is still set. Only
     * chunks this COMPLETES (512 frames) reach that sink: a partial
     * chunk stays in outbuf and would be delivered to the new sink
     * after a direct switch. The engine's destroy/create sequence
     * avoids that by calling opna_reset() first, which drops it. */
    render_pending();
    sink_fn = fn;
    sink_ctx = ctx;
    if (enabled == 0) enabled = -1;   /* re-probe on the next write */
}

extern "C" void opna_reset(void)
{
    render_pending();           /* deliver what the old session is owed */
    block_pos = block_fill = 0; /* never carry chip samples across sessions */
    block_need = 0;
    std::fill(iface.adpcm_a_ram.begin(), iface.adpcm_a_ram.end(), 0);
    std::fill(iface.adpcm_b_ram.begin(), iface.adpcm_b_ram.end(), 0);
    g_ext_reads_a = g_ext_reads_b = g_ext_writes_b = 0;
    res_pos = 0; last_l = last_r = 0;
    frame_acc = 0; outfill = 0; total_frames = 0; sec_peak = 0;
    if (enabled == 1) {
        chip.reset();
        chip.set_fidelity(ymfm::OPN_FIDELITY_MED);
        chip_rate = chip.sample_rate(CHIP_CLOCK);
    }
}

extern "C" void opna_write(int bank, int addr, int value)
{
    if (!ensure_init()) return;
    render_pending();           /* frames before this write use the old state */
    if (bank == 0) {
        chip.write_address((uint8_t)addr);
        chip.write_data((uint8_t)value);
        if (addr >= 0x2D && addr <= 0x2F)
            chip_rate = chip.sample_rate(CHIP_CLOCK);
    } else {
        chip.write_address_hi((uint8_t)addr);
        chip.write_data_hi((uint8_t)value);
    }
}

extern "C" int opna_status_hi(void)
{
    /* Headless fallback (no WAV/sink, chip inert): report EOS set
     * with PLAYING/BUSY clear so PMD's completion check proceeds.
     * Note this reads EOS as set before any key-on and BRDY as
     * clear, unlike the live chip. */
    if (!ensure_init()) return 0x04;
    render_pending();           /* status reflects every elapsed frame */
    return chip.read_status_hi();
}

extern "C" int opna_data_hi(int addr)
{
    if (!ensure_init()) return 0;
    render_pending();           /* a memory read moves the chip's address */
    /* The vessel keeps the address latch and forwards writes as
     * address + data pairs, so the chip's own latch is set here. */
    chip.write_address_hi((uint8_t)addr);
    return chip.read_data_hi();
}

extern "C" unsigned long opna_adpcm_b_ext_reads(void)
{
    return g_ext_reads_b;
}

extern "C" void opna_set_cpu_clock(long hz)
{
    if (hz <= 0 || hz == g_cpu_clock) return;
    g_cpu_clock = hz;
    frame_acc = 0;      /* the sub-frame remainder was in the old clock's units */
}

extern "C" void opna_advance(long cpu_cycles)
{
    if (!ensure_init()) return;
    frame_acc += (long long)cpu_cycles * HOST_RATE;
    long frames = (long)(frame_acc / g_cpu_clock);
    frame_acc %= g_cpu_clock;
    if (frames > HOST_RATE) frames = HOST_RATE;   /* bound pathological deltas */
    pending_frames += frames;
    /* The third condition keeps chunk delivery on the same advance as
     * the unbatched renderer: once the owed frames complete a 512-frame
     * output chunk they are rendered now, so the chunk is not left
     * unpublished across a pause or after the engine thread exits. */
    if (pending_frames >= PENDING_MAX || trace_audio ||
        outfill + pending_frames >= 512)
        render_pending();
}
