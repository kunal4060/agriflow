#!/usr/bin/env python3
"""AGRIFLOW V3 build: one page template -> preview.html + firmware .ino.

Same single-source-of-truth rule as V2: the dashboard page exists exactly
once (page_template.html). The preview gets the fetch-stub injected; the
firmware gets the stub block removed, so the two can never diverge.
"""
import pathlib, sys

HERE = pathlib.Path(__file__).resolve().parent
ROOT = HERE.parent

template = (HERE / "page_template.html").read_text(encoding="utf-8")
stub = (HERE / "preview_stub.js").read_text(encoding="utf-8")
head = (HERE / "firmware_head.cppfrag").read_text(encoding="utf-8")
tail = (HERE / "firmware_tail.cppfrag").read_text(encoding="utf-8")

STUB_BLOCK = "<script>\n/*__PREVIEW_STUB__*/\n</script>"
assert STUB_BLOCK in template, "stub marker block missing from template"
assert ")AGRIFLOW" not in template, "page would break the C++ raw literal"

# 1) Browser preview: stub in, everything else identical.
preview = template.replace("/*__PREVIEW_STUB__*/", stub.rstrip())
(ROOT / "preview.html").write_text(preview, encoding="utf-8")

# 2) Firmware page: stub block out completely.
page_fw = template.replace(STUB_BLOCK, "<!-- dashboard: single inline page, no external resources -->")
assert "__PREVIEW_STUB__" not in page_fw and "simJson" not in page_fw

ino = head + page_fw
if not ino.endswith("\n"):
    ino += "\n"
ino += tail
(ROOT / "firmware" / "Code_ESP32_Plant_Watering_V3.ino").write_text(ino, encoding="utf-8")

print("page bytes (firmware):", len(page_fw.encode("utf-8")))
print("preview bytes:", len(preview.encode("utf-8")))
print("ino bytes:", len(ino.encode("utf-8")))
print("OK")
