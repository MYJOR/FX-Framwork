#ifndef PNG_H
#define PNG_H
/* Writes an 8-bit RGB image (top row first) as an uncompressed PNG. */
int png_write_rgb(const char *path, int w, int h, const unsigned char *rgb);
#endif
