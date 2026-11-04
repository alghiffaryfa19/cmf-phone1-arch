#!/usr/bin/env bash
# Runs ON THE PHONE as alarm. Builds libhybris for the Halium 16 GSI into /opt/libhybris
# (not /usr: its libEGL/libGLESv2 would shadow Mesa for every app).
# Same sources/flags as LuneOS (shr-distribution/meta-smartphone PR #199):
#   headers  Halium/android-headers  herrie/halium-16.0 @ ad5912e (Android 16, API 36)
#   libhybris webOS-ports/libhybris  herrie/android16-tls @ 92b4e61 (A16 TLS linker fix)
set -euxo pipefail
SUDO() { echo "${SUDOPW:-alarm}" | sudo -S -p '' "$@"; }
B=~/build/hybris; PREFIX=/opt/libhybris
SUDO pacman -S --needed --noconfirm base-devel git autoconf automake libtool pkgconf wayland vulkan-headers libglvnd >/dev/null
mkdir -p $B && cd $B
[ -d android-headers ] || git clone -b herrie/halium-16.0 https://github.com/Halium/android-headers
git -C android-headers checkout -q ad5912eee8bb56968af63de46ba3b594510ead3f
[ -d libhybris ] || git clone -b herrie/android16-tls https://github.com/webOS-ports/libhybris
git -C libhybris checkout -q -f 92b4e618cef0b26f38226d1ed00f8a18f853fa03
# our patches (patches/libhybris/ in the project, copied to ~/libhybris-patches)
for p in ~/libhybris-patches/*.patch; do
    [ -e "$p" ] && patch -d libhybris -Np1 -i "$p"
done
cd libhybris/hybris
./autogen.sh
# vulkan.c wraps the NV CUDA entry points, which current vulkan-headers only declare
# with VK_ENABLE_BETA_EXTENSIONS
CFLAGS="-O2 -DVK_ENABLE_BETA_EXTENSIONS" \
./configure --prefix=$PREFIX --with-android-headers=$B/android-headers --enable-mali-quirks --enable-wayland \
    --enable-arch=arm64 --with-default-hybris-ld-library-path=/vendor/lib64:/system/lib64:/odm/lib64
make -j"$(nproc)"
SUDO make install
# The Vulkan wrapper (built when vulkan-headers is installed) must not sit next to libEGL:
# GTK/phoc with LD_LIBRARY_PATH=/opt/libhybris/lib would pick it up and crash. Only
# tetris-llama (llama.cpp on the Mali GPU) uses it.
SUDO install -d /opt/tetris-hybris-vulkan/lib
if compgen -G "$PREFIX/lib/libvulkan.so*" >/dev/null; then
    SUDO sh -c "mv -f $PREFIX/lib/libvulkan.so* /opt/tetris-hybris-vulkan/lib/"
fi
ls $PREFIX/lib | head -30

# Second build as a glvnd EGL vendor (libEGL_libhybris.so.0) for the desktop profile:
# glvnd puts it in front of Mesa (user-environment-generators/60-tetris-hybris-gl), so
# GNOME Shell (EGL surfaceless) and apps (EGL Wayland) get the Mali GPU while Xwayland's
# GBM requests still go to Mesa. Same sources and patches, own prefix.
GPREFIX=/opt/libhybris-glvnd
cd $B && rm -rf libhybris-glvnd && cp -a libhybris libhybris-glvnd
cd libhybris-glvnd/hybris
make distclean >/dev/null 2>&1 || true
CFLAGS="-O2 -DVK_ENABLE_BETA_EXTENSIONS" \
./configure --prefix=$GPREFIX --with-android-headers=$B/android-headers --enable-mali-quirks --enable-wayland \
    --enable-glvnd --enable-arch=arm64 --with-default-hybris-ld-library-path=/vendor/lib64:/system/lib64:/odm/lib64
make -j"$(nproc)"
SUDO make install
SUDO sh -c "rm -f $GPREFIX/lib/libvulkan.*"
ls $GPREFIX/lib/libEGL_libhybris.so.0
