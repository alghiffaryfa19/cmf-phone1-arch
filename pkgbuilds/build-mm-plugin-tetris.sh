#!/usr/bin/env bash
# Runs ON THE PHONE as alarm. Builds our ModemManager plugin (patches/modemmanager/tetris,
# copied to ~/mm-plugin-tetris by the caller) from the source tag of the installed
# ModemManager and installs only the plugin .so. Rebuild after a ModemManager upgrade.
set -euxo pipefail
SUDO() { echo "${SUDOPW:-alarm}" | sudo -S -p '' "$@"; }
VER=$(pacman -Q modemmanager | awk '{print $2}' | cut -d- -f1)
SUDO pacman -S --needed --noconfirm base-devel git meson glib2-devel libmbim libqmi libqrtr-glib libgudev
mkdir -p ~/build && cd ~/build
[ -d ModemManager ] || git clone --depth 1 -b "$VER" https://gitlab.freedesktop.org/mobile-broadband/ModemManager.git
cd ModemManager
git checkout -q -f "$VER"; rm -rf src/plugins/tetris
cp -r ~/mm-plugin-tetris src/plugins/tetris
python3 - <<'PY'
s = open('src/plugins/meson.build').read()
marker = "# plugin: altair lte\n"
block = """# plugin: tetris (CMF Phone 1)
common_c_args = '-DMM_MODULE_NAME="tetris"'
plugins += {'plugin-tetris': {
  'plugin': true,
  'module': {'sources': files('tetris/mm-broadband-bearer-tetris.c', 'tetris/mm-broadband-modem-tetris.c', 'tetris/mm-plugin-tetris.c'), 'include_directories': plugins_incs + [include_directories('tetris')], 'c_args': common_c_args},
}}

"""
assert marker in s
open('src/plugins/meson.build', 'w').write(s.replace(marker, block + marker, 1))
PY
[ -f build/build.ninja ] || meson setup build --prefix=/usr -Dgtk_doc=false -Dvapi=false -Dintrospection=false -Dtests=false -Dbash_completion=false
ninja -C build src/plugins/libmm-plugin-tetris.so
SUDO install -m755 build/src/plugins/libmm-plugin-tetris.so /usr/lib/ModemManager/
