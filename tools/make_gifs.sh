#!/bin/sh
# make_gifs.sh — renders the README's GIFs into docs/.
# Needs ffmpeg and a native build (cmake -B build && cmake --build build).
# Usage: tools/make_gifs.sh [sequence...]   (default: orbit ps1 liquid views)
set -e
cd "$(dirname "$0")/.."
RECORD=build/tessera_record
[ -x "$RECORD" ] || { echo "build first: cmake --build build"; exit 1; }
mkdir -p docs
TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT

for seq in ${*:-orbit ps1 liquid views}; do
    mkdir -p "$TMP/$seq"
    "$RECORD" "$seq" "$TMP/$seq"
    # 960 -> 640 wide is exactly 2/3, so PS1's 3x3 pixel blocks become clean 2x2
    # blocks with nearest-neighbor scaling; everything else gets lanczos.
    # 64 colors looks the same as 256 on these smooth gradients at ~40% less size;
    # PS1 keeps 256 so its dither pattern survives exactly.
    case $seq in ps1) flags=neighbor; colors=256 ;; *) flags=lanczos; colors=64 ;; esac
    case $seq in liquid) fps=30 ;; *) fps=20 ;; esac
    # Two-pass palette: build one palette for the whole clip, then map every frame
    # to it. Much cleaner than ffmpeg's default per-GIF palette.
    ffmpeg -loglevel error -y -framerate $fps -i "$TMP/$seq/frame_%04d.ppm" \
        -vf "scale=640:-1:flags=$flags,split[a][b];[a]palettegen=max_colors=$colors:stats_mode=diff[p];[b][p]paletteuse=dither=bayer:bayer_scale=5:diff_mode=rectangle" \
        -loop 0 "docs/$seq.gif"
    echo "docs/$seq.gif  $(du -h "docs/$seq.gif" | cut -f1)"
done
