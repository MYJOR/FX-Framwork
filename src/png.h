#ifndef PNG_H
#define PNG_H
/* Writes an 8-bit RGB image (top row first) as an uncompressed PNG. */
int png_write_rgb(const char *path, int w, int h, const unsigned char *rgb);
/* Reads an 8-bit, non-interlaced PNG (grey, grey+alpha, RGB or RGBA). Returns malloc'd pixels, top row
   first, with *channels bytes per pixel; NULL on failure. The caller frees the result. */
unsigned char *png_load(const char *path, int *w, int *h, int *channels);
#endif
