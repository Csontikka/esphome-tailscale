"""Mask sensitive regions in Tailscale admin screenshots and copy them into docs/images/."""
from pathlib import Path
from PIL import Image, ImageDraw, ImageFilter

SRC = Path(r"C:/Users/devuser2/.claude/image-cache/1f178e62-4b62-47bd-bf39-dddc19fed804")
DST = Path(__file__).resolve().parents[1] / "docs" / "images"
DST.mkdir(parents=True, exist_ok=True)


def blur_region(img, box):
    """Blur a rectangular region in place."""
    x1, y1, x2, y2 = box
    crop = img.crop(box).filter(ImageFilter.GaussianBlur(radius=12))
    img.paste(crop, box)


def black_region(img, box, label=None):
    """Draw an opaque black bar over a region with optional centered white label."""
    draw = ImageDraw.Draw(img)
    draw.rectangle(box, fill=(20, 20, 20))
    if label:
        cx = (box[0] + box[2]) // 2
        cy = (box[1] + box[3]) // 2
        draw.text((cx - len(label) * 3, cy - 6), label, fill=(180, 180, 180))


# 77: Generate auth key dialog — no masking needed
img77 = Image.open(SRC / "77.png").convert("RGBA")
img77.save(DST / "tailscale-auth-key-create.png")
print(f"tailscale-auth-key-create.png  ({img77.size[0]}x{img77.size[1]})  no masking")

# 76: Keys page — mask the auth key IDs in the table and highlight the
# "Generate auth key..." button with a gold outline so readers know where to click.
img76 = Image.open(SRC / "76.png").convert("RGBA")
# ID column of the two auth key rows
black_region(img76, (397, 505, 565, 535), label="REDACTED")
black_region(img76, (397, 541, 565, 571), label="REDACTED")
# Gold highlight around the "Generate auth key..." button (top-right of Auth keys card)
draw76 = ImageDraw.Draw(img76)
gold = (255, 215, 0, 255)
btn_box = (994, 413, 1146, 447)
for offset in range(4):
    draw76.rectangle(
        (btn_box[0] - offset, btn_box[1] - offset, btn_box[2] + offset, btn_box[3] + offset),
        outline=gold,
    )
img76.save(DST / "tailscale-keys-page.png")
print(f"tailscale-keys-page.png         ({img76.size[0]}x{img76.size[1]})  masked 2 key IDs")

# 78: Machines list — nuke the entire personal column (hostnames + emails + badges)
# and the IP column. Only the tech info (chip type, state, menu) stays visible.
img78 = Image.open(SRC / "78.png").convert("RGBA")
W, H = img78.size
# Entire left column: hostnames, emails, badges
black_region(img78, (0, 0, 340, H), label="")
# IP addresses column
black_region(img78, (340, 0, 540, H), label="")
img78.save(DST / "tailscale-disable-key-expiry.png")
print(f"tailscale-disable-key-expiry.png ({img78.size[0]}x{img78.size[1]})  masked emails/IPs/hostnames")

print("Done.")
