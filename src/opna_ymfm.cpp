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
