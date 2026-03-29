## SmartCardBridge Architecture Diagram

```
┌─────────────────────────────────────────────────────────────────────────────────┐
│                           SMARTCARDBRIDGE.EXE (Windows)                         │
│                         PC/SC to Swift Server Bridge                            │
├─────────────────────────────────────────────────────────────────────────────────┤
│                                                                                 │
│  ┌─────────────────────────┐    ┌─────────────────────────┐    ┌──────────────┐ │
│  │   WINDOWS GUI LAYER     │    │    PC/SC SMART CARD     │    │   HTTP CLIENT │ │
│  │  (Win32 API Controls)   │◄──►│       MANAGER           │◄──►│   (WinInet)   │ │
│  │                         │    │                         │    │               │ │
│  │ • Reader Combo Box      │    │ • SCardEstablishContext │    │ • POST /api/  │ │
│  │ • Connect/Refresh Btn   │    │ • SCardListReaders    │    │ • GET /health │ │
│  │ • Status Display        │    │ • SCardConnect        │    │ • JSON Payload│ │
│  │ • Card ID Field         │    │ • SCardTransmit       │    │ • X-API-Key   │ │
│  │ • ATR Display           │    │ • GetCardUID()        │    │               │ │
│  │ • Auto-send Checkbox    │    │ • GetATR()            │    │               │ │
│  │ • Event Log (ListBox)   │    │ • MonitorThread()     │    │               │ │
│  └─────────────────────────┘    └─────────────────────────┘    └───────┬───────┘ │
│           ▲                                    ▲                       │         │
│           │                                    │                       │         │
│           │    ┌───────────────────────────┐     │                       │         │
│           └───►│   CARD STATUS CALLBACK    │◄────┘                       │         │
│                │  (WM_CARD_STATUS messages)  │                            │         │
│                │  • Reader name              │                            │         │
│                │  • Present/Absent status    │                            │         │
│                │  • ATR bytes                │                            │         │
│                └───────────────────────────┘                            │         │
│                                                                       ▼         │
│  ┌─────────────────────────────────────────────────────────────────────────┐     │
│  │                    SERVER CONFIGURATION (UI Inputs)                       │     │
│  │  ┌─────────────┐  ┌─────────────┐  ┌─────────────┐  ┌─────────────────┐  │     │
│  │  │  HOST       │  │  PORT       │  │  API KEY    │  │  AUTO-SEND      │  │     │
│  │  │  localhost  │  │  8080       │  │  [hidden]   │  │  [☑ Checked]    │  │     │
│  │  │  (Edit)     │  │  (Edit)     │  │  (Password) │  │  (Checkbox)     │  │     │
│  │  └─────────────┘  └─────────────┘  └─────────────┘  └─────────────────┘  │     │
│  │                                                                         │     │
│  │  DEFAULT VALUES FROM CODE:                                               │     │
│  │  • Host: "localhost" (IDC_SERVER_HOST)                                  │     │
│  │  • Port: 8080 (IDC_SERVER_PORT)                                         │     │
│  │  • Endpoints: /api/v1/health, /api/v1/cards/events                     │     │
│  │  • Method: HTTP POST with JSON                                          │     │
│  └─────────────────────────────────────────────────────────────────────────┘     │
│                                                                       │         │
└───────────────────────────────────────────────────────────────────────┼─────────┘
                                                                        │
                                                                        │ HTTP/1.1
                                                                        ▼
┌─────────────────────────────────────────────────────────────────────────────────┐
│                           SWIFT SERVER (Remote/Local)                           │
│                                                                                   │
│  ┌─────────────────────────────────────────────────────────────────────────────┐ │
│  │  DEFAULT SWIFT SERVER CONFIGURATION (Vapor/Kitura/Perfect)                  │ │
│  │                                                                             │ │
│  │  ┌─────────────────┐  ┌─────────────────┐  ┌─────────────────┐               │ │
│  │  │   VAPOR         │  │   KITURA        │  │   PERFECT       │               │ │
│  │  │   (Most Common) │  │   (IBM)         │  │   (Legacy)      │               │ │
│  │  ├─────────────────┤  ├─────────────────┤  ├─────────────────┤               │ │
│  │  │ Host: 0.0.0.0   │  │ Host: 0.0.0.0   │  │ Host: 0.0.0.0   │               │ │
│  │  │ Port: 8080      │  │ Port: 8080/8090 │  │ Port: 8080      │               │ │
│  │  │ Config File:    │  │ Config: Code    │  │ Config: JSON/   │               │ │
│  │  │  servers.json   │  │  or Config file │  │  Dictionary     │               │ │
│  │  └─────────────────┘  └─────────────────┘  └─────────────────┘               │ │
│  │                                                                             │ │
│  │  ENDPOINTS EXPECTED BY BRIDGE:                                              │ │
│  │  • GET  /api/v1/health      → Returns server status                         │ │
│  │  • POST /api/v1/cards/events→ Receives card data JSON                       │ │
│  │                                                                             │ │
│  │  JSON PAYLOAD STRUCTURE:                                                     │ │
│  │  {                                                                          │ │
│  │    "readerName": "ACS ACR122U",                                             │ │
│  │    "cardId": "A1B2C3D4E5F6",                                                │ │
│  │    "atr": "3B 8F 80 01 80 4F 0C A0 00 00 03 06 0A 00 1B 00 00 00 00 7E",   │ │
│  │    "timestamp": "2026-03-28T21:33:00"                                        │ │
│  │  }                                                                          │ │
│  │                                                                             │ │
│  │  HEADERS SENT:                                                               │ │
│  │  • Content-Type: application/json                                           │ │
│  │  • X-API-Key: <user-provided-key>                                            │ │
│  │                                                                             │ │
│  └─────────────────────────────────────────────────────────────────────────────┘ │
│                                                                                   │
└───────────────────────────────────────────────────────────────────────────────────┘
```

## Data Flow Sequence

```
┌─────────┐     ┌─────────────┐     ┌─────────────┐     ┌─────────────┐     ┌─────────┐
│  USER   │────►│  INSERTS    │────►│  PC/SC      │────►│  BRIDGE     │────►│  SWIFT  │
│  ACTION │     │  SMART CARD │     │  DETECTS    │     │  READS UID  │     │  SERVER │
└─────────┘     └─────────────┘     └─────────────┘     └─────────────┘     └─────────┘
                                                                    │
                                                                    ▼
┌─────────┐     ┌─────────────┐     ┌─────────────┐     ┌─────────────┐     ┌─────────┐
│  SERVER │────►│  PROCESSES  │────►│  JSON POST  │────►│  HTTP POST  │────►│  AUTO-  │
│  RESPONSE    │  CARD DATA   │     │  TO /api/   │     │  WinInet    │     │  SEND?  │
│  (200 OK)    │              │     │  v1/cards/  │     │  Library    │     │  (Yes)  │
└─────────┘     └─────────────┘     └─────────────┘     └─────────────┘     └─────────┘
      ▲                                                                            │
      └────────────────────────────────────────────────────────────────────────────┘
                                    (If auto-send enabled)
```

## Key Configuration Details

### Network Parameters (Hardcoded Defaults)
| Parameter | Value | UI Control ID | Description |
|-----------|-------|---------------|-------------|
| **Host** | `localhost` | `IDC_SERVER_HOST` (1008) | Server IP/hostname |
| **Port** | `8080` | `IDC_SERVER_PORT` (1009) | TCP port number |
| **API Key** | *(empty)* | `IDC_API_KEY` (1010) | Authentication header |

### Swift Server Framework Defaults

Based on the code's default port **8080**, here are the matching Swift server configurations:

**Vapor (Most Common)** :
```swift
// Default configuration
app.http.server.configuration.hostname = "0.0.0.0"  // All interfaces
app.http.server.configuration.port = 8080            // Default port

// Or via command line:
swift run App serve --hostname 0.0.0.0 --port 8080
```

**Kitura (IBM)** :
```swift
// Default port is 8080 (older versions used 8090)
Kitura.addHTTPServer(onPort: 8080, with: router)
Kitura.run()
```

**Perfect** :
```swift
// Configuration dictionary
[
    "servers": [
        [
            "name": "localhost",
            "port": 8080,  // Default matches bridge
            "routes": [...]
        ]
    ]
]
```

### Communication Protocol

```
┌────────────────────────────────────────────────────────────────┐
│                    HTTP REQUEST FORMAT                          │
├────────────────────────────────────────────────────────────────┤
│ POST /api/v1/cards/events HTTP/1.1                               │
│ Host: localhost:8080                                             │
│ Content-Type: application/json                                   │
│ X-API-Key: <user-provided-secret-key>                            │
│ Content-Length: <calculated>                                     │
│                                                                 │
│ {"readerName":"ACS ACR122U PICC 0","cardId":"04A1B2C3D4E5F6",   │
│  "atr":"3B 8F 80 01 80 4F...","timestamp":"2026-03-28T21:33:45"}│
└────────────────────────────────────────────────────────────────┘

┌────────────────────────────────────────────────────────────────┐
│                    HEALTH CHECK REQUEST                         │
├────────────────────────────────────────────────────────────────┤
│ GET /api/v1/health HTTP/1.1                                     │
│ Host: localhost:8080                                             │
│ (API Key optional for health endpoint)                          │
└────────────────────────────────────────────────────────────────┘
```

## Threading Model

```
┌─────────────────────────────────────────────────────────┐
│                    MAIN UI THREAD                        │
│  • Window message pump (GetMessage/DispatchMessage)       │
│  • UI updates (SetDlgItemText, EnableWindow)             │
│  • Synchronous card operations                            │
├─────────────────────────────────────────────────────────┤
│                   MONITOR THREAD                         │
│  (SmartCardManager::MonitorThread)                        │
│  • Polls readers every 500ms                            │
│  • Posts WM_CARD_STATUS to main window                    │
│  • Detects card insert/remove events                      │
├─────────────────────────────────────────────────────────┤
│                   HTTP WORKER THREAD                     │
│  (Created per-request in ReadCardAndSendData)            │
│  • Non-blocking HTTP POST to Swift server                 │
│  • Posts WM_SERVER_RESPONSE on completion               │
└─────────────────────────────────────────────────────────┘
```

The bridge is designed to work with any Swift server framework (Vapor, Kitura, or Perfect) listening on **port 8080**, which is the universal default for development servers across all major Swift server-side frameworks .
