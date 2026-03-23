#define _GNU_SOURCE
#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <ctype.h>
//#define DEBUG_SANITIZE
/* -------------------------------------------------------------------------
 * Helpers
 * ------------------------------------------------------------------------- */

/* Trim leading whitespace from a string view (does not allocate). */
static const char *ltrim(const char *s)
{
    while (*s && isspace((unsigned char)*s)) ++s;
    return s;
}

/*
 * Returns 1 when the trimmed line is the preprocessor directive `kw`.
 * Handles optional whitespace between '#' and the keyword ("# extension").
 */
static int directive_match(const char *trimmed, const char *kw)
{
    if (*trimmed != '#') return 0;
    const char *p = ltrim(trimmed + 1);
    size_t klen = strlen(kw);
    if (strncmp(p, kw, klen) != 0) return 0;
    return !p[klen] || isspace((unsigned char)p[klen]);
}

/* -------------------------------------------------------------------------
 * Line counting / splitting
 *
 * Both functions use the same rule so the count is always exact:
 *   - Each '\n' ends a line (the '\n' is included in that line's length).
 *   - A trailing '\n' does NOT produce a spurious empty final entry.
 *   - Source not ending in '\n' produces one final entry without one.
 * ------------------------------------------------------------------------- */
static size_t count_lines(const char *src, size_t src_len)
{
    if (!src_len) return 0;
    size_t n = 0;
    for (size_t i = 0; i < src_len; ++i)
        if (src[i] == '\n') ++n;
        /* unterminated final line */
        if (src[src_len - 1] != '\n') ++n;
        return n;
}

/*
 * Fill caller-allocated arrays lines[]/llens[] (each of size >= count_lines()).
 * Returns the number of entries written (== count_lines() for same input).
 */
static size_t split_lines(const char *src, size_t src_len,
                          const char **lines, size_t *llens)
{
    size_t li = 0, start = 0;
    for (size_t i = 0; i < src_len; ++i) {
        if (src[i] == '\n') {
            lines[li]  = src + start;
            llens[li]  = i + 1 - start;   /* include the '\n' */
            ++li;
            start = i + 1;
        }
    }
    /* unterminated final fragment */
    if (start < src_len) {
        lines[li]  = src + start;
        llens[li]  = src_len - start;
        ++li;
    }
    return li;
}

/* -------------------------------------------------------------------------
 * Dynamic string builder
 * ------------------------------------------------------------------------- */
typedef struct { char *buf; size_t len, cap; } Strbuf;

static int sb_reserve(Strbuf *sb, size_t extra)
{
    if (sb->len + extra + 1 <= sb->cap) return 1;
    size_t ncap = (sb->len + extra + 1) * 2;
    if (ncap < 256) ncap = 256;
    char *nb = realloc(sb->buf, ncap);
    if (!nb) return 0;
    sb->buf = nb;
    sb->cap = ncap;
    return 1;
}

static int sb_append(Strbuf *sb, const char *s, size_t n)
{
    if (!sb_reserve(sb, n)) return 0;
    memcpy(sb->buf + sb->len, s, n);
    sb->len += n;
    sb->buf[sb->len] = '\0';
    return 1;
}

static int sb_appends(Strbuf *sb, const char *s)
{
    return sb_append(sb, s, strlen(s));
}

/* -------------------------------------------------------------------------
 * sanitize_shader
 *
 * Takes a joined shader source, returns a newly malloc'd rewritten copy,
 * or NULL if no rewrite is needed / on allocation failure.
 * Caller must free() a non-NULL result.
 * ------------------------------------------------------------------------- */
static char *sanitize_shader(const char *src, size_t src_len)
{
    if (!src_len) return NULL;

    /* ── 1. Split into lines ─────────────────────────────────────────────── */
    size_t nlines = count_lines(src, src_len);
    if (!nlines) return NULL;

    const char **lines = malloc(nlines * sizeof *lines);
    size_t      *llens = malloc(nlines * sizeof *llens);
    if (!lines || !llens) { free(lines); free(llens); return NULL; }

    nlines = split_lines(src, src_len, lines, llens);   /* exact, no overflow */

    /* ── 2. Classify lines ───────────────────────────────────────────────── */
    int    version_idx = -1;
    size_t next        = 0;

    size_t *ext_idx = malloc(nlines * sizeof *ext_idx);
    if (!ext_idx) { free(lines); free(llens); return NULL; }

    for (size_t i = 0; i < nlines; ++i) {
        size_t rlen = llens[i];
        while (rlen > 0 &&
            (lines[i][rlen-1] == '\n' || lines[i][rlen-1] == '\r'))
            --rlen;

        char *tmp = malloc(rlen + 1);
        if (!tmp) {
            free(ext_idx); free(lines); free(llens); return NULL;
        }
        memcpy(tmp, lines[i], rlen);
        tmp[rlen] = '\0';

        const char *t = ltrim(tmp);

        if (version_idx < 0 && directive_match(t, "version"))
            version_idx = (int)i;
        else if (directive_match(t, "extension"))
            ext_idx[next++] = i;

        free(tmp);
    }

    /* ── 3. Decide whether a rewrite is needed ───────────────────────────── */
    /*
     * A rewrite is needed only when a #extension directive appears after a
     * "real code" line (non-blank, non-comment, non-preprocessor) that itself
     * follows #version.  An extension that already lives in the preamble block
     * right after #version is fine and should not trigger an unnecessary copy.
     */
    int needs_rewrite = 0;

    if (version_idx >= 0 && next > 0) {
        /* Find the index of the first real-code line after #version. */
        int first_code_idx = -1;
        for (size_t i = (size_t)(version_idx + 1);
             i < nlines && first_code_idx < 0; ++i)
             {
                 size_t rlen = llens[i];
                 while (rlen > 0 &&
                     (lines[i][rlen-1] == '\n' || lines[i][rlen-1] == '\r'))
                     --rlen;

                 char *tmp = malloc(rlen + 1);
                 if (!tmp) {
                     free(ext_idx); free(lines); free(llens); return NULL;
                 }
                 memcpy(tmp, lines[i], rlen);
                 tmp[rlen] = '\0';
                 const char *t = ltrim(tmp);

                 /* Blank, preprocessor (#), or line-comment (//) → not real code */
                 int is_code = *t != '\0'
                 && *t != '#'
                 && !(t[0] == '/' && t[1] == '/')
                 && !(t[0] == '/' && t[1] == '*');
                 if (is_code)
                     first_code_idx = (int)i;

                 free(tmp);
             }

             /* If a real-code line exists, any extension appearing after it is
              * mid-shader and must be hoisted. */
             if (first_code_idx >= 0) {
                 for (size_t k = 0; k < next; ++k) {
                     if ((int)ext_idx[k] > first_code_idx) {
                         needs_rewrite = 1;
                         break;
                     }
                 }
             }
    }

    if (!needs_rewrite) {
        free(ext_idx); free(lines); free(llens);
        return NULL;
    }

    /* ── 4. Build rewritten shader ───────────────────────────────────────── */
    /*
     * a) lines[0 .. version_idx]  (everything up to and including #version)
     * b) blank line
     * c) hoisted #extension lines (post-version only, deduplicated)
     * d) blank line
     * e) lines[version_idx+1 .. nlines-1], skipping #extension slots
     *    (or blanking them with -DKEEP_LINENOS)
     */
    Strbuf sb = {0};

    /* a) preamble through #version */
    for (int i = 0; i <= version_idx; ++i)
        if (!sb_append(&sb, lines[i], llens[i])) goto oom;

        /* ensure trailing newline after #version */
        if (sb.len > 0 && sb.buf[sb.len - 1] != '\n')
            if (!sb_appends(&sb, "\n")) goto oom;

            /* b) blank separator */
            if (!sb_appends(&sb, "\n")) goto oom;

            /* c) hoisted extensions */
            for (size_t k = 0; k < next; ++k) {
                size_t ei = ext_idx[k];
                if ((int)ei <= version_idx) continue;   /* already in the preamble */

                    /* deduplicate by raw text */
                    int dup = 0;
                for (size_t j = 0; j < k && !dup; ++j) {
                    if ((int)ext_idx[j] <= version_idx) continue;
                    if (llens[ext_idx[j]] == llens[ei] &&
                        memcmp(lines[ext_idx[j]], lines[ei], llens[ei]) == 0)
                        dup = 1;
                }
                if (dup) continue;

                if (!sb_append(&sb, lines[ei], llens[ei])) goto oom;
                if (llens[ei] == 0 || lines[ei][llens[ei]-1] != '\n')
                    if (!sb_appends(&sb, "\n")) goto oom;
            }

            /* d) blank separator */
            if (!sb_appends(&sb, "\n")) goto oom;

            /* e) body, skipping original #extension slots */
            for (size_t i = (size_t)(version_idx + 1); i < nlines; ++i) {
                int is_ext = 0;
                for (size_t k = 0; k < next && !is_ext; ++k)
                    if (ext_idx[k] == i) is_ext = 1;

                    if (is_ext) {
                        #ifdef KEEP_LINENOS
                        if (!sb_appends(&sb, "\n")) goto oom;
                        #endif
                        continue;
                    }
                    if (!sb_append(&sb, lines[i], llens[i])) goto oom;
            }

            free(ext_idx); free(lines); free(llens);
            return sb.buf;   /* caller must free() */

            oom:
            free(sb.buf);
            free(ext_idx); free(lines); free(llens);
            return NULL;
}

/* -------------------------------------------------------------------------
 * Join multi-chunk glShaderSource input into one malloc'd buffer.
 * ------------------------------------------------------------------------- */
static char *join_source(GLsizei count, const GLchar *const *strings,
                         const GLint *length, size_t *out_len)
{
    size_t total = 0;
    for (GLsizei i = 0; i < count; ++i) {
        if (!strings[i]) continue;
        total += (length && length[i] >= 0)
        ? (size_t)length[i]
        : strlen(strings[i]);
    }

    char *buf = malloc(total + 1);
    if (!buf) return NULL;

    size_t pos = 0;
    for (GLsizei i = 0; i < count; ++i) {
        if (!strings[i]) continue;
        size_t len = (length && length[i] >= 0)
        ? (size_t)length[i]
        : strlen(strings[i]);
        memcpy(buf + pos, strings[i], len);
        pos += len;
    }
    buf[pos] = '\0';
    *out_len  = pos;
    return buf;
}

/* -------------------------------------------------------------------------
 * Real glShaderSource pointer – resolved lazily on first call
 * ------------------------------------------------------------------------- */
static PFNGLSHADERSOURCEPROC real_glShaderSource = NULL;

static void resolve_real_glShaderSource(void)
{
    if (real_glShaderSource) return;
    real_glShaderSource =
    (PFNGLSHADERSOURCEPROC)dlsym(RTLD_NEXT, "glShaderSource");
    if (!real_glShaderSource)
        fprintf(stderr,
                "[glshader_sanitize] WARNING: could not resolve "
                "glShaderSource via RTLD_NEXT\n");
}

/* -------------------------------------------------------------------------
 * Shared implementation called from all intercept points
 * ------------------------------------------------------------------------- */
static void my_glShaderSource(GLuint shader, GLsizei count,
                               const GLchar *const *strings,
                               const GLint *length)
{
    resolve_real_glShaderSource();
    if (!real_glShaderSource) return;

    size_t src_len = 0;
    char  *joined  = join_source(count, strings, length, &src_len);
    if (!joined) {
        real_glShaderSource(shader, count, strings, length);
        return;
    }

    char *rewritten = sanitize_shader(joined, src_len);

    #ifdef DEBUG_SANITIZE
    fprintf(stderr,
            "=== [glshader_sanitize] shader %u – %s ===\n%s\n=== end ===\n",
            shader,
            rewritten ? "REWRITTEN" : "unchanged",
            rewritten ? rewritten   : joined);
    #endif

    {
        const GLchar *ptr = rewritten ? rewritten : joined;
        real_glShaderSource(shader, 1, &ptr, NULL);
    }

    free(rewritten);
    free(joined);
}


