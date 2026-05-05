/*
 * payload.c - Bare-metal Mach-O payload for PS4 Mach-O loader PoC.
 *
 * This source is intentionally free of includes and standard library
 * dependencies.  It is cross-compiled into a static, nostdlib Mach-O
 * binary so that the loader can map and execute it in isolation.
 *
 * Cross-compilation (macOS / Linux with Clang):
 *   clang -target x86_64-apple-macos \
 *         -static -nostdlib \
 *         -e __start \
 *         -o bare_metal_test payload.c
 *
 * Note: Darwin/macOS tools automatically prefix every C symbol with a
 * leading underscore.  The function _start therefore appears as __start
 * in the Mach-O symbol table, so -e __start (two underscores) must be
 * passed to the linker.
 *
 * Expected result: _start() returns 0x1337 + 0x42 == 0x1379.
 */

/* No includes – we are running in a vacuum. */

int _start(void)
{
    int a = 0x1337;
    int b = 0x42;
    return a + b; /* Must equal 0x1379 when verified by the loader. */
}
