# 🚜 isoVT — Precision Ag Virtual Terminal & AgOpenGPS Task Controller

<div align="center">

![C++17](https://img.shields.io/badge/Language-C%2B%2B17-00599C?style=for-the-badge&logo=c%2B%2B)
![ISO 11783](https://img.shields.io/badge/Standard-ISO_11783--6_%7C_10_%7C_9-2ea44f?style=for-the-badge)
![DirectX 11](https://img.shields.io/badge/Graphics-DirectX_11_%2F_ImGui-orange?style=for-the-badge)
![Platform](https://img.shields.io/badge/Platform-Windows_10_%2F_11_x64-blue?style=for-the-badge&logo=windows)
![Build](https://img.shields.io/badge/Release-Nightbuild_(All--in--One)-purple?style=for-the-badge)
![License](https://img.shields.io/badge/License-MIT-lightgrey?style=for-the-badge)

**Komercyjnej klasy Wirtualny Terminal ISOBUS (ISO 11783-6) zintegrowany z Kontrolerem Zadań i Kontrolą Sekcji dla AgOpenGPS.**

[Pobierz gotowy pakiet (Releases)](../../releases) • [Instrukcja sterowników](#-3-wymagania-sprzętowe-i-instalacja-sterowników) • [Szybki Start](#-4-szybki-start-i-uruchomienie) • [Budowanie z kodu](#-7-kompilacja-ze-źródeł-cmake)

---

</div>

## 📌 Spis Treści
- [1. O projekcie isoVT](#-1-o-projekcie-isovt)
- [2. Architektura Systemu](#-2-architektura-systemu)
- [3. Wymagania Sprzętowe i Instalacja Sterowników](#-3-wymagania-sprzętowe-i-instalacja-sterowników)
  - [A. Adaptery PEAK PCAN-USB (Zalecane)](#a-adaptery-peak-pcan-usb-zalecane)
  - [B. Tryb Wirtualny (Virtual CAN)](#b-tryb-wirtualny-virtual-can)
  - [C. Biblioteki Microsoft Visual C++](#c-biblioteki-uruchomieniowe-visual-c)
- [4. Szybki Start i Uruchomienie](#-4-szybki-start-i-uruchomienie)
  - [Krok 1: Pobranie paczki All-in-One](#krok-1-pobranie-paczki-all-in-one)
  - [Krok 2: Uruchomienie środowiska](#krok-2-uruchomienie-środowiska)
  - [Krok 3: Przełączenie w tryb HUD (F11)](#krok-3-przełączenie-w-tryb-hud-f11)
- [5. Szczegółowy Opis Funkcjonalności](#-5-szczegółowy-opis-funkcjonalności)
  - [Wirtualny Terminal ISO 11783-6](#-wirtualny-terminal-iso-11783-6)
  - [Kontroler Zadań AgOpenGPS (TC-SC & TECU)](#-kontroler-zadań-agopengps-tc-sc--tecu)
  - [Obsługa Wielu Maszyn (Multi-Working Set & Mini-Views)](#-obsługa-wielu-maszyn-multi-working-set--mini-views)
  - [Dźwiękowy System Alarmów (Audio Beeper)](#-dźwiękowy-system-alarmów-audio-beeper)
  - [Pływająca Nakładka Dokowalna (HUD Overlay)](#-pływająca-nakładka-dokowalna-hud-overlay)
- [6. Skróty Klawiszowe i Sterowanie](#-6-skróty-klawiszowe-i-sterowanie)
- [7. Kompilacja ze Źródeł (CMake)](#-7-kompilacja-ze-źródeł-cmake)
- [8. Licencja i Zgodność](#-8-licencja-i-zgodność)

---

## 📖 1. O projekcie isoVT

**isoVT** to niezależny, nowoczesny Wirtualny Terminal ISOBUS (**Universal Terminal Server**) zoptymalizowany dla rolnictwa precyzyjnego. Aplikacja została zbudowana od podstaw w oparciu o silnik graficzny **DirectX 11** oraz bibliotekę **Dear ImGui**, zapewniając stabilne renderowanie masek maszyn rolniczych w 60 FPS przy minimalnym obciążeniu procesora w kabinie ciągnika.

Projekt stanowi gotowy pomost łączący dowolną maszynę ISOBUS (opryskiwacze polowe, siewniki zbożowe i punktowe, rozsiewacze nawozów, prasy belujące) z otwartym systemem nawigacji **AgOpenGPS**. Poprzez wbudowany kontroler zadań (**Task Controller**) terminal automatycznie zarządza sekcjami roboczymi (**Section Control** do 64 sekcji) oraz przesyła do maszyny prędkość ciągnika z anteny GPS (**wirtualne TECU**).

---

## 🏛 2. Architektura Systemu

```text
 ┌────────────────────────────────────────────────────────────────────────┐
 │                                 isoVT                                  │
 │                     DirectX 11 & Dear ImGui Engine                     │
 └───────────────────────────────────┬────────────────────────────────────┘
                                     │
       ┌─────────────────────────────┴─────────────────────────────┐
       ▼                                                           ▼
┌─────────────────────────────────────────┐     ┌─────────────────────────────────────────┐
│          isoVT Server Core              │     │        AgOpenGPS Task Controller        │
│  - Parser i renderer masek ISO 11783-6  │     │  - Kontrola sekcji (SC do 64 sekcji)    │
│  - Fast-Boot IOP Disk Cache (~2 ms)     │     │  - Wirtualne TECU (PGN 0xFE48 / 0xFE49) │
│  - Multi-Working Set & Mini-Views       │     │  - Variable Rate (TC-GEO)               │
│  - ISO Audio Beeper (wątek asynchro)    │     │  - Szybka pętla UDP (Port 8888 / 9999)  │
│  - Obsługa dżojstików i mapowanie AUX-N │     │                                         │
└────────────────────┬────────────────────┘     └────────────────────┬────────────────────┘
                     │                                               │
                     ▼                                               ▼
         ┌───────────────────────┐                       ┌───────────────────────┐
         │     Magistrala CAN    │                       │       AgOpenGPS       │
         │  (PEAK PCAN-USB / FD) │                       │     (AgIO / Nawigacja)│
         └───────────────────────┘                       └───────────────────────┘
```

---

## 🔌 3. Wymagania Sprzętowe i Instalacja Sterowników

Aby połączyć terminal **isoVT** z fizyczną maszyną rolniczą na magistrali CAN, upewnij się, że posiadasz odpowiedni adapter i zainstalowane sterowniki.

### A. Adaptery PEAK PCAN-USB (Zalecane) ⭐⭐⭐
Interfejsy firmy **PEAK-System** (PCAN-USB, PCAN-USB FD, PCAN-USB Pro) to branżowy standard diagnostyki i komunikacji ISOBUS.

1. **Pobierz oficjalny instalator sterowników:**
   - Przejdź na oficjalną stronę producenta:  
     👉 [**PEAK-System Device Driver Setup (DrvSetup.exe)**](https://www.peak-system.com/quick/DrvSetup)
2. **Instalacja krok po kroku:**
   - Uruchom pobrany plik `PEAK-System_Driver-Setup.exe`.
   - Zaznacz pozycję **CAN hardware (PCAN-USB)**.
   - Dokończ instalację i podłącz adapter do portu USB w komputerze.
   - W systemowym Menedżerze Urządzeń pojawi się kategoria *CAN-Hardware -> PCAN-USB*.
3. **Plik biblioteki `PCANBasic.dll`:**
   - Wersja 64-bitowa biblioteki `PCANBasic.dll` jest **już dołączona** bezpośrednio do paczki isoVT. Nie musisz niczego kopiować ręcznie.
4. **Parametry magistrali w ciągniku:**
   - Prędkość magistrali ISOBUS: **250 kbit/s** (250000 baud).

---

### B. Tryb Wirtualny (Virtual CAN)
Nie masz jeszcze fizycznego adaptera CAN?  
Terminal **isoVT** posiada wbudowany symulator magistrali CAN w pamięci RAM. Wystarczy w oknie konfiguracji (`F2` lub menu *Hardware -> Connect Virtual CAN*) wybrać tryb wirtualny, aby testować kontrolę sekcji i mostek AgOpenGPS bezpośrednio na biurku.

---

### C. Biblioteki Uruchomieniowe Visual C++
Do paczki All-in-One dołączono wszystkie niezbędne pliki uruchomieniowe (`msvcp140.dll`, `vcruntime140.dll`). Gdyby jednak na bardzo świeżym systemie Windows program zgłaszał brak składników C++, zainstaluj oficjalny pakiet Microsoftu:
- [Visual C++ 2015-2022 Redistributable (x64)](https://aka.ms/vs/17/release/vc_redist.x64.exe)

---

## 🚀 4. Szybki Start i Uruchomienie

### Krok 1: Pobranie paczki All-in-One
Pobierz najnowszą, kompletną paczkę ze strony wydań:
👉 [**Pobierz `isoVT-Nightbuild-Win64.zip`**](../../releases)

Rozpakuj archiwum w dowolnym folderze (np. na Pulpicie). Program jest całkowicie przenośny (**Portable**) – nie wymaga instalacji.

### Krok 2: Uruchomienie środowiska
W rozpakowanym folderze znajdziesz przygotowane skrypty startowe:

- **Opcja A (Uruchomienie automatyczne całego zestawu):**  
  Uruchom plik **`run_all.bat`** i wybierz opcję `[2]`. Skrypt w odpowiedniej kolejności uruchomi moduł komunikacyjny **AgIO**, nawigację **AgOpenGPS** oraz terminal **isoVT**.
- **Opcja B (Uruchomienie samego terminala):**  
  Uruchom plik **`run_terminal.bat`** (lub bezpośrednio `isobus_vt_terminal.exe`).

### Krok 3: Przełączenie w tryb HUD (F11)
Po uruchomieniu AgOpenGPS naciśnij klawisz **`F11`** na klawiaturze. Terminal natychmiast zamieni się w **pływającą, przezroczystą nakładkę dokowalną** obok mapy prowadzenia, ukrywając zbędne panele i wyświetlając wyłącznie ekran Twojej maszyny rolniczej.

---

## 🌾 5. Szczegółowy Opis Funkcjonalności

### 🖥️ Wirtualny Terminal ISO 11783-6
- **Pełny silnik obiektów VT (Wersje 3 i 4):** Kontenery, przyciski aktywne, etykiety tekstowe i numeryczne, wykresy słupkowe, wskaźniki analogowe (Meters), wielokąty, łuki, elipsy, grafiki bitmapowe oraz palety kolorów ISOBUS.
- **Fast-Boot IOP Cache:** Automatyczny zapis puli obiektów maszyny do lokalnej pamięci podręcznej (`.iop`). Kolejne uruchomienie maszyny następuje w **~2 milisekundy** bez konieczności ponownego przesyłania danych przez magistralę CAN (ETP).
- **Dotykowy interfejs:** Wirtualna klawiatura numeryczna oraz alfanumeryczna umożliwiająca wprowadzanie nastaw bezpośrednio palcem na ekranie w kabinie ciągnika.

### ⚡ Kontroler Zadań AgOpenGPS (TC-SC & TECU)
- **Automatyczna Kontrola Sekcji (Section Control):** Obsługa do 64 niezależnych sekcji belki opryskiwacza lub rzędów siewnika. Terminal odbiera stany nakładek z AgOpenGPS przez protokół UDP i natychmiast wysyła komendy wyłączenia sekcji do sterownika maszyny na magistrali CAN.
- **Wirtualne TECU (Tractor ECU):** Przesyłanie prędkości rzeczywistej ciągnika z anteny GPS (PGN `0xFE48` / `0xFE49`) do maszyn rolniczych, zapewniając precyzyjne dawkowanie nawozu i nasion niezależnie od poślizgu kół.
- **Porty UDP:** Domyślny port nasłuchu `8888`, port nadawania `9999` (możliwość zmiany w oknie konfiguracji `F2`).

### 🖼️ Obsługa Wielu Maszyn (Multi-Working Set & Mini-Views)
- Równoczesna obsługa kilku maszyn podłączonych do ciągnika (np. przedni zbiornik z nawozem + tylny siewnik precyzyjny).
- **Mini-Views:** Pływające kafelki z miniaturowym renderingiem pulpitów maszyn działających w tle.
- 1-kliknięciowe przełączanie maszyny aktywnej bezpośrednio z miniatury.

### 🔊 Dźwiękowy System Alarmów (Audio Beeper)
- Dedykowany, bezblokujący wątek dźwiękowy operujący w tle (brak spadków FPS).
- Akustyczne potwierdzenia dotknięcia ekranu (`TouchClick` 2400 Hz) oraz wciśnięcia klawiszy funkcyjnych (`SoftKeyClick` 2100 Hz).
- Zgodny ze standardem potrójny impuls alarmowy ISO (980 Hz ponawiany co 1.5 sekundy) uciszany wciśnięciem przycisku **ACK**.

### 🪟 Pływająca Nakładka Dokowalna (HUD Overlay)
- Aktywowana klawiszem **`F11`**.
- Zbudowana w technologii `WS_EX_TOPMOST` oraz `WS_EX_LAYERED` z regulowaną przezroczystością (Acrylic Glass).
- Automatyczne dokowanie do prawej lub lewej krawędzi ekranu z zachowaniem pełnej widoczności mapy nawigacji AgOpenGPS.

---

## ⌨️ 6. Skróty Klawiszowe i Sterowanie

| Klawisz | Funkcja |
| :--- | :--- |
| **`F11`** | **Przełącznik trybu HUD:** Zmiana między pełnym trybem Studio a przezroczystą nakładką nad AgOpenGPS |
| **`F2`** | **Ustawienia Terminala:** Konfiguracja interfejsu CAN, portów UDP, przezroczystości i audio |
| **`F1` – `F6`** | Klawisze programowe SoftKeys (prawa kolumna terminala) |
| **`F7` – `F12`**| Klawisze programowe SoftKeys (lewa kolumna terminala) |
| **`ESC`** | Zamknięcie aktywnego okna modalnego / wyjście z menu |
| **`Enter`** | Zatwierdzenie wartości liczbowej w oknie edycji |

---

## 🛠 7. Kompilacja ze Źródeł (CMake)

Dla programistów chcących rozwijać lub modyfikować projekt:

### Wymagania:
- System: **Windows 10 / 11** (x64)
- Kompilator: **MSVC (Visual Studio 2019 lub 2022)** z obsługą standardu C++17
- Narzędzie: **CMake 3.16+**
- Karta graficzna z obsługą **DirectX 11**

### Instrukcja kompilacji:
```powershell
# 1. Sklonuj repozytorium
git clone https://github.com/horczakdamian24-sketch/ag-isobus-terminal.git
cd ag-isobus-terminal

# 2. Skonfiguruj projekt CMake (CMake automatycznie pobierze wymagany stos AgIsoStack++ przez Git)
cmake -B build -DCMAKE_BUILD_TYPE=Release

# 3. Zbuduj plik binarny Release
cmake --build build --config Release
```

Skompilowany plik wykonywalny znajdziesz w:  
`build/isobus_vt_terminal/Release/isobus_vt_terminal.exe`

---

## 📄 8. Licencja i Zgodność

Projekt jest udostępniany na licencji **MIT** — możesz swobodnie z niego korzystać, modyfikować i adaptować do własnych zastosowań rolniczych.

Zgodność z normami międzynarodowymi:
- **ISO 11783-6:** Wirtualny Terminal (Virtual Terminal - VT3 & VT4)
- **ISO 11783-10:** Kontroler Zadań (Task Controller - TC-SC / TC-BAS)
- **ISO 11783-9:** Wejścia pomocnicze (AUX-N)
- **ISO 11783-7:** Komendy językowe i jednostki miar

*Precision Agriculture & Open-Source ISOBUS Architecture.*
