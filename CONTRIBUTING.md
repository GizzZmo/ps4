# Contributing

Thank you for your interest in contributing to the PS4 Mach-O Loader PoC. Contributions of all kinds are welcome — bug reports, documentation improvements, new features, and refactoring.

---

## Table of Contents

1. [Code of Conduct](#code-of-conduct)
2. [Getting Started](#getting-started)
3. [Reporting Bugs](#reporting-bugs)
4. [Suggesting Enhancements](#suggesting-enhancements)
5. [Pull Request Guidelines](#pull-request-guidelines)
6. [Code Style](#code-style)
7. [Testing](#testing)

---

## Code of Conduct

Be respectful and constructive in all interactions. This project is for educational and research purposes; please keep discussions focused on the technical aspects.

---

## Getting Started

1. **Fork** the repository on GitHub.
2. **Clone** your fork locally:
   ```bash
   git clone https://github.com/<your-username>/ps4.git
   cd ps4
   ```
3. **Create a branch** for your change:
   ```bash
   git checkout -b feature/my-improvement
   ```
4. **Build and test** your changes (see [docs/BUILD.md](docs/BUILD.md)).
5. **Push** to your fork and open a Pull Request.

---

## Reporting Bugs

Open a GitHub Issue with:

- A clear, descriptive title.
- Steps to reproduce the problem.
- Expected vs. actual behaviour.
- The diagnostic output from [docs/TROUBLESHOOTING.md § Gathering Diagnostic Information](docs/TROUBLESHOOTING.md#gathering-diagnostic-information).
- Your OS, Clang version, and PS4 firmware (if applicable).

---

## Suggesting Enhancements

Open a GitHub Issue with:

- A description of the feature and the problem it solves.
- Any relevant references (Mach-O spec sections, PS4 reverse-engineering resources, etc.).
- An indication of whether you plan to implement it yourself.

Likely areas of interest:
- Darwin → FreeBSD syscall translation table
- `dyld` (dynamic linker) emulation
- Support for `LC_DYLIB_CODE_SIGN_DRS` and other code-signing commands
- ARM64 (`__TEXT` segment) support for Apple Silicon Mach-O files

---

## Pull Request Guidelines

1. **One concern per PR.** Fix one bug or add one feature per pull request.
2. **Update documentation.** If you change a public API or add a new file, update the relevant `docs/` pages.
3. **Maintain backwards compatibility.** Do not change existing function signatures without a strong reason.
4. **Add a test.** If you fix a bug or add a feature, include or update the relevant test harness.
5. **Reference issues.** Link to the issue your PR addresses in the PR description.

---

## Code Style

The codebase follows a consistent C99/C11 style:

- **Indentation:** 4 spaces (no tabs).
- **Braces:** K&R style — opening brace on the same line for control flow; function definitions have the brace on the next line.
- **Line length:** ≤ 80 characters where possible.
- **Comments:**
  - Block comments use `/* … */`.
  - Section separators use a row of dashes inside a block comment as shown in existing files.
  - Inline comments are used sparingly and aligned where multiple related lines are commented.
- **Types:** Use `uint8_t`, `uint32_t`, `uint64_t`, `size_t`, and `uintptr_t` from `<stdint.h>` / `<stddef.h>`. Avoid `int` for sizes and addresses.
- **Macros:** `ALL_CAPS`. Use `UINT32_C()` / `UINT64_C()` wrappers for integer constants to avoid implicit type issues.

---

## Testing

Run the self-contained integration test with:

```bash
make test
```

This builds and executes `integration_test`, which embeds the Mach-O payload as a C byte array (no cross-compilation toolchain required) and prints `Integration test: PASS` on success.

Before submitting a PR, verify:

1. **Payload cross-compilation** produces a valid Mach-O (check with `otool -l` or `objdump -p`).
2. **Loader build** succeeds with `-Wall -Wextra` and zero warnings.
3. **Integration test** (`make test`) exits with code 0 and prints `PASS`.
4. If you have access to a PS4 with an active exploit, verify the test passes on the console too.
