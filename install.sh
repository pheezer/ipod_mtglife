#!/bin/sh
# Install the "mtglife" plugin into a Rockbox source tree.
#
# Usage: ./install.sh /path/to/rockbox
#
# This copies mtglife.c into apps/plugins/ and registers it in the build
# (apps/plugins/SOURCES and apps/plugins/CATEGORIES). It is idempotent --
# running it again just refreshes the source file.
#
# Afterwards, configure and build Rockbox as usual (see README.md).

set -eu

RB="${1:-}"
if [ -z "$RB" ] || [ ! -d "$RB/apps/plugins" ]; then
    echo "Usage: $0 /path/to/rockbox   (a Rockbox source checkout)" >&2
    exit 1
fi

here="$(cd "$(dirname "$0")" && pwd)"

cp "$here/mtglife.c" "$RB/apps/plugins/mtglife.c"
echo "copied mtglife.c -> $RB/apps/plugins/mtglife.c"

if ! grep -q '^mtglife\.c' "$RB/apps/plugins/SOURCES"; then
    printf 'mtglife.c\n' >> "$RB/apps/plugins/SOURCES"
    echo "registered mtglife.c in apps/plugins/SOURCES"
else
    echo "already present in apps/plugins/SOURCES"
fi

if ! grep -q '^mtglife,' "$RB/apps/plugins/CATEGORIES"; then
    printf 'mtglife,games\n' >> "$RB/apps/plugins/CATEGORIES"
    echo "registered mtglife,games in apps/plugins/CATEGORIES"
else
    echo "already present in apps/plugins/CATEGORIES"
fi

echo
echo "Done. Next, from your Rockbox checkout:"
echo "  mkdir build-ipod && cd build-ipod"
echo "  ../tools/configure --target=ipodvideo --type=n"
echo "  make -j rocks"
echo "  cp apps/plugins/mtglife.rock \"/Volumes/<YOUR IPOD>/.rockbox/rocks/games/\""
