#!/usr/bin/env bash
# Runs ON THE PHONE as alarm. llama.cpp with the Vulkan backend on the Mali-G615 through
# libhybris (the Mali Vulkan driver from /vendor), installed to /opt/llama.cpp.
# Needs libhybris (build-libhybris.sh, which also puts its Vulkan wrapper into
# /opt/tetris-hybris-vulkan) and the Halium container for /vendor at run time.
# Run models with tetris-llama (rootfs/overlay-server/usr/local/bin).
set -euxo pipefail
SUDO() { echo "${SUDOPW:-alarm}" | sudo -S -p '' "$@"; }
B=~/build; PREFIX=/opt/llama.cpp
REV=5851a8e   # ggml-org/llama.cpp, 2026-11-04
SUDO pacman -S --needed --noconfirm base-devel git cmake shaderc vulkan-headers vulkan-icd-loader >/dev/null
mkdir -p $B && cd $B
[ -d llama.cpp ] || git clone https://github.com/ggml-org/llama.cpp
git -C llama.cpp fetch -q origin && git -C llama.cpp checkout -q -f $REV
cd llama.cpp
# Links against the system Vulkan loader; tetris-llama swaps in libhybris' libvulkan at run time
cmake -B build -DGGML_VULKAN=ON -DGGML_NATIVE=ON -DLLAMA_CURL=OFF -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_INSTALL_PREFIX=$PREFIX -DCMAKE_INSTALL_RPATH=$PREFIX/lib
cmake --build build -j"$(nproc)"
SUDO cmake --install build
ls $PREFIX/bin
