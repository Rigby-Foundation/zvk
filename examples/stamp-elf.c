/* SPDX-License-Identifier: LGPL-2.1-or-later */
/* Copyright (C) 2026 Rigby Foundation */
/* sic's minimal dd omits conv=notrunc; stamp only ELF's OSABI byte. */
#include <stdio.h>
#include <string.h>
int main(int argc, char **argv)
{
    if (argc != 2)
        return 2;
    FILE *f = fopen(argv[1], "r+b");
    unsigned char header[16];
    if (!f) {
        perror(argv[1]);
        return 1;
    }
    int failed = fread(header, 1, sizeof header, f) != sizeof header ||
                 memcmp(header, "\177ELF", 4) || (header[7] != 0 && header[7] != 0x53);
    if (!failed)
        failed = fseek(f, 7, SEEK_SET) || fputc(0x53, f) == EOF;
    if (fclose(f))
        failed = 1;
    return failed ? 1 : 0;
}
