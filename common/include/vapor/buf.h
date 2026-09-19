#ifndef VAPOR_BUF_H
#define VAPOR_BUF_H

#include <stddef.h>

/* Growable byte buffer. Always keeps a NUL one past `len`, so `data` can be
 * handed to string APIs even when it was filled with binary appends. */
typedef struct {
    char  *data;
    size_t len;
    size_t cap;
} vapor_buf;

void vapor_buf_init(vapor_buf *b);
void vapor_buf_free(vapor_buf *b);
void vapor_buf_reset(vapor_buf *b);

/* All appenders return 0 on success, -1 on allocation failure. */
int vapor_buf_reserve(vapor_buf *b, size_t extra);
int vapor_buf_append(vapor_buf *b, const void *data, size_t n);
int vapor_buf_appends(vapor_buf *b, const char *s);
int vapor_buf_appendf(vapor_buf *b, const char *fmt, ...);

/* Hands the allocation to the caller and resets the buffer. */
char *vapor_buf_release(vapor_buf *b);

#endif /* VAPOR_BUF_H */
