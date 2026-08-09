#!/usr/bin/env python3
"""Build the installable .ts3_plugin package.

A .ts3_plugin file is just a ZIP archive containing:
    package.ini                     (metadata shown in the install dialog)
    plugins/funplugin_win64.dll     (the plugin binary)

Double-clicking the resulting file makes the TeamSpeak client show its
"TeamSpeak Add-On" install dialog and copy the DLL into place.
"""
import os
import sys
import zipfile

ROOT = os.path.dirname(os.path.abspath(__file__))
DLL = os.path.join(ROOT, "funplugin_win64.dll")
INI = os.path.join(ROOT, "package.ini")
OUT_DIR = os.path.join(ROOT, "dist")
OUT = os.path.join(OUT_DIR, "ts3-fun-plugin.ts3_plugin")

def main():
    for path in (DLL, INI):
        if not os.path.isfile(path):
            print(f"[ERROR] missing: {path}")
            return 1

    os.makedirs(OUT_DIR, exist_ok=True)
    # arcname uses forward slashes so the TS3 (Qt) installer can read it
    with zipfile.ZipFile(OUT, "w", zipfile.ZIP_DEFLATED) as z:
        z.write(INI, "package.ini")
        z.write(DLL, "plugins/funplugin_win64.dll")

    print(f"Built {OUT} ({os.path.getsize(OUT)} bytes)")
    print("Contents:")
    with zipfile.ZipFile(OUT) as z:
        for name in z.namelist():
            print(f"  {name}")
    return 0

if __name__ == "__main__":
    sys.exit(main())
