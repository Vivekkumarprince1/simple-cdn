#!/bin/sh
set -eu

project_dir=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
cd "$project_dir"

CXX=${CXX:-c++}
CXXFLAGS=${CXXFLAGS:--std=c++17 -O2 -Wall -Wextra -Wpedantic -pthread}
LDLIBS=${LDLIBS:-}

if command -v pkg-config >/dev/null 2>&1 && pkg-config --exists libmaxminddb; then
  CXXFLAGS="$CXXFLAGS $(pkg-config --cflags libmaxminddb) -DCDN_WITH_MAXMINDDB"
  LDLIBS="$LDLIBS $(pkg-config --libs libmaxminddb)"
fi

echo "Building simple-cdn..."
# shellcheck disable=SC2086
$CXX $CXXFLAGS src/main.cpp src/config.cpp src/geoip.cpp -o simple-cdn $LDLIBS