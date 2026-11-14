#!/bin/sh
# Checks (or, with --fix, applies) the project's .clang-format on gpuic's own C and C++ sources: the same list CI checks.
#
#   tools/check_format.sh          exits 1 and names the files that are not formatted
#   tools/check_format.sh --fix    formats them (repeated until nothing changes: aligned comments can move again on a second pass)
#
# Left out: third_party/ (vendored code, kept as upstream wrote it) and generated files (tools/gen_*.py and tools/gen_svg_font.py make
# them; two of them are compared with their generators by ctest). The .clang-format sets InsertBraces: every if, else, for, while and do
# body has braces.
#
# clang-format versions format slightly differently, so the version is pinned: CI installs it with `pip install clang-format==23.1.4`.
# Set CLANG_FORMAT to use another binary.
set -eu
PINNED=23.1.4
CF=${CLANG_FORMAT:-clang-format}
cd "$(dirname "$0")/.."

if ! command -v "$CF" >/dev/null 2>&1; then
    echo "check_format: $CF not found (pip install clang-format==$PINNED)" >&2
    exit 2
fi
have=$("$CF" --version | sed -n 's/.*version \([0-9][0-9.]*\).*/\1/p')
if [ "$have" != "$PINNED" ]; then
    echo "check_format: warning: clang-format $have, the project pins $PINNED: results may differ from CI" >&2
fi

FILES=$(git ls-files '*.c' '*.h' '*.cpp' '*.hpp' \
    | grep -v '^third_party/' \
    | grep -v -x -e 'src/grapheme_data.h' -e 'tests/image_fixtures.h' -e 'tests/svgfont_fixture.h')

if [ "${1:-}" = "--fix" ]; then
    for pass in 1 2 3 4 5; do
        if "$CF" --dry-run --Werror --style=file:.clang-format $FILES >/dev/null 2>&1; then
            exit 0
        fi
        "$CF" -i --style=file:.clang-format $FILES
    done
    echo "check_format: still changing after 5 passes" >&2
    exit 1
fi

if ! "$CF" --dry-run --Werror --style=file:.clang-format $FILES 2>/dev/null; then
    echo "check_format: these files are not formatted (run tools/check_format.sh --fix):" >&2
    for f in $FILES; do
        "$CF" --dry-run --Werror --style=file:.clang-format "$f" >/dev/null 2>&1 || echo "  $f" >&2
    done
    exit 1
fi
echo "check_format: all formatted"
