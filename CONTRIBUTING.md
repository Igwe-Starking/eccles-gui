# Contributing

Thanks for looking. The rules are short.

## Before you open a pull request

```
make test-asan     # lint (-Wpedantic -Wconversion, C99) + the whole suite under AddressSanitizer + UBSan
make test-all      # the configuration matrix (12 configs), each under the sanitizers
make fuzz          # 300 seeds of the widget fuzzer
make gen-check     # generated tables in src/eccles_gui.c, docs/API.md and keywords.txt are current
make mocks cxx tools-test
```

## House rules

- **Three files.** The library is `src/eccles_gui.h`, `src/eccles_gui.c` and `config/eccles_gui_config.h`. Do not add a second source file.
- **No heap, no floats, no libm, no globals that are not `const`.** Everything lives in `eccles_gui_t`.
- **Everything is `eccles_gui_*`**, statuses are `ECCLES_GUI_SUCCESSFUL / FAILED / UNSUPPORTED`, reasons go through `g_err()` so `eccles_gui_last_error()` stays truthful.
- **The invariants are the contract.** Rendering must give the same picture at every band height, and an incremental update must equal a full redraw. If you add a widget or a draw call, add it to `build_scene()` in `tests/test_gui.c`, and if it can be configured out, make the suite pass with it out.
- **Generated blocks are generated.** Edit `tools/gen_data.py`, not the font/sine/mesh tables in `eccles_gui.c`. Run `python3 tools/gen_api_doc.py` and `python3 tools/gen_keywords.py` after changing the header.
- Say which board and SDK you verified a target specific change on.
