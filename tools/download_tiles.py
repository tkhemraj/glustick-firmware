#!/usr/bin/env python3
"""
Download OpenStreetMap tiles for the GluStic Fun map view.

Tiles are saved as JPEG files at:  <output_dir>/tiles/Z/X/Y.jpg
Copy the entire  tiles/  folder to the root of the SD card.

Usage:
    python3 download_tiles.py <lat> <lon> <radius_km> <zoom> [output_dir]

Examples:
    # Bondi Beach, 3 km radius, zoom 16 (~600m per tile at this latitude)
    python3 download_tiles.py -33.8915 151.2767 3 16 ./sd_tiles

    # Zoom guide:
    #   15 → ~1200m tiles  (~50-100 tiles for 3 km radius)  good for suburbs
    #   16 → ~600m tiles   (~100-200 tiles for 3 km radius) good for streets
    #   17 → ~300m tiles   (~400-800 tiles for 3 km radius) best detail, large download

Notes:
    - OSM tile servers have rate limits; this script sleeps 100 ms between requests.
    - Tiles are PNG from OSM; this script converts them to JPEG (needs Pillow).
      Install Pillow:  pip install Pillow
    - Re-running skips already-downloaded tiles.
"""

import os
import sys
import math
import time
import urllib.request

OSM_TILE_URL = "https://tile.openstreetmap.org/{z}/{x}/{y}.png"
USER_AGENT   = "GluSticFun/0.1 tile-downloader (educational project)"
JPEG_QUALITY = 80   # 70-85 is a good balance of size and quality for OSM maps


def latlon_to_tile(lat, lon, zoom):
    n = 2 ** zoom
    x = int((lon + 180.0) / 360.0 * n)
    lat_r = math.radians(lat)
    y = int((1.0 - math.log(math.tan(lat_r) + 1.0 / math.cos(lat_r)) / math.pi) / 2.0 * n)
    return x, y


def radius_to_tile_pad(radius_km, zoom, lat):
    """How many tiles away from centre to fetch to cover radius_km."""
    # Approximate: one tile at zoom Z covers ~cos(lat)*40075/2^Z km
    tile_km = abs(math.cos(math.radians(lat))) * 40075.0 / (2 ** zoom)
    return max(1, math.ceil(radius_km / tile_km))


def download_tiles(lat, lon, radius_km, zoom, out_dir):
    try:
        from PIL import Image
        import io as _io
        have_pillow = True
    except ImportError:
        have_pillow = False
        print("WARNING: Pillow not installed — saving raw PNG files.")
        print("         Install with:  pip install Pillow")
        print("         PNG files won't load on the device; JPEG is required.")

    cx, cy = latlon_to_tile(lat, lon, zoom)
    pad    = radius_to_tile_pad(radius_km, zoom, lat)

    x0, x1 = cx - pad, cx + pad
    y0, y1 = cy - pad, cy + pad
    total  = (x1 - x0 + 1) * (y1 - y0 + 1)

    print(f"Centre tile:  {zoom}/{cx}/{cy}")
    print(f"Tile range:   x={x0}..{x1}, y={y0}..{y1}  ({total} tiles)")
    print()

    done = skipped = failed = 0

    for y in range(y0, y1 + 1):
        for x in range(x0, x1 + 1):
            ext  = "jpg" if have_pillow else "png"
            path = os.path.join(out_dir, "tiles", str(zoom), str(x), f"{y}.{ext}")
            os.makedirs(os.path.dirname(path), exist_ok=True)

            if os.path.exists(path):
                skipped += 1
                continue

            url = OSM_TILE_URL.format(z=zoom, x=x, y=y)
            try:
                req = urllib.request.Request(url, headers={"User-Agent": USER_AGENT})
                with urllib.request.urlopen(req, timeout=15) as resp:
                    data = resp.read()

                if have_pillow:
                    import io
                    img = Image.open(io.BytesIO(data)).convert("RGB")
                    buf = io.BytesIO()
                    img.save(buf, "JPEG", quality=JPEG_QUALITY)
                    data = buf.getvalue()

                with open(path, "wb") as f:
                    f.write(data)

                done += 1
                print(f"  [{done + skipped}/{total}]  {zoom}/{x}/{y}  ({len(data)//1024} KB)")
                time.sleep(0.1)   # be polite to OSM tile servers

            except Exception as e:
                failed += 1
                print(f"  FAILED {zoom}/{x}/{y}: {e}")

    print()
    print(f"Done.  Downloaded: {done}  Skipped (cached): {skipped}  Failed: {failed}")
    print(f"Tiles saved to:  {os.path.abspath(out_dir)}/tiles/")
    print()
    print("Next step: copy the 'tiles/' folder to the root of your SD card.")


if __name__ == "__main__":
    if len(sys.argv) < 5:
        print(__doc__)
        sys.exit(1)

    _lat    = float(sys.argv[1])
    _lon    = float(sys.argv[2])
    _radius = float(sys.argv[3])
    _zoom   = int(sys.argv[4])
    _outdir = sys.argv[5] if len(sys.argv) > 5 else "./sd_tiles"

    if not 1 <= _zoom <= 19:
        print("Zoom must be between 1 and 19.")
        sys.exit(1)

    download_tiles(_lat, _lon, _radius, _zoom, _outdir)
