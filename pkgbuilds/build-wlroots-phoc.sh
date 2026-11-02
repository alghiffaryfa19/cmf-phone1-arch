#!/usr/bin/env bash
# Runs ON THE PHONE as alarm. Rebuilds Arch's wlroots0.20 with phoc's layer-shell patch:
# phoc 0.57 embeds wlroots 0.20.2 + "Revert layer-shell: error on 0 dimension without anchors",
# but Arch links phoc against the unpatched system wlroots0.20, so phosh's home bar
# (height 0, anchored bottom|left|right) gets a protocol error and phosh dies.
set -euxo pipefail
SUDO() { echo "${SUDOPW:-alarm}" | sudo -S -p '' "$@"; }
PATCH=0001-Revert-layer-shell-error-on-0-dimension-without-anch.patch
# Our hwcomposer backend + hybris gralloc allocator (patches/wlroots/ in the project),
# copied next to this script by the caller. Enabled with -Dhwcomposer=enabled; libhybris
# is only dlopen()ed when WLR_BACKENDS=hwcomposer, so the normal DRM path is unchanged.
HWC_PATCH=0002-hwcomposer-backend-hybris-allocator.patch

SUDO pacman -S --needed --noconfirm base-devel git
rm -rf ~/build/wlroots0.20 && mkdir -p ~/build && cd ~/build
git clone --depth 1 https://gitlab.archlinux.org/archlinux/packaging/packages/wlroots0.20.git
cd wlroots0.20
curl -fL -o "$PATCH" "https://gitlab.gnome.org/World/Phosh/phoc/-/raw/v0.57.0/subprojects/packagefiles/wlroots/$PATCH"
cp ~/"$HWC_PATCH" .

# Add the patch: source entry + SKIP checksum + apply in prepare()
python3 - "$PATCH" "$HWC_PATCH" <<'PY'
import re, sys
patch, hwc_patch = sys.argv[1], sys.argv[2]
s = open('PKGBUILD').read()
s = re.sub(r'^pkgrel=(\S+)', lambda m: f'pkgrel={m.group(1)}.2', s, count=1, flags=re.M)
s = re.sub(r'^(source=\()', rf'\1"{patch}" "{hwc_patch}"\n  ', s, count=1, flags=re.M)
s = re.sub(r'^((?:b2|sha256|sha512)sums=\()', r"\1'SKIP' 'SKIP'\n  ", s, count=1, flags=re.M)
s = re.sub(r'(arch-meson\s+"?\$\{?pkgname\}?"?\s+build)', r'\1 -Dhwcomposer=enabled', s, count=1)
if re.search(r'^prepare\(\)\s*\{', s, re.M):
    s = re.sub(r'^(prepare\(\)\s*\{\n)', rf'\1  patch -d "$srcdir/$pkgname" -Np1 -i "$srcdir/{patch}"\n  patch -d "$srcdir/$pkgname" -Np1 -i "$srcdir/{hwc_patch}"\n', s, count=1, flags=re.M)
else:
    s = re.sub(r'^(build\(\)\s*\{)', rf'prepare() {{\n  patch -d "$srcdir/$pkgname" -Np1 -i "$srcdir/{patch}"\n  patch -d "$srcdir/$pkgname" -Np1 -i "$srcdir/{hwc_patch}"\n}}\n\n\1', s, count=1, flags=re.M)
open('PKGBUILD', 'w').write(s)
PY
grep -nE '^pkgrel|SKIP|patch|^source' PKGBUILD

# deps (makepkg -s would need an interactive sudo)
deps=$(makepkg --printsrcinfo | awk -F' = ' '/^\t(make)?depends/{print $2}' | sed 's/[<>=].*//' | sort -u)
SUDO pacman -S --needed --noconfirm $deps
# Arch packaging repo lists only x86_64
makepkg -f -A --skippgpcheck --nocheck
SUDO pacman -U --noconfirm ./wlroots0.20-*.pkg.tar.*
pacman -Q wlroots0.20
