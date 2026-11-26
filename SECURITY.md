# Security Policy

Eccles GUI draws; it does not parse network input. The data it consumes (fonts, images, meshes, the text you give a label) is meant to come from your own firmware image, and it is treated that way: **it is trusted data**.

## What is and is not hardened

- Drawing is clipped to the band buffer you provide and to the panel. Every draw call with out of range coordinates, sizes or radii is safe (the test suite runs thousands of random and degenerate calls under AddressSanitizer and UBSan).
- Meshes: out of range vertex/face indices are skipped, vertex and face caches are bounds-checked, huge coordinates and scales are tolerated without crashing.
- Text: malformed UTF-8 never reads past the terminating NUL and falls back to `?`.
- **Not validated:** glyph offsets inside a font bitmap, the data pointer and size of an `eccles_gui_image_t`, and the vertex data pointers of a mesh. A hostile *font, image or mesh file loaded at run time from outside* can make the engine read out of bounds. If you load assets from storage or a network, validate them first (the converters in `tools/` only produce well formed data).
- The context is not thread safe. Call everything from one task or guard it with your own mutex.

## Reporting a vulnerability

Please **do not** open a public issue for something exploitable. Use GitHub's private reporting instead: **Security -> Report a vulnerability** on this repository.

Include the `ECCLES_GUI_*` configuration you used, your board and toolchain, and the smallest program that reproduces it. You'll get an acknowledgement as soon as I can, and credit in the changelog if you'd like it.

## Supported versions

Only the latest release receives fixes.
