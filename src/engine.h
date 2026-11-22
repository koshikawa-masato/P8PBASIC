/* engine.h - P8PBASIC embedded engine: public C API for host
 * applications (RetroDOS Player on iOS). See docs/IOS-INTEGRATION.md.
 *
 * The interpreter is a single-instance design: one live engine per
 * process. The engine runs the guest on its own thread; the host
 * feeds keys, drains audio, and reads the screen buffers.
 *
 * Boot model: p8_boot materializes the D88's files into a scratch
 * directory (tokenized BASIC programs are converted to .BAS text
 * with p88conv), makes it the working directory, loads the boot
 * program (MENU if present) and starts it with RUN. File writes by
 * the guest land in the scratch directory; the D88 is read-only.
 */
#ifndef P8_ENGINE_H
#define P8_ENGINE_H

#ifdef __cplusplus
extern "C" {
#endif

typedef struct P8Engine P8Engine;

P8Engine *p8_create(const char *d88_path);
void      p8_destroy(P8Engine *e);

/* Materialize + load the boot program. Returns 0 on success.
 * Call p8_start afterwards to run it. */
int       p8_boot(P8Engine *e);
void      p8_start(P8Engine *e);
void      p8_stop(P8Engine *e);          /* blocks until stopped */
int       p8_running(P8Engine *e);

/* Freeze/resume the guest (the sequencer and the audio clock stop
 * with it; on resume there is no catch-up burst). */
void      p8_set_paused(P8Engine *e, int paused);
int       p8_paused(P8Engine *e);

/* Input: character / special keys (KC_* from hal.h) and the raw
 * keyboard matrix (16 bytes, as hal_keymatrix returns). */
void      p8_push_key(P8Engine *e, int code);
void      p8_set_matrix(P8Engine *e, int idx, unsigned char v);

/* Audio: 44.1 kHz stereo s16. Returns frames actually read. */
int       p8_audio_read(P8Engine *e, short *out, int frames);
unsigned long p8_audio_dropped(P8Engine *e);
unsigned long p8_frame_count(P8Engine *e);   /* guest frames (VRTC wraps) since session start */

/* Screen: graphics are composited into a 640x400 RGBA buffer.
 * Text is NOT drawn into it (the engine ships no font ROM); read
 * the text cells and draw them with the host's own font. */
void      p8_frame(P8Engine *e, unsigned char *rgba);
const unsigned short *p8_text_codes(P8Engine *e);   /* 80x25 */
const unsigned char  *p8_text_attrs(P8Engine *e);   /* 80x25 */
void      p8_cursor(P8Engine *e, int *x, int *y, int *visible);

#ifdef __cplusplus
}
#endif

#endif
