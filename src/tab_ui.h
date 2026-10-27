#ifndef FUNPLUGIN_TAB_UI_H
#define FUNPLUGIN_TAB_UI_H

#include <windows.h>
#include <cstring>
#include <map>
#include <set>

/* TS3 has no plugin API for selecting/closing tabs. Use the Qt 5 functions
 * already loaded by the client, without linking another Qt runtime or reading
 * private Qt/TeamSpeak object layouts. Calls stay on the menu/timer GUI thread.
 * Every symbol and the named, typed tab widget must exist before enabling this.
 * Objects/QString are constructed and destroyed by the client's own Qt DLL.
 */
class CopyTabUI {
    using Object = void*;
    using StringCtor = void (*)(void*, const wchar_t*, int);
    using StringDtor = void (*)(void*);
    using EmptyStringCtor = void (*)(void*);
    using Inherits = bool (*)(Object, const char*);
    using FindWidget = Object (*)(UINT_PTR);
    using WinId = UINT_PTR (*)(Object);
    using FindChild = Object (*)(Object, const void*, const void*, int);
    using CurrentWidget = Object (*)(Object);
    using TabWidget = Object (*)(Object, int);
    using TabBar = Object (*)(Object);
    using TabButton = Object (*)(Object, int, int);
    using IndexOf = int (*)(Object, Object);
    using Count = int (*)(Object);
    using Closable = bool (*)(Object);
    using Select = void (*)(Object, Object);
    using Updates = void (*)(Object, bool);
    using UpdatesEnabled = bool (*)(Object);
    // QGenericArgument's Qt 5.15 public-header layout: data pointer, then name.
    struct Argument {
        void* data;
        const char* type;
        Argument(const char* name, void* value) : data(value), type(name) {}
    };
    /* Windows x64 passes these 16-byte Qt argument values by reference. */
    using Invoke = bool (*)(Object, const char*, int, const Argument*, const Argument*,
                            const Argument*, const Argument*, const Argument*, const Argument*,
                            const Argument*, const Argument*, const Argument*, const Argument*, const Argument*);
    StringCtor stringCtor = nullptr;
    StringDtor stringDtor = nullptr;
    EmptyStringCtor emptyStringCtor = nullptr;
    Inherits inherits = nullptr;
    FindWidget findWidget = nullptr;
    WinId winId = nullptr;
    FindChild findChild = nullptr;
    CurrentWidget current = nullptr;
    TabWidget widget = nullptr;
    TabBar tabBar = nullptr;
    TabButton tabButton = nullptr;
    IndexOf indexOf = nullptr;
    Count count = nullptr;
    Closable closable = nullptr;
    Select select = nullptr;
    Updates updates = nullptr;
    UpdatesEnabled updatesEnabled = nullptr;
    Invoke invoke = nullptr;
    const void* tabMeta = nullptr;
    const void* widgetMeta = nullptr;
    Object tabs = nullptr, previous = nullptr;
    bool repaint = false;
    std::set<Object> before;
    struct Tab { Object page, previous; HWND window; UINT_PTR tag; };
    std::map<unsigned long long, Tab> copies;
    UINT_PTR nextTag = (UINT_PTR)GetTickCount64() * 0x9e3779b97f4a7c15ULL;
    const char* failure = "No TeamSpeak server tab control found on the GUI thread.";

    template<class T> static T symbol(HMODULE module, const char* name) {
        FARPROC address = GetProcAddress(module, name);
        T result = nullptr;
        static_assert(sizeof(result) == sizeof(address), "Windows x64 function pointer size");
        std::memcpy(&result, &address, sizeof(result));
        return result;
    }

    bool load() {
        HMODULE core = GetModuleHandleW(L"Qt5Core.dll"), widgets = GetModuleHandleW(L"Qt5Widgets.dll");
        if (!core || !widgets) return false;
        auto version = symbol<const char* (*)()>(core, "qVersion");
        if (!version || std::strncmp(version(), "5.15.", 5) != 0) return false;
        stringCtor = symbol<StringCtor>(core, "??0QString@@QEAA@PEBVQChar@@H@Z");
        stringDtor = symbol<StringDtor>(core, "??1QString@@QEAA@XZ");
        emptyStringCtor = symbol<EmptyStringCtor>(core, "??0QString@@QEAA@XZ");
        inherits = symbol<Inherits>(core, "?inherits@QObject@@QEBA_NPEBD@Z");
        findWidget = symbol<FindWidget>(widgets, "?find@QWidget@@SAPEAV1@_K@Z");
        winId = symbol<WinId>(widgets, "?winId@QWidget@@QEBA_KXZ");
        findChild = symbol<FindChild>(core, "?qt_qFindChild_helper@@YAPEAVQObject@@PEBV1@AEBVQString@@AEBUQMetaObject@@V?$QFlags@W4FindChildOption@Qt@@@@@Z");
        current = symbol<CurrentWidget>(widgets, "?currentWidget@QTabWidget@@QEBAPEAVQWidget@@XZ");
        widget = symbol<TabWidget>(widgets, "?widget@QTabWidget@@QEBAPEAVQWidget@@H@Z");
        tabBar = symbol<TabBar>(widgets, "?tabBar@QTabWidget@@QEBAPEAVQTabBar@@XZ");
        tabButton = symbol<TabButton>(widgets, "?tabButton@QTabBar@@QEBAPEAVQWidget@@HW4ButtonPosition@1@@Z");
        indexOf = symbol<IndexOf>(widgets, "?indexOf@QTabWidget@@QEBAHPEAVQWidget@@@Z");
        count = symbol<Count>(widgets, "?count@QTabWidget@@QEBAHXZ");
        closable = symbol<Closable>(widgets, "?tabsClosable@QTabWidget@@QEBA_NXZ");
        select = symbol<Select>(widgets, "?setCurrentWidget@QTabWidget@@QEAAXPEAVQWidget@@@Z");
        updates = symbol<Updates>(widgets, "?setUpdatesEnabled@QWidget@@QEAAX_N@Z");
        updatesEnabled = symbol<UpdatesEnabled>(widgets, "?updatesEnabled@QWidget@@QEBA_NXZ");
        invoke = symbol<Invoke>(core, "?invokeMethod@QMetaObject@@SA_NPEAVQObject@@PEBDW4ConnectionType@Qt@@VQGenericReturnArgument@@VQGenericArgument@@444444444@Z");
        tabMeta = reinterpret_cast<const void*>(GetProcAddress(widgets, "?staticMetaObject@QTabWidget@@2UQMetaObject@@B"));
        widgetMeta = reinterpret_cast<const void*>(GetProcAddress(widgets, "?staticMetaObject@QWidget@@2UQMetaObject@@B"));
        return stringCtor && stringDtor && emptyStringCtor && inherits && findWidget && winId && findChild &&
               current && widget && tabBar && tabButton && indexOf && count && closable && select && updates && updatesEnabled && invoke && tabMeta && widgetMeta;
    }

    Object child(Object root) {
        // Opaque storage, never inspected or copied across C++ runtime boundaries.
        alignas(void*) unsigned char name[64] = {};
        const wchar_t* text = L"MainWindowServerTabsWidget";
        stringCtor(name, text, (int)wcslen(text));
        // TeamSpeak names the surrounding QWidget, rather than its inner
        // ImprovedTabWidget. A typed QTabWidget lookup by that name misses it.
        Object container = findChild(root, name, widgetMeta, 1);
        stringDtor(name);
        if (!container) return nullptr;
        if (inherits(container, "QTabWidget")) return container;
        emptyStringCtor(name); // Null QString = any object name, scoped to the server container.
        Object result = findChild(container, name, tabMeta, 0); // Prefer a direct child.
        if (!result) result = findChild(container, name, tabMeta, 1);
        stringDtor(name);
        return result;
    }

    static BOOL CALLBACK findWindow(HWND window, LPARAM param) {
        auto self = reinterpret_cast<CopyTabUI*>(param);
        Object root = self->findWidget((UINT_PTR)window);
        if (root && (self->tabs = self->child(root))) return FALSE;
        return TRUE;
    }

    bool method(Object receiver, const char* name) {
        Argument empty = {nullptr, nullptr};
        return invoke(receiver, name, 1, &empty, &empty, &empty, &empty, &empty, &empty,
                      &empty, &empty, &empty, &empty, &empty); // Qt::DirectConnection
    }

public:
    bool available() {
        tabs = nullptr;
        failure = "The loaded Qt 5.15 runtime is missing a required tab function.";
        if (!load()) return false;
        failure = "No TeamSpeak server tab control found on the GUI thread.";
        EnumThreadWindows(GetCurrentThreadId(), findWindow, (LPARAM)this);
        return tabs != nullptr;
    }

    const char* failureReason() const { return failure; }

    bool begin() {
        if (!available()) return false;
        previous = current(tabs);
        if (!previous) return false;
        before.clear();
        for (int i = 0; i < count(tabs); ++i) before.insert(widget(tabs, i));
        repaint = updatesEnabled(tabs);
        if (repaint) updates(tabs, false);
        return true;
    }

    bool end(unsigned long long sch) {
        Object added = current(tabs);
        bool tracked = sch && added && !before.count(added);
        if (tracked) {
            HWND handle = (HWND)winId(added);
            UINT_PTR tag = ++nextTag;
            tracked = handle && SetPropW(handle, L"FunPluginCopyTab", (HANDLE)tag);
            if (tracked) copies[sch] = {added, previous, handle, tag};
        }
        if (previous && indexOf(tabs, previous) >= 0) select(tabs, previous);
        if (repaint) updates(tabs, true);
        previous = nullptr;
        before.clear();
        return !sch || tracked;
    }

    bool close(unsigned long long sch) {
        auto found = copies.find(sch);
        if (found == copies.end() || !available()) return false;
        // A manually closed page's address or HWND can be reused. The window
        // property dies with the owned page, so a recycled object is never closed.
        if (!IsWindow(found->second.window) ||
            (UINT_PTR)GetPropW(found->second.window, L"FunPluginCopyTab") != found->second.tag ||
            findWidget((UINT_PTR)found->second.window) != found->second.page) {
            copies.erase(found);
            return true;
        }
        Object page = found->second.page;
        int index = indexOf(tabs, page);
        if (index < 0) { copies.erase(found); return true; } // Already closed by the user.
        Object selected = current(tabs);
        bool redraw = updatesEnabled(tabs);
        if (redraw) updates(tabs, false);
        // TS3's custom close signal belongs to the close BUTTON, not to the
        // QTabWidget. Activate that exact page's button, letting the existing
        // TS3 signal connections dispose its server view and handler together.
        Object bar = tabBar(tabs);
        for (int side : {1, 0}) { // QTabBar::RightSide, LeftSide (theme dependent).
            Object button = bar ? tabButton(bar, index, side) : nullptr;
            if (!button) continue;
            if (inherits(button, "ImprovedTabBarCloseButton")) {
                method(button, "onCloseRequestedTab");
                break;
            }
            // Standard Qt close buttons are installed by tabsClosable. Avoid
            // clicking an arbitrary custom tab accessory on the opposite side.
            if (closable(tabs) && inherits(button, "QAbstractButton") && method(button, "click")) break;
        }
        bool removed = indexOf(tabs, page) < 0;
        Object preferred = selected == page ? found->second.previous : selected;
        if (preferred && indexOf(tabs, preferred) >= 0) select(tabs, preferred);
        if (redraw) updates(tabs, true);
        if (removed) copies.erase(sch);
        return removed;
    }

    void forget(unsigned long long sch) {
        auto found = copies.find(sch);
        if (found != copies.end() && IsWindow(found->second.window) &&
            (UINT_PTR)GetPropW(found->second.window, L"FunPluginCopyTab") == found->second.tag)
            RemovePropW(found->second.window, L"FunPluginCopyTab");
        copies.erase(sch);
    }

    void restoreIfSelected(unsigned long long sch) {
        auto found = copies.find(sch);
        if (found != copies.end() && available() && IsWindow(found->second.window) &&
            (UINT_PTR)GetPropW(found->second.window, L"FunPluginCopyTab") == found->second.tag &&
            current(tabs) == found->second.page &&
            indexOf(tabs, found->second.previous) >= 0)
            select(tabs, found->second.previous);
    }
};
#endif
