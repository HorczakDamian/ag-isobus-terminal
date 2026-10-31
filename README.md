# 🚜 isoVT — Precision Ag Virtual Terminal & AgOpenGPS Task Controller

<div align="center">

![C++17](https://img.shields.io/badge/C%2B%2B-17-00599C?style=for-the-badge&logo=c%2B%2B)
![ISO 11783](https://img.shields.io/badge/ISO%2011783-VT%20%7C%20TC--SC%20%7C%20AUX--N-2ea44f?style=for-the-badge)
![DirectX 11](https://img.shields.io/badge/Render-DirectX%2011-orange?style=for-the-badge)
![Platform](https://img.shields.io/badge/Platform-Windows%20x64-blue?style=for-the-badge&logo=windows)
![Build](https://img.shields.io/badge/Build-Nightbuild-purple?style=for-the-badge)
![License](https://img.shields.io/badge/License-MIT-lightgrey?style=for-the-badge)

**Nowoczesny, komercyjnej klasy Wirtualny Terminal ISOBUS (ISO 11783-6) zintegrowany z Kontrolerem Zadań i Kontrolą Sekcji dla AgOpenGPS.**

---

</div>

## 📌 Spis treści
- [O projekcie](#-o-projekcie)
- [Architektura isoVT](#-architektura-isovt)
- [Główne Funkcjonalności](#-główne-funkcjonalności)
  - [1. isoVT Server (ISO 11783-6)](#1-isovt-server-iso-11783-6)
  - [2. Kontroler Zadań AgOpenGPS (TC-SC & TECU)](#2-kontroler-zadań-agopengps-tc-sc--tecu)
  - [3. Obsługa Wielu Maszyn (Multi-Working Set & Mini-Views)](#3-obsługa-wielu-maszyn-multi-working-set--mini-views)
  - [4. Dźwiękowy System Alarmów (ISO 11783-6 Audio Beeper)](#4-dźwiękowy-system-alarmów-iso-11783-6-audio-beeper)
  - [5. Pływająca Nakładka Dokowalna (HUD Overlay F11)](#5-pływająca-nakładka-dokowalna-hud-overlay-f11)
  - [6. Wejścia AUX-N & Joysticki](#6-wejścia-aux-n--joysticki)
- [Szybki Start & Pobieranie (Nightbuild)](#-szybki-start--pobieranie-nightbuild)
- [Budowanie ze Źródeł (CMake)](#-budowanie-ze-źródeł-cmake)
- [Uruchomienie z AgOpenGPS](#-uruchomienie-z-agopengps)
- [Klawisze i Skróty](#-klawisze-i-skróty)
- [Wspierany Sprzęt CAN](#-wspierany-sprzęt-can)
- [Licencja & Standardy](#-licencja--standardy)

---

## 📖 O projekcie

**isoVT** to wysokowydajny, niezależny Wirtualny Terminal ISOBUS (**Universal Terminal / Virtual Terminal Server**) pracujący pod kontrolą silnika graficznego **DirectX 11** i **Dear ImGui**.

Projekt został zaprojektowany z myślą o rolnictwie precyzyjnym — stanowi bezpośredni pomost między maszynami ISOBUS (siewniki, opryskiwacze, rozsiewacze, prasy) a popularnym systemem nawigacji rolniczej **AgOpenGPS**. Dzięki bezpośredniej wymianie danych UDP terminal realizuje automatyczną kontrolę sekcji roboczych (**Section Control**) oraz przekazuje rzeczywistą prędkość ciągnika z anteny GPS (**TECU**) prosto na magistralę CAN.

---

## 🏛 Architektura isoVT

```text
 ┌────────────────────────────────────────────────────────┐
 │                         isoVT                          │
 │                 DirectX 11 Core Engine                 │
 └──────────────────────────┬─────────────────────────────┘
                            │
       ┌────────────────────┴────────────────────┐
       ▼                                         ▼
┌───────────────────────────────┐     ┌───────────────────────────────┐
│       isoVT Server Core       │     │    AgOpenGPS Task Controller  │
│  - ISO 11783-6 Parser & VT    │     │  - Section Control (do 64 sek)│
│  - Fast-Boot IOP Cache (2ms)  │     │  - Wirtualne TECU (GPS Speed) │
│  - Multi-Working Set & MiniVW │     │  - Variable Rate (TC-GEO)     │
│  - ISO Audio Beeper Thread    │     │  - Pętla UDP (Port 8888/9999) │
│  - AUX-N Joystick Mapping     │     │                               │
└──────────────┬────────────────┘     └──────────────┬────────────────┘
               │                                     │
               ▼                                     ▼
      ┌─────────────────┐                   ┌─────────────────┐
      │  Magistrala CAN │                   │    AgOpenGPS    │
      │ (PEAK / ESP32)  │                   │  (Nawigacja GPS)│
      └─────────────────┘                   └─────────────────┘
```

---

## 🌾 Główne Funkcjonalności

### 1. isoVT Server (ISO 11783-6)
- **Kompletna obsługa obiektów ISOBUS:** Kontenery, przyciski aktywne, pola tekstowe i liczbowe, wskaźniki analogowe (Meters), paski postępu, łuki, elipsy, bitmapy i palety barw ISO.
- **Fast-Boot IOP Cache:** Automatyczny zapis oraz natychmiastowe ładowanie puli obiektów (`.iop`) z dysku w zaledwie ~2 ms.
- **Dynamiczne skalowanie:** Tryb dopasowania do rozdzielczości ekranu w kabinie (100%, 125%, 150%, Auto-Fit).
- **Dotykowy interfejs:** Wbudowana klawiatura numeryczna oraz alfanumeryczna dla ekranów dotykowych w ciągnikach.

### 2. Kontroler Zadań AgOpenGPS (TC-SC & TECU)
- **Automatyczna Kontrola Sekcji (Section Control):** Obsługa do 64 sekcji roboczych. Wyłączanie poszczególnych sekcji na nakładkach oraz granicach pola wyznaczonych w AgOpenGPS.
- **Wirtualne TECU (Tractor ECU):** Przesyłanie prędkości ciągnika z GPS (PGN `0xFE48` / `0xFE49`) do maszyn na magistrali CAN.
- **Płynna synchronizacja UDP:** Błyskawiczna komunikacja bez lagów z AgIO (nasłuch `8888`, nadawanie `9999`).

### 3. Obsługa Wielu Maszyn (Multi-Working Set & Mini-Views)
- Równoczesna praca kilku sterowników ISOBUS (np. przedni zbiornik + tylny siewnik).
- **Mini-Views:** Pływające kafelki z podglądem na żywo ekranów maszyn działających w tle z możliwością natychmiastowego przełączenia jednym kliknięciem.

### 4. Dźwiękowy System Alarmów (ISO 11783-6 Audio Beeper)
- Dedykowany, bezblokujący wątek akustyczny (brak przycięć klatek animacji).
- Dźwięki dotyku ekranu (`TouchClick` 2400 Hz) oraz klawiszy funkcyjnych (`SoftKeyClick` 2100 Hz).
- Akustyczne potrójne impulsy alarmowe ISO (980 Hz) ponawiane cyklicznie co 1.5 sekundy aż do wciśnięcia przycisku **ACK**.

### 5. Pływająca Nakładka Dokowalna (HUD Overlay F11)
- Klawisz **`F11`** przełącza isoVT w przezroczystą nakładkę HUD (`WS_EX_TOPMOST` + `WS_EX_LAYERED`).
- Terminal unosi się stabilnie nad mapą AgOpenGPS, umożliwiając jednoczesną obserwację linii nawigacyjnych i parametrów maszyny.

### 6. Wejścia AUX-N & Joysticki
- Zgodność z normą ISO 11783-9 (Auxiliary Inputs New).
- Mapowanie fizycznych przycisków i osi joysticków USB / CAN na funkcje maszyn roboczych.

---

## 📦 Szybki Start & Pobieranie (Nightbuild)

Najnowszą wersję binarną bez konieczności kompilowania pobierzesz w zakładce [**Releases**](../../releases):

1. Pobierz archiwum **`isoVT-Nightbuild-Win64.zip`**.
2. Rozpakuj je w dowolnym folderze.
3. Podłącz swój interfejs CAN (np. PEAK PCAN-USB) lub uruchom tryb wirtualny.
4. Uruchom aplikację plikiem **`run_terminal.bat`** lub **`isobus_vt_terminal.exe`**.

---

## 🛠️ Budowanie ze Źródeł (CMake)

### Wymagania:
- System: **Windows 10 / 11** (x64)
- Kompilator: **MSVC (Visual Studio 2019 / 2022)** z obsługą C++17
- Narzędzie: **CMake 3.16+**
- Karta graficzna z obsługą **DirectX 11**

### Kompilacja:
```powershell
# 1. Klonowanie repozytorium
git clone https://github.com/horczakdamian24-sketch/ag-isobus-terminal.git
cd ag-isobus-terminal

# 2. Generowanie plików (CMake automatycznie pobierze wymagane biblioteki)
cmake -B build -DCMAKE_BUILD_TYPE=Release

# 3. Kompilacja
cmake --build build --config Release
```

Gotowy plik wykonywalny powstanie w folderze:  
`build/isobus_vt_terminal/Release/isobus_vt_terminal.exe`

---

## 🚜 Uruchomienie z AgOpenGPS

1. Włącz **AgOpenGPS** oraz moduł **AgIO**.
2. Włącz automatyczną kontrolę sekcji (ikona opryskiwacza/sekcji na zielono).
3. Uruchom **isoVT** (`run_terminal.bat`).
4. Terminal połączy się automatycznie z AgIO przez port UDP `8888`.
5. Wciśnij **`F11`**, aby zadokować terminal jako przezroczystą nakładkę HUD obok mapy prowadzenia ciągnika.

---

## ⌨️ Klawisze i Skróty

| Klawisz | Funkcja |
| :--- | :--- |
| **`F11`** | Przełącz tryb przezroczystej nakładki HUD / Studio |
| **`F2`** | Konfiguracja sprzętowa CAN i portów AgOpenGPS |
| **`F1` – `F6`** | Klawisze programowe SoftKeys (prawa kolumna) |
| **`F7` – `F12`**| Klawisze programowe SoftKeys (lewa kolumna) |
| **`ESC`** | Zamknięcie okna dialogowego / reset widoku |
| **`Enter`** | Zatwierdzenie wartości liczbowej |

---

## 🔌 Wspierany Sprzęt CAN

- **PEAK-System:** PCAN-USB, PCAN-USB Pro, PCAN-PCIe.
- **ESP32 / Arduino:** Dowolny sterownik z transceiverem CAN wysyłający ramki 250 kbps (J1939 / ISO 11783).
- **Virtual CAN:** Wbudowany symulator magistrali w pamięci RAM do testów bez fizycznego adaptera.

---

## 📄 Licencja & Standardy

Projekt udostępniany na licencji **MIT**. Zgodny ze standardami:
- **ISO 11783-6:** Ciągniki i maszyny rolnicze – Część 6: Wirtualny Terminal (VT)
- **ISO 11783-10:** Część 10: Kontroler Zadań (Task Controller & Section Control)

Precision Agriculture & ISOBUS Architecture
