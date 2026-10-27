"""Load the exact packaged DLL with an isolated config and inspect its native UI.

Usage: python tests/package_smoke.py [--preview]
No TeamSpeak process, microphone, server, or real client settings are used.
"""
import ctypes as c
from ctypes import wintypes as w
import hashlib
from pathlib import Path
import re
import sys
import tempfile
import zipfile

ROOT = Path(__file__).resolve().parents[1]
user = c.WinDLL("user32", use_last_error=True)
kernel = c.WinDLL("kernel32", use_last_error=True)
gdi = c.WinDLL("gdi32", use_last_error=True)
DialogProc = c.WINFUNCTYPE(c.c_ssize_t, w.HWND, w.UINT, w.WPARAM, w.LPARAM)


def inspect_dialog(dll, preview):
    user.CreateDialogParamW.argtypes = [w.HINSTANCE, w.LPCWSTR, w.HWND, DialogProc, w.LPARAM]
    user.CreateDialogParamW.restype = w.HWND
    user.GetDlgItem.argtypes = [w.HWND, c.c_int]
    user.GetDlgItem.restype = w.HWND
    user.SetDlgItemTextW.argtypes = [w.HWND, c.c_int, w.LPCWSTR]
    user.DestroyWindow.argtypes = [w.HWND]
    user.GetWindowRect.argtypes = [w.HWND, c.POINTER(w.RECT)]

    @DialogProc
    def procedure(window, message, wp, lp):
        return 0

    # A hidden dialog instantiated from the DLL's actual compiled resource.
    window = user.CreateDialogParamW(dll._handle, c.cast(c.c_void_p(101), w.LPCWSTR), None, procedure, 0)
    assert window, c.get_last_error()
    try:
        for control in (1200, 1201, 1202, 1203, 1, 2):
            assert user.GetDlgItem(window, control), f"Missing copy dialog control {control}"
        for control, value in ((1200, "5"), (1201, "Player"), (1202, "test password"), (1203, "Lobby / Party channel")):
            assert user.SetDlgItemTextW(window, control, value)
        if preview:
            from PIL import Image

            # Render off-screen: hidden native windows may return an all-black PrintWindow image.
            user.SetWindowPos.argtypes = [w.HWND, w.HWND, c.c_int, c.c_int, c.c_int, c.c_int, w.UINT]
            user.ShowWindow.argtypes = [w.HWND, c.c_int]
            user.UpdateWindow.argtypes = [w.HWND]
            user.SetWindowPos(window, None, -10000, -10000, 0, 0, 0x0015)
            user.ShowWindow(window, 4)
            user.UpdateWindow(window)

            user.GetDC.argtypes = [w.HWND]
            user.GetDC.restype = w.HDC
            user.ReleaseDC.argtypes = [w.HWND, w.HDC]
            user.PrintWindow.argtypes = [w.HWND, w.HDC, w.UINT]
            gdi.CreateCompatibleDC.argtypes = [w.HDC]
            gdi.CreateCompatibleDC.restype = w.HDC
            gdi.CreateCompatibleBitmap.argtypes = [w.HDC, c.c_int, c.c_int]
            gdi.CreateCompatibleBitmap.restype = w.HBITMAP
            gdi.SelectObject.argtypes = [w.HDC, w.HGDIOBJ]
            gdi.SelectObject.restype = w.HGDIOBJ
            gdi.GetDIBits.argtypes = [w.HDC, w.HBITMAP, w.UINT, w.UINT, c.c_void_p, c.c_void_p, w.UINT]
            gdi.DeleteObject.argtypes = [w.HGDIOBJ]
            gdi.DeleteDC.argtypes = [w.HDC]

            class BitmapInfo(c.Structure):
                _fields_ = [("size", w.DWORD), ("width", w.LONG), ("height", w.LONG),
                            ("planes", w.WORD), ("bits", w.WORD), ("compression", w.DWORD),
                            ("image_size", w.DWORD), ("xppm", w.LONG), ("yppm", w.LONG),
                            ("used", w.DWORD), ("important", w.DWORD)]

            rect = w.RECT()
            assert user.GetWindowRect(window, c.byref(rect))
            width, height = rect.right - rect.left, rect.bottom - rect.top
            dc = user.GetDC(window)
            memory = gdi.CreateCompatibleDC(dc)
            bitmap = gdi.CreateCompatibleBitmap(dc, width, height)
            previous = gdi.SelectObject(memory, bitmap)
            try:
                assert user.PrintWindow(window, memory, 0)
                gdi.SelectObject(memory, previous)
                info = BitmapInfo(c.sizeof(BitmapInfo), width, -height, 1, 32)
                pixels = c.create_string_buffer(width * height * 4)
                assert gdi.GetDIBits(memory, bitmap, 0, height, pixels, c.byref(info), 0)
                assert len(set(pixels.raw)) > 8, "Dialog preview is blank"
                output = ROOT / "tests/bin/copies-dialog.png"
                output.parent.mkdir(exist_ok=True)
                Image.frombytes("RGB", (width, height), pixels.raw, "raw", "BGRX").save(output)
                print(f"Dialog preview: {output}")
            finally:
                gdi.DeleteObject(bitmap)
                gdi.DeleteDC(memory)
                user.ReleaseDC(window, dc)
    finally:
        user.DestroyWindow(window)


def main():
    with zipfile.ZipFile(ROOT / "dist/ts3-fun-plugin.ts3_plugin") as archive:
        assert archive.namelist() == ["package.ini", "plugins/funplugin_win64.dll"]
        assert archive.testzip() is None
        assert b"Version = 2.10" in archive.read("package.ini")
        binary = archive.read("plugins/funplugin_win64.dll")
        assert binary == (ROOT / "funplugin_win64.dll").read_bytes()

    fields = re.findall(r"\(\*(\w+)\)", (ROOT / "sdk/include/ts3_functions.h").read_text())

    class Functions(c.Structure):
        _fields_ = [(name, c.c_void_p) for name in fields]

    class Menu(c.Structure):
        _fields_ = [("type", c.c_int), ("id", c.c_int), ("text", c.c_char * 128), ("icon", c.c_char * 128)]

    scratch = (ROOT / "tests/bin").resolve()
    scratch.mkdir(exist_ok=True)
    with tempfile.TemporaryDirectory(prefix="package-smoke-", dir=scratch) as temporary:
        assert Path(temporary).resolve().is_relative_to(scratch)
        path = Path(temporary) / "funplugin_win64.dll"
        path.write_bytes(binary)
        dll = c.WinDLL(str(path))
        initialized = False
        try:
            dll.ts3plugin_version.restype = c.c_char_p
            assert dll.ts3plugin_version() == b"2.10"
            assert dll.ts3plugin_apiVersion() == 26
            config = (temporary + "\\").encode("utf-8") + b"\0"

            @c.CFUNCTYPE(None, c.c_void_p, c.c_size_t)
            def config_path(buffer, size):
                assert len(config) <= size
                c.memmove(buffer, config, len(config))

            functions = Functions()
            functions.getConfigPath = c.cast(config_path, c.c_void_p).value
            dll.ts3plugin_setFunctionPointers.argtypes = [Functions]
            dll.ts3plugin_setFunctionPointers(functions)
            assert dll.ts3plugin_init() == 0
            initialized = True
            menus = c.POINTER(c.POINTER(Menu))()
            icon = c.c_void_p()
            dll.ts3plugin_initMenus.argtypes = [c.POINTER(type(menus)), c.POINTER(c.c_void_p)]
            dll.ts3plugin_freeMemory.argtypes = [c.c_void_p]
            dll.ts3plugin_initMenus(c.byref(menus), c.byref(icon))
            labels = []
            for i in range(21):
                if not menus[i]:
                    break
                labels.append(menus[i].contents.text.decode())
                dll.ts3plugin_freeMemory(menus[i])
            dll.ts3plugin_freeMemory(menus)
            assert len(labels) == 20
            assert "Make copies of myself..." in labels
            assert "Make copies of myself here..." in labels
            assert "Remove my copies (cancel queue)" in labels
            inspect_dialog(dll, "--preview" in sys.argv)
        finally:
            if initialized:
                dll.ts3plugin_shutdown()
            kernel.FreeLibrary.argtypes = [w.HMODULE]
            assert kernel.FreeLibrary(dll._handle)
    print("PASS: exact packaged DLL loads, API/version, lifecycle, 20 menus, native dialog controls, archive integrity")
    print("DLL SHA-256:", hashlib.sha256(binary).hexdigest())


if __name__ == "__main__":
    main()
