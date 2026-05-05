/*
 * main.c - Driver for the PS4 Mach-O loader test harness.
 *
 * Reads the Mach-O payload produced by payload.c (bare_metal_test) from
 * disk, passes it to test_macho_execution(), and reports the result.
 *
 * Usage:
 *   ./macho_loader_test <path-to-bare_metal_test>
 *
 * If no path is given, the file "bare_metal_test" in the current directory
 * is used as the default.
 */

#include "macho_loader.h"

#include <stdio.h>
#include <stdlib.h>

static uint8_t *read_file(const char *path, size_t *out_size)
{
    FILE *f = fopen(path, "rb");
    if (!f) {
        return NULL;
    }
    fseek(f, 0, SEEK_END);
    long len = ftell(f);
    if (len <= 0) {
        fclose(f);
        return NULL;
    }
    rewind(f);
    *out_size = (size_t)len;
    uint8_t *buf = malloc(*out_size);
    if (!buf) {
        fclose(f);
        return NULL;
    }
    if (fread(buf, 1, *out_size, f) != *out_size) {
        free(buf);
        fclose(f);
        return NULL;
    }
    fclose(f);
    return buf;
}

int main(int argc, char *argv[])
{
    const char *path = (argc > 1) ? argv[1] : "bare_metal_test";

    size_t   size;
    uint8_t *data = read_file(path, &size);
    if (!data) {
        fprintf(stderr, "Cannot open payload file: %s\n", path);
        return 1;
    }

    int ok = test_macho_execution(data, size);
    printf("Result: %s (expected return value 0x1379)\n",
           ok ? "PASS" : "FAIL");
    free(data);
    return ok ? 0 : 1;
}
