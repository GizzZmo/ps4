/*
 * integration_test.c - Self-contained integration test for the PS4 Mach-O
 *                      loader.
 *
 * The bare-metal Mach-O payload is embedded directly as a C byte array
 * (bare_metal_test_data.h) so the test requires no external files and
 * passes without a cross-compilation toolchain present.
 *
 * Expected output:
 *   Integration test: PASS
 *
 * Exit code: 0 on success, 1 on failure.
 */

#include "bare_metal_test_data.h"
#include "macho_loader.h"

#include <stdio.h>

int main(void)
{
    int ok = test_macho_execution(bare_metal_test_data, bare_metal_test_size);

    printf("Integration test: %s\n", ok ? "PASS" : "FAIL");

    return ok ? 0 : 1;
}
