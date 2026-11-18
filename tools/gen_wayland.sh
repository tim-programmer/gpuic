#!/bin/sh
# Makes the Wayland protocol code platform_wayland.c uses (src/wayland/), with wayland-scanner, from the protocols' XML: by default the copies in
# SDL's source (SDL 3.4.20 ships them in wayland-protocols/), else the directory given. The output is committed, so a build needs neither.
#   tools/gen_wayland.sh [dir-with-xml]
set -e
root=$(cd "$(dirname "$0")/.." && pwd)
dir=${1:-$(ls -d "$root"/build*/_deps/gpuic_sdl3-src/wayland-protocols 2>/dev/null | head -1)}
[ -d "$dir" ] || { echo "no protocol XML directory (give one: the wayland-protocols of SDL's source)" >&2; exit 1; }
out="$root/src/wayland"
mkdir -p "$out"
for p in xdg-shell xdg-decoration-unstable-v1 text-input-unstable-v3 fractional-scale-v1 viewporter primary-selection-unstable-v1 cursor-shape-v1 tablet-v2 xdg-foreign-unstable-v2; do
    wayland-scanner client-header "$dir/$p.xml" "$out/$p-client.h"
    wayland-scanner private-code "$dir/$p.xml" "$out/$p.c"
done
