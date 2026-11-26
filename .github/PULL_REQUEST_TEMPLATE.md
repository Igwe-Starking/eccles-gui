## What does this change?

<!-- One or two sentences. Link the issue if there is one: Fixes #123 -->

## Checklist

- [ ] `make test-asan` and `make test-all` pass
- [ ] `make fuzz` passes if the widget tree, dirty rectangles, input or the band loop changed
- [ ] `make gen-check` passes (generated tables, `docs/API.md`, `keywords.txt`)
- [ ] New behavior has a test in `tests/test_gui.c` (and new widgets/draw calls are in `build_scene()`)
- [ ] Docs / README / CHANGELOG updated if the public API or configuration changed
- [ ] Target-specific change? Say which board and SDK version you verified it on
