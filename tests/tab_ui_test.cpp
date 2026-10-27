/* Exercise the Qt bridge against TS3's installed Qt DLLs, using a separate,
 * hidden test window. No TS3 instance, real config, server or microphone. */
#include <cassert>
#include <cstdio>
#include "../src/tab_ui.h"

template<class T> T function(HMODULE dll, const char* name) {
    FARPROC address = GetProcAddress(dll, name);
    assert(address);
    T result;
    memcpy(&result, &address, sizeof(result));
    return result;
}

int main(int argc, char** argv) {
    const wchar_t* path = L"C:\\Program Files\\TeamSpeak 3 Client";
    if (argc > 1) {
        fprintf(stderr, "This test uses the standard installed TS3 Qt path.\n");
        return 1;
    }
    SetDllDirectoryW(path);
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX);
    SetEnvironmentVariableW(L"QT_FORCE_STDERR_LOGGING", L"1");
    SetEnvironmentVariableW(L"QT_QPA_PLATFORM_PLUGIN_PATH", L"C:\\Program Files\\TeamSpeak 3 Client\\platforms");
    HMODULE core = LoadLibraryW(L"Qt5Core.dll");
    assert(core && LoadLibraryW(L"Qt5Gui.dll"));
    HMODULE widgets = LoadLibraryW(L"Qt5Widgets.dll");
    assert(widgets);
    HMODULE crt = LoadLibraryW(L"ucrtbase.dll");
    auto environment = function<int (*)(const char*, const char*)>(crt, "_putenv_s");
    environment("QT_FORCE_STDERR_LOGGING", "1");
    auto allocate = function<void* (*)(size_t)>(crt, "malloc");
    auto release = function<void (*)(void*)>(crt, "free");
    function<unsigned int (*)(unsigned int, unsigned int)>(crt, "_set_abort_behavior")(0, 3);
    auto appCtor = function<void (*)(void*, int*, char**, int)>(widgets, "??0QApplication@@QEAA@AEAHPEAPEADH@Z");
    auto appDtor = function<void (*)(void*)>(widgets, "??1QApplication@@UEAA@XZ");
    auto widgetCtor = function<void (*)(void*, void*, int)>(widgets, "??0QWidget@@QEAA@PEAV0@V?$QFlags@W4WindowType@Qt@@@@@Z");
    auto widgetDtor = function<void (*)(void*)>(widgets, "??1QWidget@@UEAA@XZ");
    auto tabsCtor = function<void (*)(void*, void*)>(widgets, "??0QTabWidget@@QEAA@PEAVQWidget@@@Z");
    auto stringCtor = function<void (*)(void*, const wchar_t*, int)>(core, "??0QString@@QEAA@PEBVQChar@@H@Z");
    auto stringDtor = function<void (*)(void*)>(core, "??1QString@@QEAA@XZ");
    auto setName = function<void (*)(void*, void*)>(core, "?setObjectName@QObject@@QEAAXAEBVQString@@@Z");
    auto winId = function<UINT_PTR (*)(void*)>(widgets, "?winId@QWidget@@QEBA_KXZ");
    auto addTab = function<int (*)(void*, void*, void*)>(widgets, "?addTab@QTabWidget@@QEAAHPEAVQWidget@@AEBVQString@@@Z");
    auto select = function<void (*)(void*, void*)>(widgets, "?setCurrentWidget@QTabWidget@@QEAAXPEAVQWidget@@@Z");
    auto current = function<void* (*)(void*)>(widgets, "?currentWidget@QTabWidget@@QEBAPEAVQWidget@@XZ");
    auto count = function<int (*)(void*)>(widgets, "?count@QTabWidget@@QEBAHXZ");
    auto closable = function<void (*)(void*, bool)>(widgets, "?setTabsClosable@QTabWidget@@QEAAX_N@Z");
    auto tabBar = function<void* (*)(void*)>(widgets, "?tabBar@QTabWidget@@QEBAPEAVQTabBar@@XZ");
    auto setButton = function<void (*)(void*, int, int, void*)>(widgets,
        "?setTabButton@QTabBar@@QEAAXHW4ButtonPosition@1@PEAVQWidget@@@Z");
    auto buttonCtor = function<void (*)(void*, void*)>(widgets, "??0QToolButton@@QEAA@PEAVQWidget@@@Z");
    auto sendPosted = function<void (*)(void*, int)>(core, "?sendPostedEvents@QCoreApplication@@SAXPEAVQObject@@H@Z");
    auto connect = function<void (*)(void*, void*, const char*, void*, const char*, int)>(core,
        "?connect@QObject@@SA?AVConnection@QMetaObject@@PEBV1@PEBD01W4ConnectionType@Qt@@@Z");
    auto connectionDtor = function<void (*)(void*)>(core, "??1Connection@QMetaObject@@QEAA@XZ");

    void* app = allocate(4096);
    char executable[] = "tab_ui_test", option[] = "-platformpluginpath", platformPath[] = "C:\\Program Files\\TeamSpeak 3 Client\\platforms";
    char* arguments[] = {executable, option, platformPath, nullptr};
    int argumentCount = 3;
    appCtor(app, &argumentCount, arguments, 0x050f02);
    void* root = allocate(4096);
    widgetCtor(root, nullptr, 0);
    void* wrapper = allocate(4096);
    widgetCtor(wrapper, root, 0);
    void* tabs = allocate(4096);
    tabsCtor(tabs, wrapper);
    closable(tabs, true);
    alignas(void*) unsigned char name[64] = {};
    const wchar_t* objectName = L"MainWindowServerTabsWidget";
    stringCtor(name, objectName, (int)wcslen(objectName));
    setName(tabs, name); stringDtor(name);
    winId(root); // Native, hidden window discoverable on this thread.
    auto page = [&](const wchar_t* label) {
        void* result = allocate(4096);
        widgetCtor(result, nullptr, 0);
        stringCtor(name, label, (int)wcslen(label));
        addTab(tabs, result, name); stringDtor(name);
        return result;
    };
    void* original = page(L"Original server");
    void* unrelated = page(L"Unrelated server");
    CopyTabUI ui;
    assert(ui.available());
    // Regression: the real TS3 object name belongs to a surrounding QWidget,
    // not to the inner tab widget used by the old single-widget test.
    stringCtor(name, L"ActualServerTabs", 16); setName(tabs, name); stringDtor(name);
    stringCtor(name, objectName, (int)wcslen(objectName)); setName(wrapper, name); stringDtor(name);
    assert(ui.available());
    assert(!ui.close(1)); // Unknown tab IDs must never close anything.
    assert(ui.begin());
    void* copy = page(L"Fun Plugin copy");
    select(tabs, copy);
    assert(ui.end(100));
    assert(current(tabs) == original && count(tabs) == 3);

    // Qt's signal-to-slot connection schedules normal widget disposal. This
    // exercises invokeMethod's argument ABI and the deferred-close retry.
    alignas(void*) unsigned char connection[64] = {};
    connect(connection, tabs, "2tabCloseRequested(int)", copy, "1deleteLater()", 1);
    assert(*reinterpret_cast<void**>(connection));
    connectionDtor(connection);
    assert(!ui.close(100)); // Deferred deletion has not happened yet.
    function<void (*)(int)>(core, "?processEvents@QCoreApplication@@SAXV?$QFlags@W4ProcessEventsFlag@QEventLoop@@@@@Z")(0);
    sendPosted(nullptr, 52); // QEvent::DeferredDelete
    assert(ui.close(100));
    assert(count(tabs) == 2 && current(tabs) == original);

    select(tabs, unrelated);
    assert(ui.begin());
    void* secondCopy = page(L"Second copy");
    select(tabs, secondCopy);
    assert(ui.end(101));
    assert(current(tabs) == unrelated); // Preserve whichever tab the user selected.
    select(tabs, secondCopy);
    ui.restoreIfSelected(101);
    assert(current(tabs) == unrelated); // Also handle a late connect-status selection.
    // Regression: TS3 installs a custom per-page close button whose signal is
    // wired directly to its manager. Emitting QTabWidget::tabCloseRequested
    // succeeds but does nothing here, just as it did in the real client.
    void* closeButton = allocate(4096);
    buttonCtor(closeButton, nullptr);
    setButton(tabBar(tabs), 2, 1, closeButton);
    connect(connection, closeButton, "2clicked(bool)", secondCopy, "1deleteLater()", 1);
    connectionDtor(connection);
    select(tabs, secondCopy);
    assert(!ui.close(101));
    assert(current(tabs) == unrelated);
    sendPosted(nullptr, 52);
    assert(ui.close(101) && count(tabs) == 2);
    assert(ui.begin());
    void* replaced = page(L"Reused address/window test");
    select(tabs, replaced); assert(ui.end(102));
    RemovePropW((HWND)winId(replaced), L"FunPluginCopyTab");
    assert(ui.close(102) && count(tabs) == 3); // Lost ownership must never close the page.
    assert(!ui.begin() || ui.end(0));
    widgetDtor(root); release(root); // Qt disposes its own child widgets.
    appDtor(app); release(app);
    puts("PASS: actual Qt 5.15 discovery, focus, standard/custom close buttons, deferred disposal, unrelated-tab protection");
}
