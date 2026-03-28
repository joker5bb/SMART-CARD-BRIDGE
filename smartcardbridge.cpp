// SmartCardBridge.cpp - Single File Windows PC/SC to Swift Server Bridge
// Compile with: g++ -o SmartCardBridge.exe SmartCardBridge.cpp -O2 -std=c++17 -mwindows -static -static-libgcc -static-libstdc++ -lwinscard -lwininet -lcomctl32 -luser32 -lgdi32 -lshell32 -lole32 -ladvapi32

#ifndef UNICODE
#define UNICODE
#endif
#ifndef _UNICODE
#define _UNICODE
#endif

#define WIN32_LEAN_AND_MEAN
#define _WIN32_WINNT 0x0601
#include <windows.h>
#include <winscard.h>
#include <wininet.h>
#include <commctrl.h>
#include <string>
#include <vector>
#include <thread>
#include <atomic>
#include <mutex>
#include <map>
#include <sstream>
#include <iomanip>
#include <chrono>
#include <functional>
#include <algorithm>
#include <cstring>

#pragma comment(lib, "winscard.lib")
#pragma comment(lib, "wininet.lib")
#pragma comment(lib, "comctl32.lib")
#pragma comment(lib, "user32.lib")
#pragma comment(lib, "gdi32.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "advapi32.lib")

// Control IDs
#define IDC_READER_COMBO    1001
#define IDC_REFRESH_BTN     1002
#define IDC_CONNECT_BTN     1003
#define IDC_STATUS_TEXT     1004
#define IDC_CARD_ID         1005
#define IDC_ATR_TEXT        1006
#define IDC_READ_BTN        1007
#define IDC_SERVER_HOST     1008
#define IDC_SERVER_PORT     1009
#define IDC_API_KEY         1010
#define IDC_TEST_BTN        1011
#define IDC_SEND_BTN        1012
#define IDC_AUTO_SEND       1013
#define IDC_LOG_LIST        1014
#define IDC_CLEAR_LOG       1015
#define IDC_STATIC_GROUP1   1016
#define IDC_STATIC_GROUP2   1017
#define IDC_STATIC_GROUP3   1018

// Custom window messages
#define WM_CARD_STATUS      (WM_USER + 1)
#define WM_SERVER_RESPONSE  (WM_USER + 2)

// Global handles
HINSTANCE g_hInst = nullptr;
HWND g_hMainWnd = nullptr;
HFONT g_hFont = nullptr;

// PC/SC Smart Card Manager
class SmartCardManager {
private:
    SCARDCONTEXT hContext = 0;
    SCARDHANDLE hCard = 0;
    DWORD dwActiveProtocol = 0;
    std::wstring connectedReader;
    std::atomic<bool> monitoring{false};
    std::thread monitorThread;
    std::function<void(const std::wstring&, bool, const std::vector<BYTE>&)> statusCallback;
    std::mutex cardMutex;

public:
    ~SmartCardManager() { Shutdown(); }

    bool Initialize() {
        LONG result = SCardEstablishContext(SCARD_SCOPE_SYSTEM, NULL, NULL, &hContext);
        return result == SCARD_S_SUCCESS;
    }

    void Shutdown() {
        StopMonitoring();
        Disconnect();
        if (hContext != 0) {
            SCardReleaseContext(hContext);
            hContext = 0;
        }
    }

    std::vector<std::wstring> ListReaders() {
        std::vector<std::wstring> readers;
        if (hContext == 0) return readers;

        DWORD dwReaders = SCARD_AUTOALLOCATE;
        LPWSTR mszReaders = NULL;

        LONG result = SCardListReadersW(hContext, NULL, (LPWSTR)&mszReaders, &dwReaders);
        if (result != SCARD_S_SUCCESS) {
            if (result == SCARD_E_NO_READERS_AVAILABLE) {
                return readers;
            }
            return readers;
        }

        LPWSTR ptr = mszReaders;
        while (ptr && *ptr != L'\0') {
            readers.push_back(std::wstring(ptr));
            ptr += wcslen(ptr) + 1;
        }

        if (mszReaders) {
            SCardFreeMemory(hContext, mszReaders);
        }
        return readers;
    }

    bool Connect(const std::wstring& readerName) {
        std::lock_guard<std::mutex> lock(cardMutex);
        if (hCard != 0) {
            SCardDisconnect(hCard, SCARD_LEAVE_CARD);
            hCard = 0;
        }

        LONG result = SCardConnectW(hContext, readerName.c_str(),
                                   SCARD_SHARE_SHARED,
                                   SCARD_PROTOCOL_T0 | SCARD_PROTOCOL_T1,
                                   &hCard, &dwActiveProtocol);

        if (result == SCARD_S_SUCCESS) {
            connectedReader = readerName;
            return true;
        }
        return false;
    }

    void Disconnect() {
        std::lock_guard<std::mutex> lock(cardMutex);
        if (hCard != 0) {
            SCardDisconnect(hCard, SCARD_LEAVE_CARD);
            hCard = 0;
            connectedReader.clear();
        }
    }

    bool IsConnected() const {
        return hCard != 0;
    }

    bool IsCardPresent(const std::wstring& readerName) {
        SCARD_READERSTATEW state;
        state.szReader = readerName.c_str();
        state.dwCurrentState = SCARD_STATE_UNAWARE;

        if (SCardGetStatusChangeW(hContext, 0, &state, 1) == SCARD_S_SUCCESS) {
            return (state.dwEventState & SCARD_STATE_PRESENT) != 0;
        }
        return false;
    }

    std::vector<BYTE> GetCardUID() {
        std::lock_guard<std::mutex> lock(cardMutex);
        if (hCard == 0) return {};

        const SCARD_IO_REQUEST* sendPci = (dwActiveProtocol == SCARD_PROTOCOL_T0) ? SCARD_PCI_T0 : SCARD_PCI_T1;

        BYTE cmd[] = {0xFF, 0xCA, 0x00, 0x00, 0x00};
        BYTE recv[256];
        DWORD recvLen = sizeof(recv);
        SCARD_IO_REQUEST recvPci;

        LONG result = SCardTransmit(hCard, sendPci, cmd, sizeof(cmd), &recvPci, recv, &recvLen);

        if (result == SCARD_S_SUCCESS && recvLen >= 2) {
            BYTE sw1 = recv[recvLen - 2];
            BYTE sw2 = recv[recvLen - 1];
            if (sw1 == 0x90 && sw2 == 0x00) {
                return std::vector<BYTE>(recv, recv + recvLen - 2);
            }
        }
        return {};
    }

    std::vector<BYTE> GetATR(const std::wstring& readerName) {
        SCARD_READERSTATEW state;
        state.szReader = readerName.c_str();
        state.dwCurrentState = SCARD_STATE_UNAWARE;

        if (SCardGetStatusChangeW(hContext, 0, &state, 1) == SCARD_S_SUCCESS) {
            if (state.cbAtr > 0) {
                return std::vector<BYTE>(state.rgbAtr, state.rgbAtr + state.cbAtr);
            }
        }
        return {};
    }

    void StartMonitoring(std::function<void(const std::wstring&, bool, const std::vector<BYTE>&)> cb) {
        statusCallback = cb;
        monitoring = true;
        monitorThread = std::thread(&SmartCardManager::MonitorThread, this);
    }

    void StopMonitoring() {
        monitoring = false;
        if (monitorThread.joinable()) {
            monitorThread.join();
        }
    }

    bool IsMonitoring() const { return monitoring; }

private:
    void MonitorThread() {
        std::map<std::wstring, bool> lastStatus;
        auto lastReaders = ListReaders();

        for (const auto& reader : lastReaders) {
            lastStatus[reader] = IsCardPresent(reader);
        }

        while (monitoring) {
            auto currentReaders = ListReaders();

            for (const auto& reader : currentReaders) {
                bool hasCard = IsCardPresent(reader);
                auto atr = GetATR(reader);

                auto it = lastStatus.find(reader);
                if (it == lastStatus.end() || it->second != hasCard) {
                    if (statusCallback) {
                        statusCallback(reader, hasCard, atr);
                    }
                    lastStatus[reader] = hasCard;
                }
            }

            for (auto it = lastStatus.begin(); it != lastStatus.end(); ) {
                if (std::find(currentReaders.begin(), currentReaders.end(), it->first) == currentReaders.end()) {
                    it = lastStatus.erase(it);
                } else {
                    ++it;
                }
            }

            std::this_thread::sleep_for(std::chrono::milliseconds(500));
        }
    }
};

// HTTP Client for Swift Server
class HttpClient {
private:
    std::string host;
    int port;
    std::string apiKey;

public:
    void SetConfig(const std::string& h, int p, const std::string& key) {
        host = h;
        port = p;
        apiKey = key;
    }

    bool SendCardData(const std::string& readerName, const std::string& cardId, 
                      const std::string& atr, std::string& response) {
        std::string json = "{\"readerName\":\"" + EscapeJson(readerName) + "\",";
        json += "\"cardId\":\"" + EscapeJson(cardId) + "\",";
        json += "\"atr\":\"" + EscapeJson(atr) + "\",";
        json += "\"timestamp\":\"" + GetTimestamp() + "\"}";

        return Post("/api/v1/cards/events", json, response);
    }

    bool TestConnection(std::string& response) {
        return Get("/api/v1/health", response);
    }

private:
    std::string EscapeJson(const std::string& s) {
        std::string result;
        for (char c : s) {
            if (c == '\\' || c == '"') result += '\\';
            result += c;
        }
        return result;
    }

    std::string GetTimestamp() {
        auto now = std::chrono::system_clock::now();
        auto time = std::chrono::system_clock::to_time_t(now);
        char buf[64];
        strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%S", localtime(&time));
        return buf;
    }

    bool Post(const std::string& endpoint, const std::string& body, std::string& response) {
        return HttpRequest("POST", endpoint, body, response);
    }

    bool Get(const std::string& endpoint, std::string& response) {
        return HttpRequest("GET", endpoint, "", response);
    }

    bool HttpRequest(const std::string& method, const std::string& endpoint, 
                     const std::string& body, std::string& response) {
        HINTERNET hInternet = InternetOpenA("SmartCardBridge/1.0", INTERNET_OPEN_TYPE_DIRECT, NULL, NULL, 0);
        if (!hInternet) return false;

        std::string url = "http://" + host + ":" + std::to_string(port) + endpoint;

        URL_COMPONENTSA urlComp;
        char hostName[256] = {0};
        char path[512] = {0};
        memset(&urlComp, 0, sizeof(urlComp));
        urlComp.dwStructSize = sizeof(urlComp);
        urlComp.lpszHostName = hostName;
        urlComp.dwHostNameLength = sizeof(hostName);
        urlComp.lpszUrlPath = path;
        urlComp.dwUrlPathLength = sizeof(path);

        if (!InternetCrackUrlA(url.c_str(), (DWORD)url.length(), 0, &urlComp)) {
            InternetCloseHandle(hInternet);
            return false;
        }

        HINTERNET hConnect = InternetConnectA(hInternet, hostName, (INTERNET_PORT)port, NULL, NULL, 
                                               INTERNET_SERVICE_HTTP, 0, 0);
        if (!hConnect) {
            InternetCloseHandle(hInternet);
            return false;
        }

        HINTERNET hRequest = HttpOpenRequestA(hConnect, method.c_str(), path, NULL, NULL, NULL, 
                                               INTERNET_FLAG_RELOAD | INTERNET_FLAG_NO_CACHE_WRITE, 0);
        if (!hRequest) {
            InternetCloseHandle(hConnect);
            InternetCloseHandle(hInternet);
            return false;
        }

        std::string headers = "Content-Type: application/json\r\n";
        if (!apiKey.empty()) {
            headers += "X-API-Key: " + apiKey + "\r\n";
        }

        BOOL result = HttpSendRequestA(hRequest, headers.c_str(), (DWORD)headers.length(), 
                                       (LPVOID)body.c_str(), (DWORD)body.length());

        if (result) {
            char buffer[4096];
            DWORD bytesRead = 0;
            while (InternetReadFile(hRequest, buffer, sizeof(buffer) - 1, &bytesRead) && bytesRead > 0) {
                buffer[bytesRead] = '\0';
                response.append(buffer, bytesRead);
            }
        }

        InternetCloseHandle(hRequest);
        InternetCloseHandle(hConnect);
        InternetCloseHandle(hInternet);

        return result == TRUE;
    }
};

// Global instances
SmartCardManager g_cardManager;
HttpClient g_httpClient;
std::atomic<bool> g_autoSend{true};
std::string g_lastCardId;

// Utility functions
std::string WStringToString(const std::wstring& wstr) {
    if (wstr.empty()) return "";
    int size = WideCharToMultiByte(CP_UTF8, 0, wstr.c_str(), -1, NULL, 0, NULL, NULL);
    if (size <= 0) return "";
    std::string str(size - 1, 0);
    WideCharToMultiByte(CP_UTF8, 0, wstr.c_str(), -1, &str[0], size, NULL, NULL);
    return str;
}

std::wstring StringToWString(const std::string& str) {
    if (str.empty()) return L"";
    int size = MultiByteToWideChar(CP_UTF8, 0, str.c_str(), -1, NULL, 0);
    if (size <= 0) return L"";
    std::wstring wstr(size - 1, 0);
    MultiByteToWideChar(CP_UTF8, 0, str.c_str(), -1, &wstr[0], size);
    return wstr;
}

std::string BytesToHex(const std::vector<BYTE>& data) {
    std::stringstream ss;
    for (BYTE b : data) {
        ss << std::hex << std::setw(2) << std::setfill('0') << (int)b;
    }
    return ss.str();
}

std::string BytesToHexWithSpaces(const std::vector<BYTE>& data) {
    std::stringstream ss;
    for (size_t i = 0; i < data.size(); ++i) {
        ss << std::hex << std::setw(2) << std::setfill('0') << (int)data[i];
        if (i < data.size() - 1) ss << " ";
    }
    return ss.str();
}

void LogMessage(const std::wstring& msg) {
    if (!g_hMainWnd) return;
    HWND hLog = GetDlgItem(g_hMainWnd, IDC_LOG_LIST);
    if (hLog) {
        auto now = std::chrono::system_clock::now();
        auto time = std::chrono::system_clock::to_time_t(now);
        char timeStr[32];
        strftime(timeStr, sizeof(timeStr), "[%H:%M:%S] ", localtime(&time));

        std::wstring fullMsg = StringToWString(timeStr) + msg;
        SendMessageW(hLog, LB_ADDSTRING, 0, (LPARAM)fullMsg.c_str());

        int count = (int)SendMessageW(hLog, LB_GETCOUNT, 0, 0);
        if (count > 0) {
            SendMessageW(hLog, LB_SETTOPINDEX, count - 1, 0);
        }

        if (count > 1000) {
            SendMessageW(hLog, LB_DELETESTRING, 0, 0);
        }
    }
}

void UpdateCardStatusUI(const std::wstring& readerName, bool present, const std::vector<BYTE>& atr) {
    if (!g_hMainWnd) return;

    HWND hStatus = GetDlgItem(g_hMainWnd, IDC_STATUS_TEXT);
    HWND hAtr = GetDlgItem(g_hMainWnd, IDC_ATR_TEXT);
    HWND hReadBtn = GetDlgItem(g_hMainWnd, IDC_READ_BTN);

    if (present) {
        SetWindowTextW(hStatus, L"Card Present ");

        if (!atr.empty()) {
            SetWindowTextW(hAtr, StringToWString(BytesToHexWithSpaces(atr)).c_str());
        }

        EnableWindow(hReadBtn, TRUE);

        wchar_t currentReader[256] = {0};
        GetDlgItemTextW(g_hMainWnd, IDC_READER_COMBO, currentReader, 256);
        if (readerName == currentReader && !g_cardManager.IsConnected()) {
            g_cardManager.Connect(readerName);
            SetDlgItemTextW(g_hMainWnd, IDC_CONNECT_BTN, L"Disconnect");
        }

        if (g_autoSend && IsDlgButtonChecked(g_hMainWnd, IDC_AUTO_SEND) == BST_CHECKED) {
            PostMessageW(g_hMainWnd, WM_COMMAND, MAKEWPARAM(IDC_READ_BTN, BN_CLICKED), (LPARAM)hReadBtn);
        }

        LogMessage(L"Card inserted: " + readerName);
    } else {
        SetWindowTextW(hStatus, L"No Card");
        SetDlgItemTextW(g_hMainWnd, IDC_CONNECT_BTN, L"Connect");
        SetWindowTextW(hAtr, L"");
        EnableWindow(hReadBtn, FALSE);
        SetDlgItemTextW(g_hMainWnd, IDC_CARD_ID, L"");
        g_cardManager.Disconnect();
        LogMessage(L"Card removed: " + readerName);
    }
}

void RefreshReadersList() {
    if (!g_hMainWnd) return;

    HWND hCombo = GetDlgItem(g_hMainWnd, IDC_READER_COMBO);
    SendMessageW(hCombo, CB_RESETCONTENT, 0, 0);

    auto readers = g_cardManager.ListReaders();
    for (const auto& reader : readers) {
        SendMessageW(hCombo, CB_ADDSTRING, 0, (LPARAM)reader.c_str());
    }

    if (readers.empty()) {
        SendMessageW(hCombo, CB_ADDSTRING, 0, (LPARAM)L"No readers found");
        EnableWindow(hCombo, FALSE);
        EnableWindow(GetDlgItem(g_hMainWnd, IDC_CONNECT_BTN), FALSE);
    } else {
        EnableWindow(hCombo, TRUE);
        EnableWindow(GetDlgItem(g_hMainWnd, IDC_CONNECT_BTN), TRUE);
        SendMessageW(hCombo, CB_SETCURSEL, 0, 0);
    }

    LogMessage(L"Found " + std::to_wstring(readers.size()) + L" reader(s)");
}

void ReadCardAndSendData() {
    auto uid = g_cardManager.GetCardUID();
    if (uid.empty()) {
        LogMessage(L"Error: Failed to read card UID");
        MessageBoxW(g_hMainWnd, L"Failed to read card data. Ensure card is properly inserted and connected.", 
                   L"Read Error", MB_OK | MB_ICONWARNING);
        return;
    }

    std::string cardId = BytesToHex(uid);
    g_lastCardId = cardId;

    SetDlgItemTextW(g_hMainWnd, IDC_CARD_ID, StringToWString(cardId).c_str());
    LogMessage(L"Read card: " + StringToWString(cardId.substr(0, std::min((size_t)16, cardId.length()))) + L"...");

    char host[256] = {0};
    GetDlgItemTextA(g_hMainWnd, IDC_SERVER_HOST, host, sizeof(host));
    int port = GetDlgItemInt(g_hMainWnd, IDC_SERVER_PORT, NULL, FALSE);
    char apiKey[256] = {0};
    GetDlgItemTextA(g_hMainWnd, IDC_API_KEY, apiKey, sizeof(apiKey));

    if (strlen(host) == 0) {
        LogMessage(L"Warning: No server configured");
        return;
    }

    g_httpClient.SetConfig(host, port, apiKey);

    std::thread([cardId]() {
        wchar_t readerName[256] = {0};
        GetDlgItemTextW(g_hMainWnd, IDC_READER_COMBO, readerName, 256);

        char atr[512] = {0};
        GetDlgItemTextA(g_hMainWnd, IDC_ATR_TEXT, atr, sizeof(atr));

        std::string response;
        bool success = g_httpClient.SendCardData(WStringToString(readerName), cardId, atr, response);

        std::wstring msg = success ? L"Card data sent OK" : L"Failed to send data";
        PostMessageW(g_hMainWnd, WM_SERVER_RESPONSE, success ? 1 : 0, (LPARAM)_wcsdup(msg.c_str()));
    }).detach();
}

void TestServerConnection() {
    char host[256] = {0};
    GetDlgItemTextA(g_hMainWnd, IDC_SERVER_HOST, host, sizeof(host));
    int port = GetDlgItemInt(g_hMainWnd, IDC_SERVER_PORT, NULL, FALSE);
    char apiKey[256] = {0};
    GetDlgItemTextA(g_hMainWnd, IDC_API_KEY, apiKey, sizeof(apiKey));

    if (strlen(host) == 0) {
        MessageBoxW(g_hMainWnd, L"Please enter server host address", L"Configuration", MB_OK | MB_ICONINFORMATION);
        return;
    }

    g_httpClient.SetConfig(host, port, apiKey);
    LogMessage(L"Testing connection to " + StringToWString(host) + L":" + std::to_wstring(port));

    std::thread([]() {
        std::string response;
        bool success = g_httpClient.TestConnection(response);

        std::wstring msg = success ? L"Server connection OK" : L"Server connection failed";
        PostMessageW(g_hMainWnd, WM_SERVER_RESPONSE, success ? 2 : 0, (LPARAM)_wcsdup(msg.c_str()));
    }).detach();
}

// Window procedure
LRESULT CALLBACK MainWndProc(HWND hWnd, UINT message, WPARAM wParam, LPARAM lParam) {
    switch (message) {
    case WM_CREATE: {
        g_hMainWnd = hWnd;

        g_hFont = CreateFontW(14, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET, 
                              OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, DEFAULT_QUALITY, 
                              DEFAULT_PITCH | FF_SWISS, L"Segoe UI");

        // FIXED LAYOUT - Better spacing and positioning
        const int margin = 10;
        const int groupWidth = 285;
        const int groupHeight = 190;
        const int logGroupTop = 210;
        const int logGroupHeight = 200;

        // Group 1: Smart Card Reader (Left)
        HWND hGroup1 = CreateWindowW(L"BUTTON", L"Smart Card Reader", 
                                    WS_VISIBLE | WS_CHILD | BS_GROUPBOX,
                                    margin, margin, groupWidth, groupHeight, 
                                    hWnd, (HMENU)IDC_STATIC_GROUP1, g_hInst, NULL);
        SendMessageW(hGroup1, WM_SETFONT, (WPARAM)g_hFont, TRUE);

        // Reader row
        CreateWindowW(L"STATIC", L"Reader:", WS_VISIBLE | WS_CHILD | SS_LEFT,
                     margin + 10, margin + 20, 50, 20, hWnd, NULL, g_hInst, NULL);

        HWND hCombo = CreateWindowW(L"COMBOBOX", L"", 
                                    WS_VISIBLE | WS_CHILD | CBS_DROPDOWNLIST | WS_VSCROLL | WS_TABSTOP,
                                    margin + 65, margin + 18, 150, 150, 
                                    hWnd, (HMENU)IDC_READER_COMBO, g_hInst, NULL);
        SendMessageW(hCombo, WM_SETFONT, (WPARAM)g_hFont, TRUE);

        HWND hRefresh = CreateWindowW(L"BUTTON", L"Refresh", 
                                     WS_VISIBLE | WS_CHILD | BS_PUSHBUTTON | WS_TABSTOP,
                                     margin + 220, margin + 17, 55, 23, 
                                     hWnd, (HMENU)IDC_REFRESH_BTN, g_hInst, NULL);
        SendMessageW(hRefresh, WM_SETFONT, (WPARAM)g_hFont, TRUE);

        // Connect button
        HWND hConnect = CreateWindowW(L"BUTTON", L"Connect", 
                                     WS_VISIBLE | WS_CHILD | BS_PUSHBUTTON | WS_TABSTOP,
                                     margin + 10, margin + 50, 80, 25, 
                                     hWnd, (HMENU)IDC_CONNECT_BTN, g_hInst, NULL);
        SendMessageW(hConnect, WM_SETFONT, (WPARAM)g_hFont, TRUE);

        // Status
        CreateWindowW(L"STATIC", L"Status:", WS_VISIBLE | WS_CHILD | SS_LEFT,
                     margin + 10, margin + 85, 40, 20, hWnd, NULL, g_hInst, NULL);

        HWND hStatus = CreateWindowW(L"STATIC", L"Select reader and click Connect", 
                                    WS_VISIBLE | WS_CHILD | SS_LEFT,
                                    margin + 60, margin + 85, 200, 20, 
                                    hWnd, (HMENU)IDC_STATUS_TEXT, g_hInst, NULL);
        SendMessageW(hStatus, WM_SETFONT, (WPARAM)g_hFont, TRUE);

        // Card ID
        CreateWindowW(L"STATIC", L"Card ID:", WS_VISIBLE | WS_CHILD | SS_LEFT,
                     margin + 10, margin + 110, 40, 20, hWnd, NULL, g_hInst, NULL);

        HWND hCardId = CreateWindowW(L"EDIT", L"", 
                                    WS_VISIBLE | WS_CHILD | ES_AUTOHSCROLL | ES_READONLY | WS_BORDER,
                                    margin + 60, margin + 108, 215, 22, 
                                    hWnd, (HMENU)IDC_CARD_ID, g_hInst, NULL);
        SendMessageW(hCardId, WM_SETFONT, (WPARAM)g_hFont, TRUE);

        // Read button
        HWND hRead = CreateWindowW(L"BUTTON", L"Read & Send", 
                                  WS_VISIBLE | WS_CHILD | BS_PUSHBUTTON | WS_TABSTOP | WS_DISABLED,
                                  margin + 10, margin + 140, 90, 25, 
                                  hWnd, (HMENU)IDC_READ_BTN, g_hInst, NULL);
        SendMessageW(hRead, WM_SETFONT, (WPARAM)g_hFont, TRUE);

        // ATR
        CreateWindowW(L"STATIC", L"ATR:", WS_VISIBLE | WS_CHILD | SS_LEFT,
                     margin + 10, margin + 172, 30, 20, hWnd, NULL, g_hInst, NULL);

        HWND hAtr = CreateWindowW(L"EDIT", L"", 
                                 WS_VISIBLE | WS_CHILD | ES_READONLY | WS_BORDER | ES_AUTOHSCROLL,
                                 margin + 45, margin + 170, 230, 22, 
                                 hWnd, (HMENU)IDC_ATR_TEXT, g_hInst, NULL);
        SendMessageW(hAtr, WM_SETFONT, (WPARAM)g_hFont, TRUE);

        // Group 2: Server Configuration (Right)
        int rightGroupX = margin + groupWidth + 10;
        HWND hGroup2 = CreateWindowW(L"BUTTON", L"Swift Server Configuration", 
                                    WS_VISIBLE | WS_CHILD | BS_GROUPBOX,
                                    rightGroupX, margin, groupWidth, groupHeight, 
                                    hWnd, (HMENU)IDC_STATIC_GROUP2, g_hInst, NULL);
        SendMessageW(hGroup2, WM_SETFONT, (WPARAM)g_hFont, TRUE);

        // Host
        CreateWindowW(L"STATIC", L"Host:", WS_VISIBLE | WS_CHILD | SS_LEFT,
                     rightGroupX + 10, margin + 20, 35, 20, hWnd, NULL, g_hInst, NULL);

        HWND hHost = CreateWindowW(L"EDIT", L"localhost", 
                                  WS_VISIBLE | WS_CHILD | ES_AUTOHSCROLL | WS_BORDER,
                                  rightGroupX + 50, margin + 18, 180, 22, 
                                  hWnd, (HMENU)IDC_SERVER_HOST, g_hInst, NULL);
        SendMessageW(hHost, WM_SETFONT, (WPARAM)g_hFont, TRUE);

        // Port
        CreateWindowW(L"STATIC", L"Port:", WS_VISIBLE | WS_CHILD | SS_LEFT,
                     rightGroupX + 240, margin + 20, 30, 20, hWnd, NULL, g_hInst, NULL);

        HWND hPort = CreateWindowW(L"EDIT", L"8080", 
                                  WS_VISIBLE | WS_CHILD | ES_NUMBER | WS_BORDER,
                                  rightGroupX + 275, margin + 18, 50, 22, 
                                  hWnd, (HMENU)IDC_SERVER_PORT, g_hInst, NULL);
        SendMessageW(hPort, WM_SETFONT, (WPARAM)g_hFont, TRUE);

        // API Key
        CreateWindowW(L"STATIC", L"API Key:", WS_VISIBLE | WS_CHILD | SS_LEFT,
                     rightGroupX + 10, margin + 50, 45, 20, hWnd, NULL, g_hInst, NULL);

        HWND hApiKey = CreateWindowW(L"EDIT", L"", 
                                    WS_VISIBLE | WS_CHILD | ES_AUTOHSCROLL | ES_PASSWORD | WS_BORDER,
                                    rightGroupX + 60, margin + 48, 215, 22, 
                                    hWnd, (HMENU)IDC_API_KEY, g_hInst, NULL);
        SendMessageW(hApiKey, WM_SETFONT, (WPARAM)g_hFont, TRUE);

        // Auto-send checkbox
        HWND hAuto = CreateWindowW(L"BUTTON", L"Auto-send card data", 
                                  WS_VISIBLE | WS_CHILD | BS_AUTOCHECKBOX | WS_TABSTOP,
                                  rightGroupX + 10, margin + 80, 140, 20, 
                                  hWnd, (HMENU)IDC_AUTO_SEND, g_hInst, NULL);
        SendMessageW(hAuto, BM_SETCHECK, BST_CHECKED, 0);
        SendMessageW(hAuto, WM_SETFONT, (WPARAM)g_hFont, TRUE);

        // Test button
        HWND hTest = CreateWindowW(L"BUTTON", L"Test Connection", 
                                  WS_VISIBLE | WS_CHILD | BS_PUSHBUTTON | WS_TABSTOP,
                                  rightGroupX + 10, margin + 110, 110, 25, 
                                  hWnd, (HMENU)IDC_TEST_BTN, g_hInst, NULL);
        SendMessageW(hTest, WM_SETFONT, (WPARAM)g_hFont, TRUE);

        // Send button
        HWND hSend = CreateWindowW(L"BUTTON", L"Send Data", 
                                  WS_VISIBLE | WS_CHILD | BS_PUSHBUTTON | WS_TABSTOP,
                                  rightGroupX + 130, margin + 110, 80, 25, 
                                  hWnd, (HMENU)IDC_SEND_BTN, g_hInst, NULL);
        SendMessageW(hSend, WM_SETFONT, (WPARAM)g_hFont, TRUE);

        // Server status info
        CreateWindowW(L"STATIC", L"Endpoints: /api/v1/health  /api/v1/cards/events", 
                     WS_VISIBLE | WS_CHILD | SS_LEFT,
                     rightGroupX + 10, margin + 145, 260, 20, hWnd, NULL, g_hInst, NULL);
        CreateWindowW(L"STATIC", L"Method: HTTP POST with JSON payload", 
                     WS_VISIBLE | WS_CHILD | SS_LEFT,
                     rightGroupX + 10, margin + 165, 260, 20, hWnd, NULL, g_hInst, NULL);

        // Group 3: Event Log (Bottom - FULL WIDTH)
        HWND hGroup3 = CreateWindowW(L"BUTTON", L"Event Log", 
                                    WS_VISIBLE | WS_CHILD | BS_GROUPBOX,
                                    margin, logGroupTop, 580, logGroupHeight, 
                                    hWnd, (HMENU)IDC_STATIC_GROUP3, g_hInst, NULL);
        SendMessageW(hGroup3, WM_SETFONT, (WPARAM)g_hFont, TRUE);

        // Log list - takes most of the group space
        int logListHeight = logGroupHeight - 45;
        HWND hLog = CreateWindowW(L"LISTBOX", L"", 
                                 WS_VISIBLE | WS_CHILD | LBS_NOINTEGRALHEIGHT | WS_VSCROLL | 
                                 WS_HSCROLL | WS_TABSTOP | WS_BORDER,
                                 margin + 10, logGroupTop + 20, 560, logListHeight, 
                                 hWnd, (HMENU)IDC_LOG_LIST, g_hInst, NULL);
        SendMessageW(hLog, WM_SETFONT, (WPARAM)g_hFont, TRUE);
        SendMessageW(hLog, LB_SETHORIZONTALEXTENT, 1000, 0);

        // Clear log button - at bottom of log group
        HWND hClear = CreateWindowW(L"BUTTON", L"Clear Log", 
                                   WS_VISIBLE | WS_CHILD | BS_PUSHBUTTON | WS_TABSTOP,
                                   margin + 10, logGroupTop + 20 + logListHeight + 5, 70, 23, 
                                   hWnd, (HMENU)IDC_CLEAR_LOG, g_hInst, NULL);
        SendMessageW(hClear, WM_SETFONT, (WPARAM)g_hFont, TRUE);

        // Initialize PC/SC
        if (!g_cardManager.Initialize()) {
            MessageBoxW(hWnd, L"Failed to initialize PC/SC. Ensure smart card service is running.", 
                       L"Error", MB_OK | MB_ICONERROR);
        } else {
            g_cardManager.StartMonitoring([](const std::wstring& reader, bool present, const std::vector<BYTE>& atr) {
                PostMessageW(g_hMainWnd, WM_CARD_STATUS, present ? 1 : 0, 
                             (LPARAM)new std::pair<std::wstring, std::vector<BYTE>>(reader, atr));
            });
        }

        RefreshReadersList();
        LogMessage(L"Application started");
        return 0;
    }

    case WM_CARD_STATUS: {
        auto* data = (std::pair<std::wstring, std::vector<BYTE>>*)lParam;
        UpdateCardStatusUI(data->first, wParam == 1, data->second);
        delete data;
        return 0;
    }

    case WM_SERVER_RESPONSE: {
        wchar_t* msg = (wchar_t*)lParam;
        LogMessage(msg);

        if (wParam == 2) {
            MessageBoxW(hWnd, msg, L"Connection Test", 
                       MB_OK | (wParam ? MB_ICONINFORMATION : MB_ICONERROR));
        }
        free(msg);
        return 0;
    }

    case WM_COMMAND: {
        int wmId = LOWORD(wParam);
        switch (wmId) {
        case IDC_REFRESH_BTN:
            RefreshReadersList();
            return 0;

        case IDC_CONNECT_BTN: {
            wchar_t readerName[256] = {0};
            GetDlgItemTextW(hWnd, IDC_READER_COMBO, readerName, 256);

            if (wcscmp(readerName, L"No readers found") == 0) return 0;

            wchar_t btnText[32] = {0};
            GetDlgItemTextW(hWnd, IDC_CONNECT_BTN, btnText, 32);

            if (wcscmp(btnText, L"Connect") == 0) {
                if (g_cardManager.Connect(readerName)) {
                    SetDlgItemTextW(hWnd, IDC_CONNECT_BTN, L"Disconnect");
                    LogMessage(L"Connected to reader");

                    if (g_cardManager.IsCardPresent(readerName)) {
                        auto atr = g_cardManager.GetATR(readerName);
                        UpdateCardStatusUI(readerName, true, atr);
                    }
                } else {
                    LogMessage(L"Failed to connect to reader");
                    MessageBoxW(hWnd, L"Failed to connect to reader", L"Error", MB_OK | MB_ICONERROR);
                }
            } else {
                g_cardManager.Disconnect();
                SetDlgItemTextW(hWnd, IDC_CONNECT_BTN, L"Connect");
                SetDlgItemTextW(hWnd, IDC_STATUS_TEXT, L"Disconnected");
                EnableWindow(GetDlgItem(hWnd, IDC_READ_BTN), FALSE);
                LogMessage(L"Disconnected from reader");
            }
            return 0;
        }

        case IDC_READ_BTN:
            ReadCardAndSendData();
            return 0;

        case IDC_TEST_BTN:
            TestServerConnection();
            return 0;

        case IDC_SEND_BTN: {
            wchar_t cardIdW[256] = {0};
            GetDlgItemTextW(hWnd, IDC_CARD_ID, cardIdW, 256);
            if (wcslen(cardIdW) > 0) {
                g_lastCardId = WStringToString(cardIdW);
                ReadCardAndSendData();
            }
            return 0;
        }

        case IDC_CLEAR_LOG:
            SendDlgItemMessageW(hWnd, IDC_LOG_LIST, LB_RESETCONTENT, 0, 0);
            return 0;

        case IDC_READER_COMBO:
            if (HIWORD(wParam) == CBN_SELCHANGE) {
                g_cardManager.Disconnect();
                SetDlgItemTextW(hWnd, IDC_CONNECT_BTN, L"Connect");
                SetDlgItemTextW(hWnd, IDC_STATUS_TEXT, L"Select reader");
                EnableWindow(GetDlgItem(hWnd, IDC_READ_BTN), FALSE);
            }
            return 0;
        }
        break;
    }

    case WM_DESTROY:
        g_cardManager.Shutdown();
        DeleteObject(g_hFont);
        PostQuitMessage(0);
        return 0;

    case WM_SIZE: {
        // Handle window resizing - adjust log list
        RECT rc;
        GetClientRect(hWnd, &rc);

        HWND hLog = GetDlgItem(hWnd, IDC_LOG_LIST);
        HWND hGroup3 = GetDlgItem(hWnd, IDC_STATIC_GROUP3);
        HWND hClear = GetDlgItem(hWnd, IDC_CLEAR_LOG);

        if (hLog && hGroup3 && hClear) {
            int margin = 10;
            int logGroupTop = 210;
            int logGroupHeight = rc.bottom - logGroupTop - margin;
            int logListHeight = logGroupHeight - 45;

            // Resize group box
            SetWindowPos(hGroup3, NULL, margin, logGroupTop, rc.right - 2*margin, logGroupHeight, SWP_NOZORDER);

            // Resize log list
            SetWindowPos(hLog, NULL, margin + 10, logGroupTop + 20, rc.right - 2*margin - 20, logListHeight, SWP_NOZORDER);

            // Move clear button
            SetWindowPos(hClear, NULL, margin + 10, logGroupTop + 20 + logListHeight + 5, 70, 23, SWP_NOZORDER);
        }
        return 0;
    }
    }

    return DefWindowProcW(hWnd, message, wParam, lParam);
}

// Main application function
int RunApplication(HINSTANCE hInstance, int nCmdShow) {
    g_hInst = hInstance;

    INITCOMMONCONTROLSEX iccex;
    iccex.dwSize = sizeof(iccex);
    iccex.dwICC = ICC_STANDARD_CLASSES | ICC_LISTVIEW_CLASSES;
    InitCommonControlsEx(&iccex);

    WNDCLASSEXW wcex = {0};
    wcex.cbSize = sizeof(wcex);
    wcex.style = CS_HREDRAW | CS_VREDRAW;
    wcex.lpfnWndProc = MainWndProc;
    wcex.hInstance = hInstance;
    wcex.hIcon = LoadIconW(NULL, IDI_APPLICATION);
    wcex.hCursor = LoadCursorW(NULL, IDC_ARROW);
    wcex.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
    wcex.lpszClassName = L"SmartCardBridgeClass";
    wcex.hIconSm = LoadIconW(NULL, IDI_APPLICATION);

    if (!RegisterClassExW(&wcex)) {
        MessageBoxW(NULL, L"Failed to register window class", L"Error", MB_OK | MB_ICONERROR);
        return 1;
    }

    // FIXED: Larger window to accommodate log
    HWND hWnd = CreateWindowExW(0, L"SmartCardBridgeClass", 
                                L"Smart Card Bridge - PC/SC to Swift Server",
                                WS_OVERLAPPEDWINDOW & ~WS_THICKFRAME & ~WS_MAXIMIZEBOX,
                                CW_USEDEFAULT, CW_USEDEFAULT, 620, 480,
                                NULL, NULL, hInstance, NULL);

    if (!hWnd) {
        MessageBoxW(NULL, L"Failed to create window", L"Error", MB_OK | MB_ICONERROR);
        return 1;
    }

    ShowWindow(hWnd, nCmdShow);
    UpdateWindow(hWnd);

    MSG msg;
    while (GetMessageW(&msg, NULL, 0, 0)) {
        if (!IsDialogMessageW(hWnd, &msg)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
    }

    return (int)msg.wParam;
}

// Entry point for MinGW compatibility
int main() {
    return RunApplication(GetModuleHandle(NULL), SW_SHOWDEFAULT);
}