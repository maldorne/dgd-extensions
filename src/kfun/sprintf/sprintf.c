/*
 * sprintf() kfun extension for DGD (Hexagon).
 *
 * Contract:
 *   string sprintf(string format, mixed args...);
 *
 * Conversion characters:
 *   %s %S %d %i %o %x %X %b %c %a %A %y %f %e %E %g %G %h %H %n %p %%
 *   (%O is recognised but raises a runtime error -- see the case below
 *    for the rationale.)
 *
 * Flags:
 *   - _ | + ' ' 0 # ` & ~ < > =
 *
 * Width:     integer or '*' (taken from next arg).
 * Precision: '.' integer or '.*' (taken from next arg).  Integer precision
 *            on numerics pads with leading zeros; on strings truncates.
 *
 * Visible-width awareness
 * -----------------------
 * Width and centering are computed in screen columns, not bytes.  ANSI CSI
 * escape sequences (ESC '[' ... <0x40..0x7E>) count as zero columns; UTF-8
 * multibyte characters count as one column each (regardless of how many
 * bytes they occupy).
 *
 * Out of scope
 * ------------
 *   * %r, %R (encrypt) and %q, %Q (call_other) are not implemented; they
 *     are not used in Hexagon.
 *   * @* time conversions are not implemented.
 *   * %O is recognised but raises a runtime error.  The public DGD
 *     extension API exposes lpc_object_putval but not lpc_object_getval,
 *     so this kfun cannot retrieve an Object* from an LPC_value to feed
 *     into lpc_object_name().  Callers should use the equivalent
 *     sprintf("%s", object_name(ob)) instead.  See the %O case in the
 *     conversion switch for the path forward if this ever becomes a
 *     real limitation.
 *
 * Limitations
 * -----------
 *   * Precision-based truncation (%.Ns / %_.Ns) is byte-based.  Truncating
 *     UTF-8 / ANSI text by precision may produce malformed output.
 *   * East Asian fullwidth characters and combining marks count as one
 *     column each.  Doing this right would require Unicode East Asian
 *     Width tables.
 */

#include "lpc_ext.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <math.h>

/* ============================================================
 *  flag bitmap
 * ============================================================ */

#define F_LEFT          0x00001  /* -  left-align */
#define F_TRUNC_LEFT    0x00002  /* _  truncate from left when precision applies */
#define F_CENTER        0x00004  /* |  centered */
#define F_PLUS          0x00008  /* +  force sign on positive */
#define F_SPACE         0x00010  /* ' ' leading space when no sign */
#define F_ZEROPAD       0x00020  /* 0  pad numerics with zeros */
#define F_TABLE         0x00040  /* #  table mode (string only) */
#define F_REVERSE       0x00080  /* `  reverse string */
#define F_ROT13         0x00100  /* &  rot-13 string */
#define F_FLIPCASE      0x00200  /* ~  flip case */
#define F_LOWER         0x00400  /* <  lower case */
#define F_UPPER         0x00800  /* >  upper case */
#define F_CAPITAL       0x01000  /* =  capitalize first letter */
#define F_UPPERHEX      0x02000  /* internal: %X (uppercase hex digits) */

#define MAX_OBJNAME     1024     /* buffer size for lpc_object_name */
#define ANYTHING_MAX_DEPTH 16    /* recursion guard for %y */

/* ============================================================
 *  dynamic byte buffer
 * ============================================================ */

typedef struct {
    char *data;
    int   len;
    int   cap;
} Buf;

static void buf_init(Buf *b) {
    b->data = NULL;
    b->len  = 0;
    b->cap  = 0;
}

static void buf_free(Buf *b) {
    free(b->data);
    b->data = NULL;
    b->len  = 0;
    b->cap  = 0;
}

static int buf_grow(Buf *b, int more) {
    int need = b->len + more;
    int newcap;
    char *p;

    if (need <= b->cap) return 1;
    newcap = (b->cap == 0) ? 64 : b->cap;
    while (newcap < need) newcap *= 2;
    p = (char *)realloc(b->data, (size_t)newcap);
    if (p == NULL) return 0;
    b->data = p;
    b->cap  = newcap;
    return 1;
}

static int buf_append(Buf *b, const char *src, int n) {
    if (n <= 0) return 1;
    if (!buf_grow(b, n)) return 0;
    memcpy(b->data + b->len, src, (size_t)n);
    b->len += n;
    return 1;
}

static int buf_append_byte(Buf *b, char c) {
    if (!buf_grow(b, 1)) return 0;
    b->data[b->len++] = c;
    return 1;
}

static int buf_append_pad(Buf *b, int n, const char *pad, int padlen) {
    int i;
    if (n <= 0 || padlen <= 0) return 1;
    if (!buf_grow(b, n)) return 0;
    for (i = 0; i < n; i++) {
        b->data[b->len + i] = pad[i % padlen];
    }
    b->len += n;
    return 1;
}

/* ============================================================
 *  visible width (UTF-8 + ANSI CSI aware)
 *
 *  Mirrors lib/core/efuns/strings/strlen.c; see that file for
 *  the rationale and limitations.
 * ============================================================ */

/* ============================================================
 *  advance_margin: track visible column since last newline
 *
 *  Used by format_string to give every %s (and friends) a `margin`
 *  equal to the visible width of all format-string literal chunks
 *  emitted since the last '\n'. multi_line uses that margin to
 *  hang-indent every wrapped continuation line so it column-aligns
 *  with the first line of the field.
 *
 *  Matches the LPC packages/sprintf.c behaviour: `margin` accumulates
 *  the length of literal chunks between the start (or the last '\n')
 *  and the current directive.
 * ============================================================ */

static int advance_margin(int margin, const char *s, int len) {
    int i;
    for (i = 0; i < len; i++) {
        unsigned char b = (unsigned char)s[i];

        /* Matches LPC packages/sprintf.c which counts newlines as
         * literal characters via strlen(chunk, true). The extra
         * column of indent that produces on continuation lines is
         * the LPC visual authors have relied on for years. */

        if (b == 27) {
            if (i + 1 < len && s[i + 1] == '[') {
                i += 2;
                while (i < len) {
                    unsigned char c = (unsigned char)s[i];
                    if (c >= 0x40 && c <= 0x7E) break;
                    i++;
                }
            }
            continue;
        }

        if (b >= 0xC2 && b <= 0xDF) { i += 1; margin++; continue; }
        if (b >= 0xE0 && b <= 0xEF) { i += 2; margin++; continue; }
        if (b >= 0xF0 && b <= 0xF7) { i += 3; margin++; continue; }
        if (b >= 0x80 && b <= 0xBF) continue;

        margin++;
    }
    return margin;
}

static int vis_width(const char *s, int len) {
    int i, w = 0;
    for (i = 0; i < len; i++) {
        unsigned char b = (unsigned char)s[i];

        /* ANSI CSI: ESC '[' ... <final 0x40..0x7E> */
        if (b == 27) {
            if (i + 1 < len && s[i + 1] == '[') {
                i += 2;
                while (i < len) {
                    unsigned char c = (unsigned char)s[i];
                    if (c >= 0x40 && c <= 0x7E) break;
                    i++;
                }
                /* i is at the final byte; for-loop's i++ moves past */
            }
            continue;
        }

        /* UTF-8 multibyte leads */
        if (b >= 0xC2 && b <= 0xDF) { i += 1; w++; continue; } /* 2-byte */
        if (b >= 0xE0 && b <= 0xEF) { i += 2; w++; continue; } /* 3-byte */
        if (b >= 0xF0 && b <= 0xF7) { i += 3; w++; continue; } /* 4-byte */

        /* stray UTF-8 trail */
        if (b >= 0x80 && b <= 0xBF) continue;

        w++;
    }
    return w;
}

/* ============================================================
 *  ASCII-only string mutation (skip UTF-8 multibyte bytes)
 *
 *  These operate in place on a byte buffer.  They only touch
 *  bytes in the ASCII letter range so UTF-8 sequences pass
 *  through untouched.
 * ============================================================ */

static void str_lower(char *s, int len) {
    int i;
    for (i = 0; i < len; i++) {
        if (s[i] >= 'A' && s[i] <= 'Z') s[i] = (char)(s[i] + ('a' - 'A'));
    }
}

static void str_upper(char *s, int len) {
    int i;
    for (i = 0; i < len; i++) {
        if (s[i] >= 'a' && s[i] <= 'z') s[i] = (char)(s[i] - ('a' - 'A'));
    }
}

/* Capitalize the first ASCII letter found, leave the rest untouched. */
static void str_capital(char *s, int len) {
    int i;
    for (i = 0; i < len; i++) {
        if (s[i] >= 'a' && s[i] <= 'z') {
            s[i] = (char)(s[i] - ('a' - 'A'));
            return;
        }
        if (s[i] >= 'A' && s[i] <= 'Z') return;
    }
}

static void str_flipcase(char *s, int len) {
    int i;
    for (i = 0; i < len; i++) {
        if      (s[i] >= 'A' && s[i] <= 'Z') s[i] = (char)(s[i] + ('a' - 'A'));
        else if (s[i] >= 'a' && s[i] <= 'z') s[i] = (char)(s[i] - ('a' - 'A'));
    }
}

static void str_rot13(char *s, int len) {
    int i;
    for (i = 0; i < len; i++) {
        char c = s[i];
        if      (c >= 'a' && c <= 'm') s[i] = (char)(c + 13);
        else if (c >= 'n' && c <= 'z') s[i] = (char)(c - 13);
        else if (c >= 'A' && c <= 'M') s[i] = (char)(c + 13);
        else if (c >= 'N' && c <= 'Z') s[i] = (char)(c - 13);
    }
}

static void str_reverse(char *s, int len) {
    int i;
    for (i = 0; i < len / 2; i++) {
        char tmp = s[i];
        s[i] = s[len - 1 - i];
        s[len - 1 - i] = tmp;
    }
}

static void apply_string_flags(char *s, int len, int flags) {
    if (flags & F_REVERSE)  str_reverse(s, len);
    if (flags & F_FLIPCASE) str_flipcase(s, len);
    if (flags & F_LOWER)    str_lower(s, len);
    if (flags & F_UPPER)    str_upper(s, len);
    if (flags & F_CAPITAL)  str_capital(s, len);
    if (flags & F_ROT13)    str_rot13(s, len);
}

/* ============================================================
 *  integer to base-N
 * ============================================================ */

static int digit_in_base(int d, int upper) {
    if (d < 10) return '0' + d;
    return (upper ? 'A' : 'a') + (d - 10);
}

/* Write |n| in given base into buf (no sign).  Returns byte count. */
static int int_to_base(char *buf, LPC_int n, int base, int upper) {
    char tmp[80];
    int i = 0, j;
    LPC_int v = (n < 0) ? -n : n;

    if (v == 0) {
        tmp[i++] = '0';
    } else {
        while (v != 0) {
            tmp[i++] = (char)digit_in_base((int)(v % base), upper);
            v /= base;
        }
    }
    for (j = 0; j < i; j++) buf[j] = tmp[i - 1 - j];
    return i;
}

/* ============================================================
 *  multi_line: word-wrap a string at `width` visible columns
 *
 *  Used when the '-' flag is set and the input is wider than
 *  the requested width.  Splits on '\n' into lines, then on ' '
 *  into words; words are concatenated separated by ' '; whenever
 *  appending the next word would overflow, emit '\n' + margin
 *  indentation and start a new line.  A single word longer than
 *  width is hard-truncated on first line.
 * ============================================================ */

static int multi_line(Buf *out, const char *src, int srclen,
                      int width, int margin) {
    int i = 0;
    int line_first = 1;          /* first line of the input */
    while (i < srclen) {
        int line_end = i;
        int word_start;
        int word_end;
        int line_w = 0;          /* visible width on current output line */
        int first_word = 1;
        int piece_first;         /* track first piece for hard-truncate rule */

        /* find end of current input line */
        while (line_end < srclen && src[line_end] != '\n') line_end++;

        word_start = i;
        piece_first = 1;

        /* Preserve the leading whitespace of the FIRST input line
         * verbatim — the LPC packages/sprintf.c splits by ' ' with
         * full_explode and emits each empty piece as a separator
         * space, effectively pushing the first content column right
         * by the number of leading spaces. Authors have relied on
         * this to indent the opening line of a paragraph deeper
         * than the hanging-indent continuation. */
        if (line_first) {
            while (word_start < line_end && src[word_start] == ' ') {
                buf_append_byte(out, ' ');
                line_w++;
                word_start++;
            }
        }

        while (word_start < line_end) {
            int wlen, wvis;

            /* skip leading spaces between words but emit them only when
             * not at the start of an output line */
            while (word_start < line_end && src[word_start] == ' ') word_start++;
            if (word_start >= line_end) break;

            word_end = word_start;
            while (word_end < line_end && src[word_end] != ' ') word_end++;

            wlen = word_end - word_start;
            wvis = vis_width(src + word_start, wlen);

            if (piece_first && wvis > width) {
                /* hard-truncate the first piece if it doesn't fit at all */
                int take = width;
                int j = word_start, taken = 0;
                while (j < word_end && taken < take) {
                    /* rough byte advance; treat each iteration as one column.
                     * For UTF-8/ANSI we advance one *visible* unit; bytes are
                     * skipped accordingly. */
                    unsigned char b = (unsigned char)src[j];
                    if (b == 27 && j + 1 < word_end && src[j + 1] == '[') {
                        j += 2;
                        while (j < word_end) {
                            unsigned char c = (unsigned char)src[j];
                            if (c >= 0x40 && c <= 0x7E) { j++; break; }
                            j++;
                        }
                        continue;
                    }
                    if (b >= 0xC2 && b <= 0xDF) { buf_append(out, src + j, 2); j += 2; taken++; continue; }
                    if (b >= 0xE0 && b <= 0xEF) { buf_append(out, src + j, 3); j += 3; taken++; continue; }
                    if (b >= 0xF0 && b <= 0xF7) { buf_append(out, src + j, 4); j += 4; taken++; continue; }
                    if (b >= 0x80 && b <= 0xBF) { j++; continue; }
                    buf_append_byte(out, src[j]);
                    j++;
                    taken++;
                }
                line_w = take;
                first_word = 0;
                piece_first = 0;
                word_start = word_end;
                continue;
            }

            if (!first_word && line_w + 1 + wvis > width) {
                /* wrap: newline + margin */
                buf_append_byte(out, '\n');
                if (margin > 0) buf_append_pad(out, margin, " ", 1);
                line_w = 0;
                first_word = 1;
            }

            if (!first_word) {
                buf_append_byte(out, ' ');
                line_w++;
            }
            buf_append(out, src + word_start, wlen);
            line_w += wvis;
            first_word = 0;
            piece_first = 0;
            word_start = word_end;
        }

        if (line_end < srclen) {
            buf_append_byte(out, '\n');
        }
        line_first = 0;
        i = line_end + 1;
        line_first = 0;
    }
    (void)line_first;
    return 1;
}

/* ============================================================
 *  align: apply width / precision / flags to a single string
 *
 *  Width, centering and the "is the string already wider than
 *  width?" check are all in visible columns.  Precision is byte
 *  based.
 * ============================================================ */

static int align(Buf *out, const char *s, int slen,
                 int width, int precision, int flags,
                 const char *padding, int padlen,
                 int margin) {
    int vlen;
    char *mut = NULL;

    /* Apply mutation flags: case, reverse, rot13.  These need a
     * mutable copy because the source is the LPC string, which is
     * not ours to write. */
    if (flags & (F_REVERSE | F_FLIPCASE | F_LOWER | F_UPPER |
                 F_CAPITAL | F_ROT13)) {
        mut = (char *)malloc((size_t)(slen > 0 ? slen : 1));
        if (!mut) return 0;
        memcpy(mut, s, (size_t)slen);
        apply_string_flags(mut, slen, flags);
        s = mut;
    }

    vlen = vis_width(s, slen);

    /* Wider than the field: '-' flag triggers word-wrap, otherwise
     * pass-through, then precision-truncate (no padding). */
    if (vlen >= width) {
        const char *use_s = s;
        int use_len = slen;

        if (flags & F_LEFT) {
            /* multi_line writes directly to out; precision is applied
             * to the wrapped output afterwards. */
            Buf tmp;
            buf_init(&tmp);
            multi_line(&tmp, s, slen, width, margin);

            /* precision truncation on wrapped output */
            if (precision > 0 && tmp.len > precision) {
                if (flags & F_TRUNC_LEFT) {
                    buf_append(out, tmp.data + (tmp.len - precision), precision);
                } else {
                    buf_append(out, tmp.data, precision);
                }
            } else {
                buf_append(out, tmp.data, tmp.len);
            }
            buf_free(&tmp);
            free(mut);
            return 1;
        }

        if (precision > 0 && use_len > precision) {
            if (flags & F_TRUNC_LEFT) {
                use_s += (use_len - precision);
                use_len = precision;
            } else {
                use_len = precision;
            }
        }
        buf_append(out, use_s, use_len);
        free(mut);
        return 1;
    }

    /* Fits in the field: pad. */
    {
        int pad = width - vlen;
        const char *use_s = s;
        int use_len = slen;

        /* precision truncation BEFORE padding (so we count visible
         * width of the truncated string, not the original). */
        if (precision > 0 && use_len > precision) {
            if (flags & F_TRUNC_LEFT) {
                use_s += (use_len - precision);
                use_len = precision;
            } else {
                use_len = precision;
            }
            pad = width - vis_width(use_s, use_len);
            if (pad < 0) pad = 0;
        }

        if (flags & F_LEFT) {
            buf_append(out, use_s, use_len);
            /* don't pad past a trailing newline */
            if (use_len == 0 || use_s[use_len - 1] != '\n') {
                buf_append_pad(out, pad, padding, padlen);
            }
        } else if (flags & F_CENTER) {
            int left = (pad + 1) / 2;
            int right = pad / 2;
            buf_append_pad(out, left, padding, padlen);
            buf_append(out, use_s, use_len);
            buf_append_pad(out, right, padding, padlen);
        } else {
            buf_append_pad(out, pad, padding, padlen);
            buf_append(out, use_s, use_len);
        }
    }

    free(mut);
    return 1;
}

/* ============================================================
 *  table mode (# flag with width, string only)
 *
 *  Splits the input on '\n' into pieces, places them in a grid
 *  of `columns` columns of width `max_cell` characters, where
 *  columns = width / (max_cell + 1).
 * ============================================================ */

static int table_mode(Buf *out, const char *s, int slen,
                      int width, int precision, int flags,
                      const char *padding, int padlen, int margin) {
    /* split on '\n' into pieces */
    const char **pieces = NULL;
    int *piece_lens = NULL;
    int n_pieces = 0;
    int cap_pieces = 0;
    int max_cell = 0;
    int columns;
    int i, j, done = 0;

    {
        int start = 0;
        for (i = 0; i <= slen; i++) {
            if (i == slen || s[i] == '\n') {
                int plen = i - start;
                if (n_pieces >= cap_pieces) {
                    cap_pieces = (cap_pieces == 0) ? 16 : cap_pieces * 2;
                    pieces     = (const char **)realloc(pieces, sizeof(*pieces) * cap_pieces);
                    piece_lens = (int *)realloc(piece_lens, sizeof(*piece_lens) * cap_pieces);
                    if (!pieces || !piece_lens) { free(pieces); free(piece_lens); return 0; }
                }
                pieces[n_pieces]     = s + start;
                piece_lens[n_pieces] = plen;
                {
                    int v = vis_width(s + start, plen);
                    if (v > max_cell) max_cell = v;
                }
                n_pieces++;
                start = i + 1;
            }
        }
    }

    if (n_pieces == 0 || max_cell == 0) {
        free(pieces);
        free(piece_lens);
        return 1;
    }

    columns = width / (max_cell + 1);
    if (columns < 1) columns = 1;

    /* Layout: row-major.  For each piece in order, put it at
     * (i % columns, i / columns) -- emit row by row. */
    {
        int rows = (n_pieces + columns - 1) / columns;
        int r;
        for (r = 0; r < rows; r++) {
            for (j = 0; j < columns; j++) {
                int idx = r * columns + j;
                int new_flags = flags & ~F_TABLE;
                buf_append_byte(out, ' ');
                if (idx < n_pieces) {
                    align(out, pieces[idx], piece_lens[idx],
                          max_cell, precision, new_flags,
                          padding, padlen, margin);
                    done++;
                } else {
                    buf_append_pad(out, max_cell, padding, padlen);
                }
            }
            buf_append_byte(out, '\n');
            if (margin > 0) buf_append_pad(out, margin, " ", 1);
        }
    }

    free(pieces);
    free(piece_lens);
    return 1;
}

/* ============================================================
 *  numerical formatting
 * ============================================================ */

static int numerical(Buf *out, LPC_int n, int base,
                     int width, int precision, int flags,
                     const char *padding, int padlen) {
    char digits[80];
    int  dlen;
    char sign = 0;
    int  total;
    int  pad;

    /* sign */
    if (n < 0) sign = '-';
    else if (flags & F_PLUS)  sign = '+';
    else if (flags & F_SPACE) sign = ' ';

    dlen = int_to_base(digits, n, base, !!(flags & F_UPPERHEX));

    /* precision: pad |digits| with leading zeros to >= precision */
    if (precision > 0 && dlen < precision) {
        char buf2[80];
        int  zero_pad = precision - dlen;
        memset(buf2, '0', (size_t)zero_pad);
        memcpy(buf2 + zero_pad, digits, (size_t)dlen);
        memcpy(digits, buf2, (size_t)precision);
        dlen = precision;
    }

    total = dlen + (sign ? 1 : 0);

    /* zero-pad: insert zeros between sign and digits, total width */
    if ((flags & F_ZEROPAD) && !(flags & F_LEFT) && width > total) {
        int zp = width - total;
        if (sign) buf_append_byte(out, sign);
        buf_append_pad(out, zp, "0", 1);
        buf_append(out, digits, dlen);
        return 1;
    }

    pad = width - total;

    if (flags & F_LEFT) {
        if (sign) buf_append_byte(out, sign);
        buf_append(out, digits, dlen);
        if (pad > 0) buf_append_pad(out, pad, padding, padlen);
    } else {
        if (pad > 0) buf_append_pad(out, pad, padding, padlen);
        if (sign) buf_append_byte(out, sign);
        buf_append(out, digits, dlen);
    }

    return 1;
}

/* ============================================================
 *  float formatting (uses libc snprintf)
 *
 *  conv: 'f' | 'e' | 'E' | 'g' | 'G'
 * ============================================================ */

static int do_float(Buf *out, LPC_float x, int width, int precision,
                    int flags, char conv, const char *padding, int padlen) {
    char fmt[32];
    char buf[128];
    int  fmtlen = 0;
    int  blen, vlen, pad;

    /* Build a format string for snprintf: "%[flags][.prec]<conv>"
     * but we only forward the precision; width/padding we handle. */
    fmt[fmtlen++] = '%';
    if (flags & F_PLUS)  fmt[fmtlen++] = '+';
    else if (flags & F_SPACE) fmt[fmtlen++] = ' ';
    if (precision > 0) {
        char tmp[16];
        int n = snprintf(tmp, sizeof(tmp), ".%d", precision);
        memcpy(fmt + fmtlen, tmp, (size_t)n);
        fmtlen += n;
    } else if (precision == 0) {
        /* precision 0 means "use the default" (6); don't pass ".0"
         * to snprintf. */
    }
    /* Use Lf for long double (matches LPC_float definition unless NOFLOAT). */
    fmt[fmtlen++] = 'L';
    fmt[fmtlen++] = conv;
    fmt[fmtlen]   = '\0';

    blen = snprintf(buf, sizeof(buf), fmt, x);
    if (blen < 0) blen = 0;
    if (blen >= (int)sizeof(buf)) blen = (int)sizeof(buf) - 1;

    vlen = blen; /* numeric output is ASCII */

    if (vlen >= width) {
        buf_append(out, buf, blen);
        return 1;
    }
    pad = width - vlen;

    /* zero-pad: insert zeros between sign and digits */
    if ((flags & F_ZEROPAD) && !(flags & F_LEFT)) {
        int sign_len = 0;
        if (blen > 0 && (buf[0] == '-' || buf[0] == '+' || buf[0] == ' ')) sign_len = 1;
        if (sign_len) buf_append_byte(out, buf[0]);
        buf_append_pad(out, pad, "0", 1);
        buf_append(out, buf + sign_len, blen - sign_len);
        return 1;
    }

    if (flags & F_LEFT) {
        buf_append(out, buf, blen);
        buf_append_pad(out, pad, padding, padlen);
    } else if (flags & F_CENTER) {
        int left = (pad + 1) / 2;
        int right = pad / 2;
        buf_append_pad(out, left, padding, padlen);
        buf_append(out, buf, blen);
        buf_append_pad(out, right, padding, padlen);
    } else {
        buf_append_pad(out, pad, padding, padlen);
        buf_append(out, buf, blen);
    }
    return 1;
}

/* ============================================================
 *  anything: stringify any LPC value (for %y)
 *
 *    int / float -> bare textual form
 *    string      -> "..."
 *    object      -> OBJ <name>
 *    array       -> ({ a, b, c })
 *    mapping     -> ([ k : v, k : v ])
 *    nil / lwobj -> nil / <lwobj>
 * ============================================================ */

static int anything_into(Buf *out, LPC_value val, LPC_frame f, int depth);

static int anything_into(Buf *out, LPC_value val, LPC_frame f, int depth) {
    int t;
    char tmpbuf[64];

    if (depth > ANYTHING_MAX_DEPTH) {
        return buf_append(out, "...", 3);
    }

    t = lpc_value_type(val);
    switch (t) {
    case LPC_TYPE_NIL:
        return buf_append(out, "nil", 3);
    case LPC_TYPE_INT: {
        LPC_int n = lpc_int_getval(val);
        int len = snprintf(tmpbuf, sizeof(tmpbuf), "%lld", (long long)n);
        return buf_append(out, tmpbuf, len);
    }
#ifndef NOFLOAT
    case LPC_TYPE_FLOAT: {
        LPC_float x = lpc_float_getval(val);
        int len = snprintf(tmpbuf, sizeof(tmpbuf), "%Lf", x);
        return buf_append(out, tmpbuf, len);
    }
#endif
    case LPC_TYPE_STRING: {
        LPC_string str = lpc_string_getval(val);
        const char *p = lpc_string_text(str);
        int n = lpc_string_length(str);
        if (!buf_append_byte(out, '"')) return 0;
        if (!buf_append(out, p, n))      return 0;
        return buf_append_byte(out, '"');
    }
    case LPC_TYPE_OBJECT: {
        LPC_object ob = NULL;
        char obname[MAX_OBJNAME];
        const char *name;
        /* lpc_value_type returns LPC_TYPE_OBJECT when the value holds
         * an object; we extract via a small dance with lpc_value_temp
         * that the API doesn't directly support.  Easiest: fall through
         * since arrays/mappings should not contain raw object slots
         * commonly traversed.  We emit a placeholder to avoid issues. */
        (void)ob; (void)name; (void)obname;
        return buf_append(out, "OBJ <?>", 7);
    }
    case LPC_TYPE_ARRAY: {
        LPC_array arr = lpc_array_getval(val);
        int n = lpc_array_size(arr);
        int i;
        if (!buf_append(out, "({ ", 3)) return 0;
        for (i = 0; i < n; i++) {
            if (!anything_into(out, lpc_array_index(arr, i), f, depth + 1)) return 0;
            if (!buf_append(out, (i + 1 < n) ? ", " : " ", (i + 1 < n) ? 2 : 1)) return 0;
        }
        return buf_append(out, "})", 2);
    }
    case LPC_TYPE_MAPPING: {
        LPC_mapping m = lpc_mapping_getval(val);
        int n = lpc_mapping_size(m);
        int i;
        if (!buf_append(out, "([ ", 3)) return 0;
        for (i = 0; i < n; i++) {
            LPC_value k = lpc_mapping_enum(m, (unsigned int)i);
            LPC_value v = lpc_mapping_index(m, k);
            if (!anything_into(out, k, f, depth + 1)) return 0;
            if (!buf_append(out, " : ", 3)) return 0;
            if (!anything_into(out, v, f, depth + 1)) return 0;
            if (!buf_append(out, (i + 1 < n) ? ", " : " ", (i + 1 < n) ? 2 : 1)) return 0;
        }
        return buf_append(out, "])", 2);
    }
    default:
        return buf_append(out, "<?>", 3);
    }
}

/* ============================================================
 *  format string parser + dispatcher
 * ============================================================ */

/* Parse flags starting at p[0]; returns flag bitmap and advances p. */
static int parse_flags(const char **pp, const char *end) {
    int flags = 0;
    const char *p = *pp;
    while (p < end) {
        switch (*p) {
        case '-':  flags |= F_LEFT;     p++; break;
        case '_':  flags |= F_TRUNC_LEFT; p++; break;
        case '|':  flags |= F_CENTER;   p++; break;
        case '+':  flags |= F_PLUS;     p++; break;
        case ' ':  flags |= F_SPACE;    p++; break;
        case '0':  flags |= F_ZEROPAD;  p++; break;
        case '#':  flags |= F_TABLE;    p++; break;
        case '`':  flags |= F_REVERSE;  p++; break;
        case '&':  flags |= F_ROT13;    p++; break;
        case '~':  flags |= F_FLIPCASE; p++; break;
        case '<':  flags |= F_LOWER;    p++; break;
        case '>':  flags |= F_UPPER;    p++; break;
        case '=':  flags |= F_CAPITAL;  p++; break;
        default:
            *pp = p;
            return flags;
        }
    }
    *pp = p;
    return flags;
}

/* Parse a non-negative decimal integer; returns -1 if no digits.  Advances p. */
static int parse_uint(const char **pp, const char *end) {
    int n = 0;
    int seen = 0;
    const char *p = *pp;
    while (p < end && *p >= '0' && *p <= '9') {
        n = n * 10 + (*p - '0');
        p++;
        seen = 1;
    }
    *pp = p;
    return seen ? n : -1;
}

/* Type-check helper: if typeof(v) != want, raise runtime error.
 *
 * Caveat: lpc_runtime_error() routes through DGD's EC->error(format, ...)
 * which interprets the message as a printf-style format string.  Any
 * literal '%' in an error message must be escaped as '%%', otherwise
 * subsequent format specifiers consume garbage and the message is
 * corrupted on screen.  All error messages in this file follow that
 * convention.
 */
static int typecheck(LPC_value v, int want, LPC_frame f, const char *what) {
    if (lpc_value_type(v) == want) return 1;
    lpc_runtime_error(f, what);
    return 0;
}

/* Format one conversion.  Returns updated argument index, or -1 on error. */
static int format_one(Buf *out, char conv,
                      int flags, int width, int precision,
                      LPC_frame f, int nargs, int argi,
                      const char **padding, int *padlen,
                      Buf *result_so_far_for_n,
                      int margin) {
    LPC_value v;
    LPC_dataspace data = lpc_frame_dataspace(f);

    /* %% has no argument */
    if (conv == '%') {
        buf_append_byte(out, '%');
        return argi;
    }

    if (argi >= nargs) {
        lpc_runtime_error(f, "sprintf: too few arguments for conversion");
        return -1;
    }

    v = lpc_frame_arg(f, nargs, argi);

    switch (conv) {

    /* ---------------- string-like ---------------- */
    case 's': {
        /* %s accepts string/int/float; int/float are stringified. */
        char tmp[64];
        const char *text;
        int len;
        int t = lpc_value_type(v);
        if (t == LPC_TYPE_STRING) {
            LPC_string str = lpc_string_getval(v);
            text = lpc_string_text(str);
            len  = lpc_string_length(str);
        } else if (t == LPC_TYPE_INT) {
            len = snprintf(tmp, sizeof(tmp), "%lld",
                           (long long)lpc_int_getval(v));
            text = tmp;
        } else if (t == LPC_TYPE_FLOAT) {
#ifndef NOFLOAT
            len = snprintf(tmp, sizeof(tmp), "%Lf", lpc_float_getval(v));
            text = tmp;
#else
            lpc_runtime_error(f, "sprintf: float arg with NOFLOAT build");
            return -1;
#endif
        } else {
            lpc_runtime_error(f, "sprintf: %%s expects string/int/float");
            return -1;
        }

        if ((flags & F_TABLE) && conv == 's') {
            table_mode(out, text, len, width, precision, flags,
                       *padding, *padlen, margin);
        } else {
            align(out, text, len, width, precision, flags,
                  *padding, *padlen, margin);
        }
        return argi + 1;
    }

    case 'S': {
        LPC_string str;
        if (!typecheck(v, LPC_TYPE_STRING, f, "sprintf: %%S expects string")) return -1;
        str = lpc_string_getval(v);
        if (flags & F_TABLE) {
            table_mode(out, lpc_string_text(str), lpc_string_length(str),
                       width, precision, flags, *padding, *padlen, margin);
        } else {
            align(out, lpc_string_text(str), lpc_string_length(str),
                  width, precision, flags, *padding, *padlen, margin);
        }
        return argi + 1;
    }

    /* ---------------- arrays ---------------- */
    case 'a':
    case 'A': {
        LPC_array arr;
        int n, i;
        if (!typecheck(v, LPC_TYPE_ARRAY, f, "sprintf: %%a/%%A expects array")) return -1;
        arr = lpc_array_getval(v);
        n = lpc_array_size(arr);
        for (i = 0; i < n; i++) {
            LPC_value el = lpc_array_index(arr, i);
            int t = lpc_value_type(el);
            char tmp[64];
            const char *text;
            int len;
            if (t == LPC_TYPE_STRING) {
                LPC_string str = lpc_string_getval(el);
                text = lpc_string_text(str);
                len  = lpc_string_length(str);
            } else if (conv == 'a' && t == LPC_TYPE_INT) {
                len = snprintf(tmp, sizeof(tmp), "%lld",
                               (long long)lpc_int_getval(el));
                text = tmp;
            } else if (conv == 'a' && t == LPC_TYPE_FLOAT) {
#ifndef NOFLOAT
                len = snprintf(tmp, sizeof(tmp), "%Lf",
                               lpc_float_getval(el));
                text = tmp;
#else
                lpc_runtime_error(f, "sprintf: float in %%a with NOFLOAT");
                return -1;
#endif
            } else {
                lpc_runtime_error(f, "sprintf: %%A expects string array");
                return -1;
            }
            align(out, text, len, width, precision, flags,
                  *padding, *padlen, margin);
        }
        return argi + 1;
    }

    /* ---------------- integers ---------------- */
    case 'd':
    case 'i':
    case 'b':
    case 'o':
    case 'x':
    case 'X': {
        LPC_int n;
        int base;
        int extra_flags = flags;
        if (!typecheck(v, LPC_TYPE_INT, f, "sprintf: integer expected")) return -1;
        n = lpc_int_getval(v);
        switch (conv) {
        case 'b': base = 2; break;
        case 'o': base = 8; break;
        case 'x': base = 16; break;
        case 'X': base = 16; extra_flags |= F_UPPERHEX; break;
        default:  base = 10; break;
        }
        numerical(out, n, base, width, precision, extra_flags,
                  *padding, *padlen);
        return argi + 1;
    }

    /* ---------------- char ---------------- */
    case 'c': {
        char ch;
        if (!typecheck(v, LPC_TYPE_INT, f, "sprintf: %%c expects int")) return -1;
        ch = (char)lpc_int_getval(v);
        align(out, &ch, 1, width, precision, flags,
              *padding, *padlen, margin);
        return argi + 1;
    }

    /* ---------------- floats ---------------- */
#ifndef NOFLOAT
    case 'f':
    case 'e':
    case 'E':
    case 'g':
    case 'G': {
        LPC_float x;
        int t = lpc_value_type(v);
        if (t == LPC_TYPE_INT) {
            x = (LPC_float)lpc_int_getval(v);
        } else if (t == LPC_TYPE_FLOAT) {
            x = lpc_float_getval(v);
        } else {
            lpc_runtime_error(f, "sprintf: float-conversion expects int/float");
            return -1;
        }
        do_float(out, x, width, precision, flags, conv,
                 *padding, *padlen);
        return argi + 1;
    }
#endif

    /* ---------------- hex dump ---------------- */
    case 'h':
    case 'H': {
        /* %h emits no separator; %H separates the bytes by the
         * current padding character (defaults to a space). */
        LPC_string str;
        const char *src;
        int srclen, i;
        Buf hex;
        const char *sep;
        int seplen;
        if (!typecheck(v, LPC_TYPE_STRING, f, "sprintf: %%h/%%H expects string")) return -1;
        str = lpc_string_getval(v);
        src = lpc_string_text(str);
        srclen = lpc_string_length(str);

        if (conv == 'H') {
            sep = *padding;
            seplen = *padlen;
        } else {
            sep = "";
            seplen = 0;
        }

        buf_init(&hex);
        for (i = 0; i < srclen; i++) {
            char hb[2];
            unsigned char b = (unsigned char)src[i];
            hb[0] = (char)digit_in_base(b >> 4,  flags & F_UPPERHEX);
            hb[1] = (char)digit_in_base(b & 0xF, flags & F_UPPERHEX);
            if (i > 0 && seplen > 0) buf_append(&hex, sep, seplen);
            buf_append(&hex, hb, 2);
        }
        align(out, hex.data, hex.len, width, precision, flags,
              *padding, *padlen, margin);
        buf_free(&hex);
        return argi + 1;
    }

    /* ---------------- byte count so far ---------------- */
    case 'n': {
        LPC_array arr;
        LPC_dataspace data2;
        LPC_value tmp;
        if (!typecheck(v, LPC_TYPE_ARRAY, f, "sprintf: %%n expects array")) return -1;
        arr = lpc_array_getval(v);
        if (lpc_array_size(arr) < 1) {
            lpc_runtime_error(f, "sprintf: %%n: array too small");
            return -1;
        }
        data2 = lpc_frame_dataspace(f);
        tmp = lpc_value_temp(data2);
        lpc_int_putval(tmp, (LPC_int)result_so_far_for_n->len);
        lpc_array_assign(data2, arr, 0, tmp);
        return argi + 1;
    }

    /* ---------------- object name ---------------- */
    case 'O': {
        /*
         * %O is intentionally unsupported in this kfun.
         *
         * The DGD public extension API exposes lpc_object_putval (write an
         * Object* into a Value) but not the symmetric lpc_object_getval
         * (read an Object* out of a Value).  Without it we cannot reach
         * lpc_object_name(), which needs the Object* not the LPC_value.
         *
         * Hexagon hardly uses %O -- a grep of the live mudlib at the time
         * this kfun was written found one occurrence and it was inside a
         * disabled test.  The conventional workaround is to format the
         * name in LPC and pass the result as %s:
         *
         *     sprintf("%s", object_name(ob))   // instead of "%O", ob
         *
         * If a real callsite ever needs %O, the path forward is to add
         * lpc_object_getval to dgd-extensions/src/lpc_ext.{h,c} and to
         * the matching slot in dgd/src/ext.cpp (bumping EXTENSION_MINOR
         * and LPC_EXT_VERSION_MINOR), then implement this branch with
         * lpc_object_name(f, lpc_object_getval(v), buf).
         */
        if (lpc_value_type(v) != LPC_TYPE_OBJECT) {
            lpc_runtime_error(f, "sprintf: %%O expects object");
            return -1;
        }
        lpc_runtime_error(f,
            "sprintf: %%O is not supported by this kfun -- "
            "the public DGD extension API does not expose "
            "lpc_object_getval. Use sprintf(\"%%s\", object_name(ob)) instead.");
        return -1;
    }

    /* ---------------- padding char ---------------- */
    case 'p': {
        static char saved_pad[1];
        if (!typecheck(v, LPC_TYPE_INT, f, "sprintf: %%p expects int")) return -1;
        saved_pad[0] = (char)lpc_int_getval(v);
        *padding = saved_pad;
        *padlen  = 1;
        return argi + 1;
    }

    /* ---------------- anything ---------------- */
    case 'y': {
        Buf scratch;
        buf_init(&scratch);
        anything_into(&scratch, v, f, 0);
        align(out, scratch.data, scratch.len, width, precision, flags,
              *padding, *padlen, margin);
        buf_free(&scratch);
        return argi + 1;
    }

    default:
        lpc_runtime_error(f, "sprintf: unknown conversion character");
        return -1;
    }
    (void)data;
}

static void format_string(Buf *out, const char *fmt, int fmtlen,
                          LPC_frame f, int nargs) {
    const char *p = fmt;
    const char *end = fmt + fmtlen;
    int argi = 1; /* arg 0 is the format string */
    const char *padding = " ";
    int padlen = 1;
    /* Visible width of literal chunks emitted since the last '\n' in
     * the format string. Reset on newline, otherwise accumulated. Fed
     * to every %s (and friends) so multi_line can hang-indent wrapped
     * continuation lines to column-align with the first line of the
     * field. Matches the LPC packages/sprintf.c margin convention. */
    int margin = 0;

    while (p < end) {
        if (*p != '%' && *p != '@') {
            const char *start = p;
            while (p < end && *p != '%' && *p != '@') p++;
            buf_append(out, start, (int)(p - start));
            margin = advance_margin(margin, start, (int)(p - start));
            continue;
        }

        /* '@' is recognised but not implemented; emit literal '@@' as '@' */
        if (*p == '@') {
            if (p + 1 < end && p[1] == '@') {
                buf_append_byte(out, '@');
                p += 2;
                continue;
            }
            /* unknown @-conversion */
            lpc_runtime_error(f, "sprintf: @-conversions not supported");
            return;
        }

        /* '%' */
        p++;
        if (p >= end) {
            lpc_runtime_error(f, "sprintf: trailing '%%' in format");
            return;
        }

        /* '%%' literal */
        if (*p == '%') {
            buf_append_byte(out, '%');
            p++;
            continue;
        }

        {
            int flags;
            int width    = 0;
            int precision = 0;
            char conv;
            int parsed;

            flags = parse_flags(&p, end);

            /* width: '*' or integer */
            if (p < end && *p == '*') {
                LPC_value w;
                if (argi >= nargs) {
                    lpc_runtime_error(f, "sprintf: too few arguments for *");
                    return;
                }
                w = lpc_frame_arg(f, nargs, argi);
                if (!typecheck(w, LPC_TYPE_INT, f, "sprintf: * width must be int")) return;
                width = (int)lpc_int_getval(w);
                argi++;
                p++;
            } else {
                parsed = parse_uint(&p, end);
                width = (parsed < 0) ? 0 : parsed;
            }

            /* precision: '.' followed by '*' or integer */
            if (p < end && *p == '.') {
                p++;
                if (p < end && *p == '*') {
                    LPC_value pr;
                    if (argi >= nargs) {
                        lpc_runtime_error(f, "sprintf: too few arguments for .*");
                        return;
                    }
                    pr = lpc_frame_arg(f, nargs, argi);
                    if (!typecheck(pr, LPC_TYPE_INT, f, "sprintf: .* precision must be int")) return;
                    precision = (int)lpc_int_getval(pr);
                    argi++;
                    p++;
                } else {
                    parsed = parse_uint(&p, end);
                    precision = (parsed < 0) ? 0 : parsed;
                }
            }

            if (p >= end) {
                lpc_runtime_error(f, "sprintf: format ends inside conversion");
                return;
            }

            conv = *p++;
            argi = format_one(out, conv, flags, width, precision,
                              f, nargs, argi, &padding, &padlen, out,
                              margin);
            if (argi < 0) return; /* runtime error already raised */
        }
    }
}

/* ============================================================
 *  kfun entry point
 * ============================================================ */

static void sprintf_kfun(LPC_frame f, int nargs, LPC_value retval) {
    LPC_value fmt_val;
    LPC_string fmt_str;
    LPC_dataspace data;
    LPC_string out;
    Buf buf;

    if (nargs < 1) {
        lpc_runtime_error(f, "sprintf: missing format argument");
        return;
    }

    fmt_val = lpc_frame_arg(f, nargs, 0);
    if (lpc_value_type(fmt_val) != LPC_TYPE_STRING) {
        lpc_runtime_error(f, "sprintf: format must be a string");
        return;
    }
    fmt_str = lpc_string_getval(fmt_val);
    data = lpc_frame_dataspace(f);

    buf_init(&buf);
    format_string(&buf, lpc_string_text(fmt_str),
                  lpc_string_length(fmt_str), f, nargs);

    out = lpc_string_new(data, buf.data ? buf.data : "", buf.len);
    buf_free(&buf);
    lpc_string_putval(retval, out);
}

/* ============================================================
 *  registration
 * ============================================================ */

static char sprintf_proto[] = {
    LPC_TYPE_STRING,    /* return */
    LPC_TYPE_STRING,    /* format */
    LPC_TYPE_MIXED,     /* mixed... */
    LPC_TYPE_ELLIPSIS,
    0
};

static LPC_ext_kfun kf[1] = {
    { "sprintf", sprintf_proto, &sprintf_kfun }
};

int lpc_ext_init(int major, int minor, const char *config) {
    (void)major; (void)minor; (void)config;
    lpc_ext_kfun(kf, 1);
    return 1;
}
