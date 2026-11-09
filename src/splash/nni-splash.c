/*
 * nni-splash - draw a PPM picture in the middle of the Linux framebuffer.
 * Used by the nni family (YnniOS, SGnniOS) to show the chicken while booting.
 *
 *   nni-splash IMAGE.ppm [/dev/fb0]
 *
 * The screen is filled with the picture's top-left colour, then the picture is
 * drawn centred, enlarged by a whole number (2x, 3x...) when the screen has room.
 * Handles 16, 24 and 32 bits per pixel. Binary (P6) PPM, maxval 255 only.
 *
 * Testing without a framebuffer:
 *   NNI_SPLASH_TEST="WIDTH HEIGHT BPP out.raw" nni-splash IMAGE.ppm
 * writes what would be on screen to out.raw instead.
 */
#include <errno.h>
#include <fcntl.h>
#include <linux/fb.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>

struct image { int w, h; unsigned char *rgb; };

static int read_token(FILE *f, char *buf, size_t n)
{
    int c;
    size_t i = 0;
    for (;;) {                                  /* skip blanks and # comments */
        c = fgetc(f);
        if (c == '#') { while (c != '\n' && c != EOF) c = fgetc(f); continue; }
        if (c == ' ' || c == '\t' || c == '\n' || c == '\r') continue;
        break;
    }
    while (c != EOF && c != ' ' && c != '\t' && c != '\n' && c != '\r' && i + 1 < n) {
        buf[i++] = (char)c;
        c = fgetc(f);
    }
    buf[i] = 0;
    return i > 0 ? 0 : -1;                      /* one whitespace byte consumed */
}

static int load_ppm(const char *path, struct image *img)
{
    char tok[32];
    long w, h, maxv;
    FILE *f = fopen(path, "rb");
    if (!f) { fprintf(stderr, "nni-splash: %s: %s\n", path, strerror(errno)); return -1; }
    if (read_token(f, tok, sizeof tok) || strcmp(tok, "P6")) goto bad;
    if (read_token(f, tok, sizeof tok)) goto bad;
    w = strtol(tok, NULL, 10);
    if (read_token(f, tok, sizeof tok)) goto bad;
    h = strtol(tok, NULL, 10);
    if (read_token(f, tok, sizeof tok)) goto bad;
    maxv = strtol(tok, NULL, 10);
    if (w <= 0 || h <= 0 || w > 8192 || h > 8192 || maxv != 255) goto bad;
    img->w = (int)w; img->h = (int)h;
    img->rgb = malloc((size_t)w * h * 3);
    if (!img->rgb || fread(img->rgb, 3, (size_t)w * h, f) != (size_t)w * h) goto bad;
    fclose(f);
    return 0;
bad:
    fprintf(stderr, "nni-splash: %s is not a binary PPM (P6, 255) picture\n", path);
    fclose(f);
    return -1;
}

static uint32_t chan(unsigned v, const struct fb_bitfield *bf)
{
    if (bf->length == 0) return 0;
    return ((uint32_t)v >> (8 - (bf->length > 8 ? 8 : bf->length))) << bf->offset;
}

int main(int argc, char **argv)
{
    struct image img;
    struct fb_var_screeninfo var;
    struct fb_fix_screeninfo fix;
    const char *dev = argc > 2 ? argv[2] : "/dev/fb0";
    const char *test = getenv("NNI_SPLASH_TEST");
    char outpath[512] = "";
    unsigned char *fb;
    size_t fblen;
    int fd = -1, bpp, scale, x0, y0, x, y;

    if (argc < 2) { fprintf(stderr, "usage: nni-splash IMAGE.ppm [/dev/fb0]\n"); return 2; }
    if (load_ppm(argv[1], &img)) return 1;

    memset(&var, 0, sizeof var);
    memset(&fix, 0, sizeof fix);
    if (test) {                                 /* fake framebuffer for testing */
        int tw, th, tb;
        if (sscanf(test, "%d %d %d %511s", &tw, &th, &tb, outpath) != 4) return 2;
        var.xres = var.xres_virtual = (unsigned)tw;
        var.yres = var.yres_virtual = (unsigned)th;
        var.bits_per_pixel = (unsigned)tb;
        if (tb == 16) {
            var.red.offset = 11; var.red.length = 5;
            var.green.offset = 5; var.green.length = 6;
            var.blue.offset = 0; var.blue.length = 5;
        } else {
            var.red.offset = 16; var.green.offset = 8; var.blue.offset = 0;
            var.red.length = var.green.length = var.blue.length = 8;
        }
        fix.line_length = (unsigned)(tw * tb / 8);
        fix.smem_len = fix.line_length * (unsigned)th;
        fb = calloc(1, fix.smem_len);
        if (!fb) return 1;
    } else {
        fd = open(dev, O_RDWR);
        if (fd < 0) { fprintf(stderr, "nni-splash: %s: %s\n", dev, strerror(errno)); return 1; }
        if (ioctl(fd, FBIOGET_VSCREENINFO, &var) || ioctl(fd, FBIOGET_FSCREENINFO, &fix)) {
            fprintf(stderr, "nni-splash: %s is not a framebuffer\n", dev); return 1;
        }
        fb = mmap(NULL, fix.smem_len, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
        if (fb == MAP_FAILED) { fprintf(stderr, "nni-splash: cannot map %s\n", dev); return 1; }
    }
    fblen = fix.smem_len;
    bpp = (int)var.bits_per_pixel;
    if (bpp != 16 && bpp != 24 && bpp != 32) {
        fprintf(stderr, "nni-splash: %d bits per pixel is not supported\n", bpp); return 1;
    }

    scale = 1;                                  /* biggest whole-number zoom that fits */
    while ((scale + 1) * img.w <= (int)var.xres * 6 / 10 && (scale + 1) * img.h <= (int)var.yres * 6 / 10)
        scale++;
    if (img.w > (int)var.xres || img.h > (int)var.yres) {
        fprintf(stderr, "nni-splash: picture is larger than the screen\n"); return 1;
    }
    x0 = ((int)var.xres - img.w * scale) / 2;
    y0 = ((int)var.yres - img.h * scale) / 2;

    for (y = 0; y < (int)var.yres; y++) {
        size_t row = ((size_t)y + var.yoffset) * fix.line_length;
        for (x = 0; x < (int)var.xres; x++) {
            const unsigned char *p = img.rgb;   /* background = top-left pixel */
            int ix = (x - x0) / scale, iy = (y - y0) / scale;
            uint32_t v;
            size_t off = row + ((size_t)x + var.xoffset) * (size_t)(bpp / 8);
            if (x >= x0 && y >= y0 && ix < img.w && iy < img.h)
                p = img.rgb + ((size_t)iy * img.w + ix) * 3;
            v = chan(p[0], &var.red) | chan(p[1], &var.green) | chan(p[2], &var.blue);
            if (off + (size_t)(bpp / 8) > fblen) continue;
            if (bpp == 16) { uint16_t s = (uint16_t)v; memcpy(fb + off, &s, 2); }
            else if (bpp == 24) { fb[off] = v & 0xff; fb[off + 1] = (v >> 8) & 0xff; fb[off + 2] = (v >> 16) & 0xff; }
            else memcpy(fb + off, &v, 4);
        }
    }

    if (test) {
        FILE *o = fopen(outpath, "wb");
        if (!o || fwrite(fb, 1, fblen, o) != fblen) return 1;
        fclose(o);
    }
    return 0;
}
