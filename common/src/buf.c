#include "vapor/buf.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void
vapor_buf_init(vapor_buf *b)
{
    b->data = NULL;
    b->len = 0;
    b->cap = 0;
}

void
vapor_buf_free(vapor_buf *b)
{
    free(b->data);
    vapor_buf_init(b);
}

void
vapor_buf_reset(vapor_buf *b)
{
    b->len = 0;
    if (b->data) {
        b->data[0] = '\0';
    }
}

int
vapor_buf_reserve(vapor_buf *b, size_t extra)
{
    size_t need, cap;
    char  *p;

    /* +1 for the trailing NUL we always maintain. */
    if (extra > (size_t)-1 - b->len - 1) {
        return -1;
    }
    need = b->len + extra + 1;
    if (need <= b->cap) {
        return 0;
    }

    cap = b->cap ? b->cap : 64;
    while (cap < need) {
        if (cap > ((size_t)-1) / 2) {
            cap = need;
            break;
        }
        cap *= 2;
    }

    p = (char *)realloc(b->data, cap);
    if (!p) {
        return -1;
    }
    b->data = p;
    b->cap = cap;
    return 0;
}

int
vapor_buf_append(vapor_buf *b, const void *data, size_t n)
{
    if (n == 0) {
        /* Still materialise the buffer so data is never NULL after an append. */
        if (vapor_buf_reserve(b, 0) != 0) {
            return -1;
        }
        b->data[b->len] = '\0';
        return 0;
    }
    if (vapor_buf_reserve(b, n) != 0) {
        return -1;
    }
    memcpy(b->data + b->len, data, n);
    b->len += n;
    b->data[b->len] = '\0';
    return 0;
}

int
vapor_buf_appends(vapor_buf *b, const char *s)
{
    return s ? vapor_buf_append(b, s, strlen(s)) : 0;
}

int
vapor_buf_appendf(vapor_buf *b, const char *fmt, ...)
{
    va_list ap;
    int     n;

    va_start(ap, fmt);
    n = vsnprintf(NULL, 0, fmt, ap);
    va_end(ap);
    if (n < 0) {
        return -1;
    }
    if (vapor_buf_reserve(b, (size_t)n) != 0) {
        return -1;
    }

    va_start(ap, fmt);
    vsnprintf(b->data + b->len, (size_t)n + 1, fmt, ap);
    va_end(ap);

    b->len += (size_t)n;
    b->data[b->len] = '\0';
    return 0;
}

char *
vapor_buf_release(vapor_buf *b)
{
    char *p = b->data;
    vapor_buf_init(b);
    return p;
}
