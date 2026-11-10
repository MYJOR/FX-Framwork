#ifndef INFLATE_H
#define INFLATE_H
#include <stddef.h>

/* Decompresses a zlib stream (RFC 1950: 2-byte header, deflate data, adler32) into dst.
   Returns the number of bytes written, or -1 if the stream is corrupt or dst is too small.
   Used by the PNG reader and the FBX reader. */
long zlib_inflate(const unsigned char *src, size_t src_len, unsigned char *dst, size_t dst_cap);

#endif
