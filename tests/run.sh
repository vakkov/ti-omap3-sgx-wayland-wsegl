#!/bin/sh
set -eu

if [ "${1:-}" = --sanitize ]; then SANITIZE=1; fi

cd "$(dirname "$0")/.."
build_dir=$(mktemp -d "${TMPDIR:-/tmp}/wsegl-tests.XXXXXX")
trap 'rm -rf "$build_dir"' EXIT HUP INT TERM

wayland-scanner client-header common/wayland-sgx.xml "$build_dir/wayland-sgx-client-protocol.h"
wayland-scanner server-header common/wayland-sgx.xml "$build_dir/wayland-sgx-server-protocol.h"
wayland-scanner private-code common/wayland-sgx.xml "$build_dir/wayland-sgx-protocol.c"

for mode in debug release; do
    defines=
    if [ "$mode" = release ]; then defines=-DNDEBUG; fi
    # SANITIZE=1 enables AddressSanitizer, UndefinedBehaviorSanitizer and the
    # compiler's default leak check.  Some sandboxes cannot run LeakSanitizer.
    sanitizers=
    if [ "${SANITIZE:-0}" = 1 ]; then
        sanitizers='-fsanitize=address,undefined -fno-omit-frame-pointer'
    fi
    "${CC:-cc}" -std=gnu99 -g -O1 $defines $sanitizers \
        -Werror=implicit-function-declaration -Werror=return-type \
        -I"$build_dir" -Idoc/reference -Ilibwayland-egl -Icommon \
        tests/render-sync.c common/log.c "$build_dir/wayland-sgx-protocol.c" \
        $(pkg-config --cflags --libs wayland-client) -o "$build_dir/render-sync"
    "$build_dir/render-sync"

    "${CC:-cc}" -std=gnu99 -g -O1 $defines $sanitizers \
        -Werror=implicit-function-declaration -Werror=return-type \
        -pthread \
        tests/real-roundtrip.c $(pkg-config --cflags --libs wayland-client wayland-server) \
        -o "$build_dir/real-roundtrip"
    timeout 20 "$build_dir/real-roundtrip"
done

"${CXX:-c++}" -fsyntax-only -Werror=return-type -include tests/legacy-wayland.h \
    -I"$build_dir" -Icommon $(pkg-config --cflags wayland-server) common/server_wlegl_buffer.cpp
