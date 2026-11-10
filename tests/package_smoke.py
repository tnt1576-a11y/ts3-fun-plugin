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


def inspect_dialog(dll, preview, resource=101, channel=False, copies=False):
    user.CreateDialogParamW.argtypes = [w.HINSTANCE, w.LPCWSTR, w.HWND, DialogProc, w.LPARAM]
    user.CreateDialogParamW.restype = w.HWND
    user.GetDlgItem.argtypes = [w.HWND, c.c_int]
    user.GetDlgItem.restype = w.HWND
    user.SetDlgItemTextW.argtypes = [w.HWND, c.c_int, w.LPCWSTR]
    user.SetWindowTextW.argtypes = [w.HWND, w.LPCWSTR]
    user.DestroyWindow.argtypes = [w.HWND]
    user.GetWindowRect.argtypes = [w.HWND, c.POINTER(w.RECT)]

    @DialogProc
    def procedure(window, message, wp, lp):
        return 0

    # A hidden dialog instantiated from the DLL's actual compiled resource.
    window = user.CreateDialogParamW(dll._handle, c.cast(c.c_void_p(resource), w.LPCWSTR), None, procedure, 0)
    assert window, c.get_last_error()
    try:
        controls = {100: (1001, 1010, 1020, 1, 2),
                    101: (1200, 1201, 1202, 1203, 1, 2),
                    102: (1210, 1211, 1212, 1213, 1215, 1216, 1217, 1, 2),
                    103: (1220, 1221, 1222, 1223, 1, 2)}[resource]
        for control in controls:
            assert user.GetDlgItem(window, control), f"Missing dialog control {control}"
        values = {100: ((1001, "0"), (1010, "0.001")),
                  101: ((1200, "5"), (1201, "Player"), (1202, "test password"), (1203, "Lobby / Party channel")),
                  102: ((1210, "Selected friend"), (1213, "6"), (1215, "75")),
                  103: ((1220, "Selected friend"), (1221, "25"), (1222, "Hello!"),
                        (1223, "Send this many pokes immediately, without the Settings timer.\nHARD STOP cancels requests that have not been submitted."))}[resource]
        for control, value in values:
            assert user.SetDlgItemTextW(window, control, value)
        if copies:
            assert user.SetWindowTextW(window, "Fun Plugin - Message from my copies")
            assert user.SetDlgItemTextW(window, 1221, "7")
            assert user.SetDlgItemTextW(window, 1223, "3 connected copies. Amount is PER COPY.\nYour original tab is not a sender.")
        if channel:
            assert user.SetWindowTextW(window, "Fun Plugin - Channel Talk Back")
            assert user.SetDlgItemTextW(window, 1210, "Lobby / Party channel")
            assert user.SetDlgItemTextW(window, 1216, "Echo channel:")
            assert user.SetDlgItemTextW(window, 1217, "Live pitched echo through your current user.\nEveryone in this channel hears it as normal voice.\nYour microphone is replaced by the echo until Stop.")
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
                name = "settings-dialog" if resource == 100 else "direct-send-dialog" if resource == 103 else "copies-dialog" if resource == 101 else "channel-talk-back-dialog" if channel else "talk-back-dialog"
                if copies:
                    name = "copy-send-dialog"
                output = ROOT / f"tests/bin/{name}.png"
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
        assert b"Version = 2.17" in archive.read("package.ini")
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
            assert dll.ts3plugin_version() == b"2.17"
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
            for i in range(35):
                if not menus[i]:
                    break
                labels.append(menus[i].contents.text.decode())
                dll.ts3plugin_freeMemory(menus[i])
            dll.ts3plugin_freeMemory(menus)
            assert len(labels) == 34
            for action in ("Poke", "Random Move", "Chaos"):
                assert f"{action} from copies: Start..." in labels
                assert f"{action} from copies: Stop" in labels
            assert "Poke Pack..." in labels
            assert "Poke from my copies..." in labels
            assert "Message from my copies..." in labels
            assert "Talk Back: Start..." in labels
            assert labels.count("Talk Back: Stop") == 2
            assert labels.count("Talk Back: Channel...") == 2
            for export in ("ts3plugin_onEditPlaybackVoiceDataEvent", "ts3plugin_onEditCapturedVoiceDataEvent", "ts3plugin_onServerErrorEvent"):
                assert getattr(dll, export)
            assert "Make copies of myself..." in labels
            assert "Make copies of myself here..." in labels
            assert "Remove my copies (cancel queue)" in labels
            inspect_dialog(dll, "--preview" in sys.argv)
            inspect_dialog(dll, "--preview" in sys.argv, 102)
            inspect_dialog(dll, "--preview" in sys.argv, 102, channel=True)
            inspect_dialog(dll, "--preview" in sys.argv, 103)
            inspect_dialog(dll, "--preview" in sys.argv, 103, copies=True)
            inspect_dialog(dll, "--preview" in sys.argv, 100)
        finally:
            if initialized:
                dll.ts3plugin_shutdown()
            kernel.FreeLibrary.argtypes = [w.HMODULE]
            assert kernel.FreeLibrary(dll._handle)
    print("PASS: exact packaged DLL loads, API/version, lifecycle, 34 menus, audio exports, native dialogs, archive integrity")
    print("DLL SHA-256:", hashlib.sha256(binary).hexdigest())


if __name__ == "__main__":
    main()
