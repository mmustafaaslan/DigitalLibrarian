# 📚 Digital Librarian (ESP32-S3)

[![Hardware: ESP32-S3](https://img.shields.io/badge/Hardware-ESP32--S3-orange.svg)](https://www.espressif.com/en/products/socs/esp32-s3)
[![UI: LVGL](https://img.shields.io/badge/UI-LVGL%208.3-blue.svg)](https://lvgl.io/)
[![License: MIT](https://img.shields.io/badge/License-MIT-yellow.svg)](LICENSE)

**Digital Librarian** is a smart physical media management system running on the **Waveshare 7" ESP32-S3 Touch LCD**. It acts as a "physical search engine" for your library: simply search for an item on the screen or your phone, and an **addressable LED strip** will instantly light up its exact location on your shelf.

---

## 📸 Gallery

| Main Interface | Smart Filtering |
|:---:|:---:|
| <img src="docs/images/main_view.jpg" width="400"> | <img src="docs/images/filter_cds.jpg" width="400"> |
| *High-fidelity browse view* | *Drill down by genre or decade* |

| Add New Media | Dynamic Themes |
|:---:|:---:|
| <img src="docs/images/add_new_cd.jpg" width="400"> | <img src="docs/images/theme_selection.jpg" width="400"> |
| *Barcode & ISBN lookup* | *Customizable CD/Book themes* |

---

## 📖 User Guide

### Overview
Digital Librarian turns your media shelf into an interactive experience. Use the 7-inch touch screen or your smartphone to browse, search, and physically locate items.

### Features
*   **Browsing**: Swipe through your collection with instant cover art loading.
*   **Searching**: Tap the search icon to find items by Title, Artist, or Author. Real-time filtering makes finding "Sci-Fi from the 80s" a breeze.
*   **Locating**: Tap any item to light up its exact position on your shelf via the LED strip.
*   **Adding Items**:
    1.  Tap **Add New**.
    2.  Scan the barcode or enter the ISBN/UPC.
    3.  Metadata and cover art are automatically fetched from MusicBrainz (CDs) or Google Books.
*   **Web Interface**: Access `http://digitallibrarian.local` on your phone to manage your library from the comfort of your couch.

---

## 👨‍💻 Developer Guide

### Technical Specifications
*   **MCU**: ESP32-S3 (Xtensa LX7 Dual Core, 240MHz)
*   **Memory**: 8MB PSRAM (OPI), 16MB Flash (QSPI)
*   **Display**: 7-inch IPS (1024x600) with Capacitive Touch (GT911)
*   **Connectivity**: 2.4GHz WiFi
*   **Peripherals**:
    *   **SD Card**: SDMMC (4-bit mode) for database and image storage.
    *   **LEDs**: WS2812B (Data Pin: GPIO 6)
    *   **IO Expander**: CH422G (via I2C) for backlight and reset control.

### Setup & Installation
1.  **Dependencies**:
    *   **Arduino IDE**
    *   **ESP32 Board Package** (by Espressif)
    *   LVGL 8.3
    *   ArduinoJson 6.x
    *   FastLED

2.  **Configuration**:
    *   Rename `secrets.example.h` to `secrets.h` and add your WiFi credentials.
    *   Copy the `/libs` folder to your library path if not using PlatformIO's lib_deps.

3.  **Compiling**:
    *   **Board**: `esp32s3`
    *   **Flash Mode**: QIO 80MHz
    *   **PSRAM**: OPI (Critical for LVGL performance)

### Architecture
The system uses a **Dual-Core Architecture** to ensure smooth UI performance:
*   **Core 1 (UI Task)**: Runs the LVGL loop. Handles touch input, animations, and rendering.
*   **Core 0 (Background Task)**: Handles heavy lifting:
    *   WiFi / API Requests (MusicBrainz, Google Books)
    *   SD Card I/O (Database reads, Cover art caching)
    *   LED Control (FastLED timing)

```mermaid
graph TD
    User((User)) -->|Touch| UI[LVGL UI - Core 1]
    User -->|Browser| Web[Web Interface]
    
    UI -->|Events| Core[Core Logic]
    Web -->|API| Core
    
    Core -->|Job Queue| BW[Background Worker - Core 0]
    BW -->|I/O| SD[(SD Card)]
    BW -->|Fetch| API[MusicBrainz/Google Books]
    
    Core -->|Update| LED[FastLED Strip]
```

### Hardware Connections
```mermaid
graph LR
    PSU[5V 3A Power] -->|USB-C| ESP[ESP32-S3]
    ESP -->|I2C| IO[IO Expander]
    ESP -->|RGB Interface| LCD[7 Inch Touch Screen]
    ESP -->|GPIO 6| LED[WS2812B Strip]
    ESP -->|SDMMC| SD[SD Card]
    
    IO -->|Control| LCD_BL[Backlight]
    IO -->|Control| LCD_RST[Touch Reset]
```

### "Add Item" Workflow
```mermaid
sequenceDiagram
    participant User
    participant UI as Touch UI
    participant Core as Core Logic
    participant API as MusicBrainz/Discogs
    participant SD as SD Storage

    User->>UI: Scans Barcode
    UI->>Core: Request Lookup (ISBN/UPC)
    Core->>API: GET Metadata
    API-->>Core: JSON Data
    Core->>SD: Download Cover Art
    Core->>SD: Save details.json
    Core-->>UI: Show Cover & Success
    Core->>User: LED Blinks Green
```

### "Search & Locate" Workflow
```mermaid
sequenceDiagram
    participant User
    participant Web as Web Interface
    participant ESP as ESP32 Core
    participant LED as Addressable Strip

    Note over User, Web: User searches for "Pink Floyd"
    User->>Web: Type "Pink Floyd"
    Web->>ESP: GET /api/search?q=Pink
    ESP-->>Web: JSON [Results List]
    
    User->>Web: Select "Dark Side of the Moon"
    Web->>ESP: POST /api/locate {id: 123}
    ESP->>LED: Set Pixel #123 to CYAN
    ESP->>LED: Pulse Effect
    ESP-->>Web: 200 OK
    Web->>User: "Item Located!"
```

---

## 📜 License
This project is licensed under the MIT License. See [LICENSE](LICENSE) for details.

## 🙏 Credits
*   **Metadata**: [MusicBrainz](https://musicbrainz.org/), [Google Books](https://books.google.com/), [Discogs](https://www.discogs.com/), and [Apple iTunes](https://www.apple.com/itunes/).
*   **UI Framework**: [LVGL](https://lvgl.io/).
*   **Logic**: Powered by ESP32-S3.
