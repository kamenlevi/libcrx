/* SPDX-License-Identifier: Apache-2.0
 *
 * crxoracle: the reference fingerprint of a CR3 file, taken from LibRaw.
 * One TSV row per file, no paths, so the output can be published:
 *
 *   file_sha256 model raw_w raw_h w h top left filters black cb0 cb1 cb2 cb3 max pixels_sha256 libraw
 *
 * pixels_sha256 is the SHA-256 of LibRaw's raw_image (raw_w * raw_h uint16)
 * as little-endian bytes. Files LibRaw cannot unpack, or that have no
 * 16-bit Bayer buffer (sRAW), print a row starting with '#'. */
#include "common.h"
#include <libraw/libraw.h>

int main(int argc, char **argv)
{
    setvbuf(stdout, NULL, _IOLBF, 0);
    if (argc < 2) { fprintf(stderr, "usage: crxoracle <file.cr3>...\n"); return 2; }
    int bad = 0;
    for (int i = 1; i < argc; i++) {
        size_t len; uint8_t *bytes = read_file(argv[i], &len);
        if (!bytes) { printf("#\tunreadable\t%s\n", argv[i]); bad++; continue; }
        char fh[65]; sha256_hex(bytes, len, fh);
        libraw_data_t *lr = libraw_init(0);
        int r = libraw_open_buffer(lr, bytes, len);
        if (r == 0) r = libraw_unpack(lr);
        if (r != 0) {
            printf("#\t%s\tlibraw:%s\n", fh, libraw_strerror(r));
            bad++; libraw_close(lr); free(bytes); continue;
        }
        if (!lr->rawdata.raw_image) {
            printf("#\t%s\tno-bayer-buffer\n", fh);
            bad++; libraw_close(lr); free(bytes); continue;
        }
        char ph[65];
        sha256_u16le(lr->rawdata.raw_image, (size_t)lr->sizes.raw_width * lr->sizes.raw_height, ph);
        printf("%s\t%s\t%u\t%u\t%u\t%u\t%u\t%u\t%08x\t%u\t%u\t%u\t%u\t%u\t%u\t%s\t%s\n",
               fh, lr->idata.model, lr->sizes.raw_width, lr->sizes.raw_height,
               lr->sizes.width, lr->sizes.height, lr->sizes.top_margin, lr->sizes.left_margin,
               lr->idata.filters, lr->color.black, lr->color.cblack[0], lr->color.cblack[1],
               lr->color.cblack[2], lr->color.cblack[3], lr->color.maximum, ph, libraw_version());
        libraw_close(lr); free(bytes);
    }
    return bad ? 1 : 0;
}
