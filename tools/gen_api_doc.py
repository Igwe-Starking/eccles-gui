#!/usr/bin/env python3
"""
gen_api_doc.py - writes docs/API.md from the comments in src/eccles_gui.h, so the reference cannot drift from the header

  python3 tools/gen_api_doc.py            rewrite docs/API.md
  python3 tools/gen_api_doc.py --check    fail if it is stale (CI runs this)
"""
import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
HDR = os.path.join(HERE, "..", "src", "eccles_gui.h")
OUT = os.path.join(HERE, "..", "docs", "API.md")

SECTIONS = {
    "init / loop": "Setup and the main loop",
    "fonts and text": "Fonts and text",
    "drawing (use inside a draw callback, or let the widgets do it)": "Drawing",
    "colors / math helpers": "Colors and math",
    "styles": "Styles and theme",
    "widgets: creating": "Widgets: creating",
    "widgets: the tree": "Widgets: the tree",
    "widgets: geometry and look. every setter redraws what it changed": "Widgets: geometry and look",
    "widgets: content": "Widgets: content",
    "input": "Input",
    "animation": "Animation",
    "3d drawing": "3D drawing",
    "view3d: the scene lives inside the context, edit it with these or take the pointer": "3D scenes (view3d widget)",
}


def parse():
    lines = open(HDR, encoding="utf-8").read().split("\n")
    out = []  # (kind, text)
    comment = []
    i = 0
    in_api = False
    cond = []
    while i < len(lines):
        ln = lines[i]
        s = ln.strip()
        if s.startswith("//eccles_gui is") or "init / loop" == s.lstrip("/ "):
            pass
        if s == "//init / loop":
            in_api = True
        if not in_api:
            i += 1
            continue
        m = re.match(r"^//(.*)$", s)
        if m and m.group(1).strip() in SECTIONS:
            out.append(("section", m.group(1).strip()))
            comment = []
            i += 1
            continue
        if s.startswith("//"):
            comment.append(s[2:].strip())
            i += 1
            continue
        if s == "":
            comment = []
            i += 1
            continue
        if s.startswith("#"):
            comment = [] if s.startswith("#if") or s.startswith("#endif") else comment
            i += 1
            continue
        # a prototype, may span lines
        if re.match(r"^(eccles_gui_|const |void |bool |int |uint|static inline|char|int32|int16)", s) or re.match(r"^[a-z_0-9]+_t \*?eccles_gui_", s):
            def split(line):  #code and trailing //comment
                k = line.find("//")
                return (line[:k].strip(), line[k + 2:].strip()) if k >= 0 else (line.strip(), "")
            proto, tail = split(s)
            j = i
            while not proto.endswith(";") and j + 1 < len(lines):
                j += 1
                code, t2 = split(lines[j])
                proto += " " + code
                tail = (tail + " " + t2).strip()
            if tail:
                comment.append(tail)
            if "eccles_gui_" in proto and "(" in proto and not proto.startswith("typedef") and not proto.startswith("static inline"):
                name = re.search(r"(eccles_gui_\w+)\s*\(", proto)
                if name:
                    out.append(("fn", name.group(1), re.sub(r"\s+", " ", proto), " ".join(comment)))
            comment = []
            i = j + 1
            continue
        comment = []
        i += 1
    return out


def render(items):
    md = ["# eccles_gui API reference", "",
          "Generated from the comments in `src/eccles_gui.h` by `tools/gen_api_doc.py`. Do not edit by hand.",
          "Everything that can fail returns `eccles_gui_status_t` (`ECCLES_GUI_SUCCESSFUL`, `ECCLES_GUI_FAILED`, `ECCLES_GUI_UNSUPPORTED`); "
          "the reason is in `eccles_gui_last_error()` / `eccles_gui_last_error_str()`.", ""]
    cur = None
    for it in items:
        if it[0] == "section":
            title = SECTIONS.get(it[1])
            if title:
                md += ["## " + title, ""]
                cur = title
            continue
        if cur is None:
            continue
        _, name, proto, comment = it
        md += ["### `%s`" % name, "", "```c", proto, "```", ""]
        if comment:
            md += [comment[0].upper() + comment[1:] if comment else "", ""]
    return "\n".join(md).rstrip() + "\n"


def main():
    text = render(parse())
    if "--check" in sys.argv:
        old = open(OUT, encoding="utf-8").read() if os.path.exists(OUT) else ""
        if old != text:
            print("docs/API.md is stale: run python3 tools/gen_api_doc.py")
            sys.exit(1)
        print("docs/API.md up to date")
        return
    os.makedirs(os.path.dirname(OUT), exist_ok=True)
    open(OUT, "w", encoding="utf-8", newline="\n").write(text)
    n = text.count("### `")
    print("docs/API.md written, %d functions" % n)


if __name__ == "__main__":
    main()
