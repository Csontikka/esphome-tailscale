"""Capture a fresh screenshot of https://web.esphome.io/ using Playwright."""
from pathlib import Path
from playwright.sync_api import sync_playwright

OUT = Path(__file__).resolve().parents[1] / "docs" / "images" / "esphome-web-flasher.png"

with sync_playwright() as p:
    browser = p.chromium.launch()
    context = browser.new_context(
        viewport={"width": 1280, "height": 800},
        device_scale_factor=2,
    )
    page = context.new_page()
    page.goto("https://web.esphome.io/", wait_until="networkidle", timeout=30000)
    page.wait_for_timeout(1500)
    page.screenshot(path=str(OUT), full_page=False)
    browser.close()

print(f"Saved: {OUT}")
