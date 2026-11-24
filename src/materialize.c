/* materialize.c - see materialize.h. Moved out of engine.c so the
 * host CLI can boot from a D88 exactly the way the embedded
 * engine does, instead of only finding plain files in the
 * working directory. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "materialize.h"
#include "p88disk.h"
#include "p88conv.h"

/* The name a BASIC program materializes under. dot points at the
 * extension's '.' inside base (or is 0); base is cut there, so on
 * return it holds the bare program name. */
static void bas_matname(char *matname, size_t cap, char *base, char *dot)
{
    if (dot) {
        /* A program whose disk name carries an extension
         * (MENU.ALP) materializes as MENU_ALP.BAS: writing
         * MENU.BAS would collide case-insensitively with a
         * distinct program named "menu" (iOS returns EEXIST),
         * and the guest only ever asks for the extension-less
         * name. */
        char ext[4];
        snprintf(ext, sizeof(ext), "%.3s", dot + 1);
        *dot = 0;
        snprintf(matname, cap, "%.10s_%.3s.BAS", base, ext);
    } else {
        snprintf(matname, cap, "%.15s.BAS", base);
    }
}

/* Case-insensitive name equality: the workdir may sit on a case-
 * insensitive host file system (macOS, iOS). */
static int name_eq(const char *a, const char *b)
{
    for (;; a++, b++) {
        int x = (unsigned char)*a, y = (unsigned char)*b;
        if (x >= 'a' && x <= 'z') x -= 32;
        if (y >= 'a' && y <= 'z') y -= 32;
        if (x != y) return 0;
        if (!x) return 1;
    }
}

/* The workdir is one flat namespace, so two disk entries can want the
 * same host name (a raw file against another entry's materialized
 * .BAS, or names differing only in case). First writer wins: a name
 * is claimed by the write that creates it, and a later write under a
 * claimed name is skipped instead of silently replacing the file.
 * Returns 1 when the write was this name's first (and so happened),
 * 0 when it was skipped. The list is per run: p8_materialize resets
 * it, as each run fills a fresh directory. */
#define CLAIM_MAX 128
static char claimed[CLAIM_MAX][64];
static int  nclaimed;

static int write_claimed(const char *workdir, const char *name,
                         const void *data, size_t len)
{
    char path[320];
    FILE *fp;
    int k;
    for (k = 0; k < nclaimed; k++)
        if (name_eq(claimed[k], name)) return 0;
    if (nclaimed >= CLAIM_MAX) return 0;
    snprintf(claimed[nclaimed++], sizeof(claimed[0]), "%s", name);
    snprintf(path, sizeof(path), "%s/%s", workdir, name);
    fp = fopen(path, "wb");
    if (fp) { fwrite(data, 1, len, fp); fclose(fp); }
    return 1;
}

/* Does an untokenized file hold an ASCII-saved BASIC program? Genuine
 * N88-BASIC boots those too (disk attr 0x00), but the attribute alone
 * cannot tell them from machine code or data, so the content has to
 * be something prog_load (exec.c) accepts, by its own rules: the
 * text ends at the first 0x1A or at the end of the file; lines split
 * on \r or \n; leading spaces / tabs are skipped; blank lines are
 * fine; any other line must start with a line number <= 65529; and
 * a line's content ends at its first 0x00 byte (prog_load reads the
 * line back as a C string - ALPHA-MINI's own MENU.ALP carries a
 * 0x00 just before its 0x1A, after the last line). Two guards on top
 * keep binary files out: a 0xFF first byte is the tokenized marker
 * prog_load itself rejects, and no 0x00 / 0xFF may appear in the
 * first 512 bytes, where a data file's header would sit. At least
 * one numbered line must be present. Any doubt means "not a
 * program": the file is then written raw only, as it always was.
 * (Boot safety does not rest on this test alone: the engine loads
 * the winner through prog_load_boot, which catches a load error.) */
static int sniff_ascii_basic(const unsigned char *b, long sz)
{
    long end, head, i;
    int numbered = 0;
    if (!b || sz <= 0) return 0;
    if (b[0] == 0xFF) return 0;
    for (end = 0; end < sz && b[end] != 0x1A; end++) ;
    head = end < 512 ? end : 512;
    for (i = 0; i < head; i++)
        if (b[i] == 0x00 || b[i] == 0xFF) return 0;
    i = 0;
    while (i < end) {
        long ln = 0;
        while (i < end && (b[i] == ' ' || b[i] == '\t')) i++;
        if (i < end && b[i] != '\r' && b[i] != '\n' && b[i] != 0x00) {
            if (b[i] < '0' || b[i] > '9') return 0;
            while (i < end && b[i] >= '0' && b[i] <= '9') {
                ln = ln * 10 + (b[i] - '0');
                if (ln > 65529L) return 0;
                i++;
            }
            numbered = 1;
        }
        /* skip the rest of the line: up to \r, \n or the text end,
         * with a 0x00 ending the visible content where it stands */
        while (i < end && b[i] != '\r' && b[i] != '\n') i++;
        if (i < end) i++;       /* the line end itself */
    }
    return numbered;
}

char *p8_materialize(char *bootfile, size_t bootfile_cap)
{
    P88FileInfo files[64];
    static unsigned char buf[1 << 20];
    char tmpl[256];
    char boot[32];
    const char *tmp = getenv("TMPDIR");
    char *workdir;
    int n, i;
    if (!tmp || !*tmp) tmp = "/tmp";
    snprintf(tmpl, sizeof(tmpl), "%s/p8pXXXXXX", tmp);
    if (!mkdtemp(tmpl)) return 0;
    workdir = strdup(tmpl);
    if (!workdir) return 0;
    n = p88disk_list(files, 64);
    if (n < 0) { free(workdir); return 0; }
    boot[0] = 0;
    nclaimed = 0;
    for (i = 0; i < n; i++) {
        long sz = p88disk_read(files[i].name, buf, sizeof(buf));
        char base[16], rawname[16];
        char *dot;
        if (sz < 0) continue;
        snprintf(rawname, sizeof(rawname), "%.15s", files[i].name);
        /* base name without extension, for the .BAS materialization */
        memset(base, 0, sizeof(base));
        strncpy(base, files[i].name, sizeof(base) - 1);
        dot = strrchr(base, '.');
        if (files[i].attr & 0x80) {
            char *text, *report = 0;
            unsigned long tlen = 0;
            char matname[32];
            int written;
            bas_matname(matname, sizeof(matname), base, dot);
            /* The raw bytes go out under the disk name as well: the
             * attribute does not prove a program (ALPHA-MINI's
             * 88BD1.PC8 is PCM data carrying 0x80), and genuine BLOAD
             * reads any file by name, whatever its attribute. Written
             * before the conversion, so a file that fails to convert
             * still exists raw instead of vanishing. */
            write_claimed(workdir, rawname, buf, (size_t)sz);
            text = p88_convert(buf, (unsigned long)sz, &tlen, &report);
            free(report);
            if (!text) continue;
            /* A converted write that lost its name to an earlier file
             * confers no boot candidacy either: what sits under that
             * name is not this program. */
            written = write_claimed(workdir, matname, text, tlen);
            free(text);
            if (written && (!boot[0] || strcmp(base, "MENU") == 0))
                snprintf(boot, sizeof(boot), "%s", matname);
        } else if (dot && (name_eq(dot, ".BAS") || name_eq(dot, ".ALP")) &&
                   sniff_ascii_basic(buf, sz)) {
            /* An ASCII-saved program (attr 0x00): the bytes are
             * already what prog_load reads - it tokenizes text
             * itself - so they go out unconverted, under the same
             * name and boot preference as a tokenized program. They
             * also stay under the disk name, as before ASCII boot
             * support, so other programs still reach the file by
             * that name (LOAD / RUN / BLOAD). */
            char matname[32];
            bas_matname(matname, sizeof(matname), base, dot);
            write_claimed(workdir, rawname, buf, (size_t)sz);
            if (write_claimed(workdir, matname, buf, (size_t)sz) &&
                (!boot[0] || strcmp(base, "MENU") == 0))
                snprintf(boot, sizeof(boot), "%s", matname);
        } else {
            write_claimed(workdir, rawname, buf, (size_t)sz);
        }
    }
    if (bootfile && bootfile_cap) {
        snprintf(bootfile, bootfile_cap, "%s", boot);
        if (!boot[0]) { free(workdir); return 0; }
    }
    return workdir;
}
