/* Offline SDK + native dialog tests; never connects to TeamSpeak. */
#include <windows.h>
#include <cassert>
#include <atomic>
#include <iostream>
static int warningAnswer = IDNO, warningCount = 0, dialogResult = 0;
static int WINAPI fakeMessageBox(HWND, LPCWSTR text, LPCWSTR, UINT flags) {
    if (flags & MB_YESNO) {
        ++warningCount;
        assert(flags & MB_DEFBUTTON2);
        assert(wcsstr(text, L"own server") && wcsstr(text, L"flood protection"));
        return warningAnswer;
    }
    return IDOK;
}
static BOOL WINAPI fakeEndDialog(HWND, INT_PTR result) { dialogResult = (int)result; return TRUE; }
#define MessageBoxW fakeMessageBox
#define EndDialog fakeEndDialog
#include "../src/plugin.cpp"
#undef MessageBoxW
#undef EndDialog

namespace {
std::vector<uint64> sent;
std::string resultText, server = "server-A", recipient = "friend-A";
std::string currentCode;
int serial = 0, failAt = 0, changeAt = 0, disconnectAt = 0, replyAt = 0;
bool expectPrivate = false, wrongServer = false, slow = false;
std::atomic<int> entered{0};
DirectSend::Batch* active = nullptr;
SendWindow* window = nullptr;
bool acknowledge = true;
unsigned int release(void* p) { free(p); return ERROR_ok; }
unsigned int status(uint64, int* value) { *value = STATUS_CONNECTION_ESTABLISHED; return ERROR_ok; }
unsigned int serverID(uint64 sch, size_t flag, char** value) {
    assert(flag == VIRTUALSERVER_UNIQUE_IDENTIFIER);
    *value = _strdup(wrongServer && sch == 102 ? "other-server" : server.c_str()); return ERROR_ok;
}
unsigned int clientID(uint64, anyID client, size_t flag, char** value) {
    assert(client == 42 && flag == CLIENT_UNIQUE_IDENTIFIER);
    *value = _strdup(recipient.c_str()); return ERROR_ok;
}
void code(const char*, char* value, size_t length) { snprintf(value, length, "test-%d", ++serial); }
void print(uint64 sch, const char* text, PluginMessageTarget) { assert(sch == 1); resultText = text; }
unsigned int send(uint64 sch, anyID client, const char* text, const char* returnCode) {
    assert(client == 42 && std::string(text) == "hello \xd8\xb3");
    currentCode = returnCode;
    sent.push_back(sch);
    int n = (int)sent.size();
    entered = n;
    if (slow) Sleep(1);
    if (changeAt == n) recipient = "different-client-reusing-id";
    if (disconnectAt == n) active->disconnected(1);
    if (acknowledge) {
        std::string owner = window->reply(sch, returnCode);
        if (replyAt == n) active->reply(sch, owner.c_str(), ERROR_client_is_flooding);
    }
    return failAt == n ? ERROR_client_is_flooding : ERROR_ok;
}
unsigned int poke(uint64 sch, anyID client, const char* text, const char* returnCode) {
    assert(!expectPrivate); return send(sch, client, text, returnCode);
}
unsigned int message(uint64 sch, const char* text, anyID client, const char* returnCode) {
    assert(expectPrivate); return send(sch, client, text, returnCode);
}
DirectSend::Request request(bool copies = false) {
    DirectSend::Request r;
    r.source = 1; r.target = 42; r.senders = copies ? std::vector<uint64>{100, 101, 102} : std::vector<uint64>{1};
    r.count = copies ? 1 : 5; r.text = "hello \xd8\xb3"; r.serverUID = "server-A"; r.targetUID = "friend-A";
    return r;
}
void reset() {
    sent.clear(); resultText.clear(); recipient = "friend-A"; server = "server-A";
    failAt = changeAt = disconnectAt = replyAt = 0; expectPrivate = wrongServer = slow = false; entered = 0;
    acknowledge = true;
    window->cancelAll();
}
void finish(DirectSend::Batch& batch) {
    DWORD deadline = GetTickCount() + 5000;
    while (batch.busy() && GetTickCount() < deadline) Sleep(1);
    assert(!batch.busy()); batch.stop();
}
}

int main() {
    TS3Functions api = {};
    api.freeMemory = release; api.getConnectionStatus = status;
    api.getServerVariableAsString = serverID; api.getClientVariableAsString = clientID;
    api.createReturnCode = code; api.printMessage = print;
    api.requestClientPoke = poke; api.requestSendPrivateTextMsg = message;
    SendWindow flow(api); window = &flow;
    DirectSend::Batch batch(api, flow); active = &batch;
    reset(); assert(batch.start(request(), "plugin")); finish(batch);
    assert(sent == std::vector<uint64>(5, 1)); assert(resultText.find("5/5") != std::string::npos);
    reset(); assert(batch.start(request(true), "plugin")); finish(batch);
    assert((sent == std::vector<uint64>{100, 101, 102}));
    reset(); expectPrivate = true; auto r = request(true); r.privateMessage = true;
    assert(batch.start(r, "plugin")); finish(batch); assert(sent.size() == 3);
    reset(); r = request(true); r.count = 4;
    assert(batch.start(r, "plugin")); finish(batch);
    assert((sent == std::vector<uint64>{100,100,100,100,101,101,101,101,102,102,102,102}));
    reset(); expectPrivate = true; r.privateMessage = true;
    assert(batch.start(r, "plugin")); finish(batch); assert(sent.size() == 12);
    reset(); failAt = 2; assert(batch.start(request(), "plugin")); finish(batch);
    assert(sent.size() == 2 && resultText.find("1/5") != std::string::npos);
    reset(); replyAt = 2; assert(batch.start(request(), "plugin")); finish(batch); assert(sent.size() == 2);
    reset(); changeAt = 2; assert(batch.start(request(), "plugin")); finish(batch); assert(sent.size() == 2);
    reset(); disconnectAt = 1; assert(batch.start(request(), "plugin")); finish(batch); assert(sent.size() == 1);
    reset(); wrongServer = true; assert(batch.start(request(true), "plugin")); finish(batch);
    assert((sent == std::vector<uint64>{100, 101}));
    reset(); r = request(); r.count = INT_MAX; slow = true;
    assert(batch.start(r, "plugin")); assert(!batch.start(r, "plugin"));
    while (!entered.load()) Sleep(1);
    batch.reply(1, "old-return-code", ERROR_client_is_flooding); assert(batch.busy());
    batch.disconnected(999); assert(batch.busy());
    batch.stop(); assert(!batch.busy() && sent.size() < 100);
    reset(); acknowledge = false; r = request(); r.count = 100000;
    assert(batch.start(r, "plugin"));
    while (entered.load() < (int)SendWindow::capacity) Sleep(1);
    Sleep(30); flow.cancelAll(); batch.stop();
    assert(sent.size() == SendWindow::capacity && !batch.busy());
    reset(); r = request(); r.targetUID.clear(); assert(!batch.start(r, "plugin")); assert(sent.empty());

    // Exercise the actual dialog procedure: invalid input and declining the warning cannot send.
    hInstance = GetModuleHandleW(nullptr);
    DirectSendDialog data; data.request = request(); data.name = L"Friend";
    HWND dialog = CreateDialogParamW(hInstance, MAKEINTRESOURCEW(IDD_DIRECT_SEND), nullptr, directSendProc, (LPARAM)&data);
    assert(dialog);
    SetDlgItemTextW(dialog, IDC_SEND_COUNT, L"0");
    SendMessageW(dialog, WM_COMMAND, IDOK, 0); assert(!dialogResult && !warningCount);
    SetDlgItemTextW(dialog, IDC_SEND_COUNT, L"7");
    SetDlgItemTextW(dialog, IDC_SEND_TEXT, std::wstring(51, L'\x0633').c_str());
    SendMessageW(dialog, WM_COMMAND, IDOK, 0); assert(!dialogResult && !warningCount);
    SetDlgItemTextW(dialog, IDC_SEND_TEXT, L"hello");
    SendMessageW(dialog, WM_COMMAND, IDOK, 0); assert(!dialogResult && warningCount == 1);
    warningAnswer = IDYES;
    SendMessageW(dialog, WM_COMMAND, IDOK, 0); assert(dialogResult == IDOK && data.request.count == 7);
    DestroyWindow(dialog);
    data.copies = true; data.request = request(true); data.request.privateMessage = true;
    dialog = CreateDialogParamW(hInstance, MAKEINTRESOURCEW(IDD_DIRECT_SEND), nullptr, directSendProc, (LPARAM)&data);
    assert(dialog && IsWindowEnabled(GetDlgItem(dialog, IDC_SEND_COUNT)));
    SetDlgItemTextW(dialog, IDC_SEND_COUNT, L"7");
    SendMessageW(dialog, WM_COMMAND, IDOK, 0); assert(data.request.count == 7);
    DestroyWindow(dialog);
    std::cout << "PASS: exact poke counts, copy poke/private sends, cancellation, flood/errors, identity/server checks, warning and UTF-8 validation\n";
}
