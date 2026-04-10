"""Convert all SVGs in docs/images/ to PNGs using Playwright headless Chromium."""
import base64
from pathlib import Path
from playwright.sync_api import sync_playwright

ROOT = Path(__file__).resolve().parents[1]
IMG_DIR = ROOT / "docs" / "images"

svgs = sorted(IMG_DIR.glob("*.svg"))
print(f"Found {len(svgs)} SVGs in {IMG_DIR}")

with sync_playwright() as p:
    browser = p.chromium.launch()
    context = browser.new_context(device_scale_factor=2)
    page = context.new_page()

    for svg in svgs:
        content = svg.read_text(encoding="utf-8")
        # Extract width/height from root <svg> tag
        import re
        w = re.search(r'width="(\d+)"', content)
        h = re.search(r'height="(\d+)"', content)
        width = int(w.group(1)) if w else 1200
        height = int(h.group(1)) if h else 600

        html = f"""<!doctype html><html><head><style>
        html,body{{margin:0;padding:0;background:transparent;}}
        svg{{display:block;}}
        </style></head><body>{content}</body></html>"""

        page.set_viewport_size({"width": width, "height": height})
        page.set_content(html)
        out = svg.with_suffix(".png")
        page.screenshot(path=str(out), omit_background=False, full_page=False,
                        clip={"x": 0, "y": 0, "width": width, "height": height})
        print(f"  {svg.name} -> {out.name} ({width}x{height})")

    browser.close()

print("Done.")
