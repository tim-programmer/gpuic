NanoSVG (nanosvg.h, nanosvgrast.h) from https://github.com/memononen/nanosvg (zlib license, LICENSE.txt), master as fetched.

Upstream SHA-256:
  e34fd5d084be106cea972d19ce5d27fd96d17ba89f8d06bdceee058420c8b2b0  nanosvg.h
  79a9c5f4db19debf9f3a648a1589e96d92854f245a5cb4f3d823f263785234d8  nanosvgrast.h   (unmodified)

One local change, in nanosvg.h (search "GPUIC PATCH"): with gradientUnits="objectBoundingBox" (the default) a plain number
in x1/y1/x2/y2/cx/cy/fx/fy/r is a fraction of the shape's box, as the SVG specification says. Upstream treated it as a length
in user units and only handled percentages, which turned the usual `x2="1"` into a gradient one pixel long. Patched file:
  f63c97393326f7d5d139fbf8338687a5c5a4c5a3b1c964e5340de7c625e3bf04  nanosvg.h

tests/svg_test.c checks gradients written with fractions, so updating nanosvg shows if the patch is still needed.
Used by src/svg.c only.
