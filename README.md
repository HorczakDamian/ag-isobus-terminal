# 🌾 AG-ISOBUS PRO — Precision Ag Universal Terminal & Task Controller

<div align="center">

![C++17](https://img.shields.io/badge/Language-C%2B%2B17-00599C?style=for-the-badge&logo=c%2B%2B)
![ISO 11783](https://img.shields.io/badge/Standard-ISO_11783--6_%2F_ISO_11783--10-2ea44f?style=for-the-badge)
![DirectX 11](https://img.shields.io/badge/Render-DirectX_11_%2F_Dear_ImGui-orange?style=for-the-badge)
![Platform](https://img.shields.io/badge/Platform-Windows_10_%2F_11_x64-blue?style=for-the-badge&logo=windows)
![Release](https://img.shields.io/badge/Release-v1.0.0-success?style=for-the-badge)
![License](https://img.shields.io/badge/License-MIT-lightgrey?style=for-the-badge)

**Komercyjnej klasy wirtualny terminal rolniczy (ISOBUS Virtual Terminal) zintegrowany z kontrolerem zadań (Task Controller) dla AgOpenGPS.**

*System Architecture & Precision Ag Engineering by **D. Horczak***

---

</div>

## 📌 Spis treści
- [O projekcie](#-o-projekcie)
- [Architektura Systemu](#-architektura-systemu)
- [Główne Funkcjonalności](#-główne-funkcjonalności)
  - [1. ISO 11783-6 Virtual Terminal Server (VT/UT)](#1-iso-11783-6-virtual-terminal-server-vtut)
  - [2. Kontroler Zadań AgOpenGPS (TC-SC & TC-BAS)](#2-kontroler-zadań-agopengps-tc-sc--tc-bas)
  - [3. Obsługa Wielu Maszyn (Multi-Working Set & Mini-Views)](#3-obsługa-wielu-maszyn-multi-working-set--mini-views)
  - [4. Dźwiękowy System Alarmów (ISO 11783-6 Audio Beeper)](#4-dźwiękowy-system-alarmów-iso-11783-6-audio-beeper)
  - [5. Pływająca Nakładka Dokowalna (HUD Overlay F11)](#5-pływająca-nakładka-dokowalna-hud-overlay-f11)
  - [6. Wejścia AUX-N i Joysticki](#6-wejścia-aux-n-i-joysticki)
- [Pobieranie i Gotowe Wydanie (Release v1.0.0)](#-pobieranie-i-gotowe-wydanie-release-v100)
- [Budowanie ze Źródeł (CMake)](#-budowanie-ze-źródeł-cmake)
- [Konfiguracja z AgOpenGPS](#-konfiguracja-z-agopengps)
- [Klawiszologia i Skróty](#-klawiszologia-i-skróty)
- [Wspierany Sprzęt CAN](#-wspierany-sprzęt-can)
- [Licencja & Informacje Prawne](#-licencja--informacje-prawne)

---

## 📖 O projekcie

**AG-ISOBUS PRO** to nowoczesna, wysoce zoptymalizowana stacja dokująca i wirtualny terminal rolniczy (Virtual Terminal) pracujący w standardzie **ISO 11783 (ISOBUS)**. 

Aplikacja łączy w sobie pełny terminal graficzny dla maszyn rolniczych (opryskiwacze, siewniki, rozsiewacze nawozów, prasy) z dwukierunkowym mostkiem kontrolera zadań dla otwartego systemu nawigacji rolniczej **AgOpenGPS**. Dzięki akceleracji sprzętowej **DirectX 11** terminal zapewnia płynne odświeżanie maski obiektów w 60 FPS, dotykowy interfejs kabinowy oraz bezstratne skalowanie wektorowe.

---

## 🏛 Architektura Systemu

```text
 ┌────────────────────────────────────────────────────────┐
 │                      AG-ISOBUS PRO                     │
 │      DirectX 11 & Dear ImGui Engine (D. Horczak)       │
 └──────────────────────────┬─────────────────────────────┘
                            │
       ┌────────────────────┴────────────────────┐
       ▼                                         ▼
┌───────────────────────────────┐     ┌───────────────────────────────┐
│     ISO 11783-6 VT Server     │     │    ISO 11783-10 Task Control  │
│  - Object Pool Fast Boot (2ms)│     │  - Section Control (do 64 sek)│
│  - Multi-Working Set & Mini-VW│     │  - TECU GPS Speed Injection   │
│  - Acoustic Alarm Beeper      │     │  - Variable Rate (TC-GEO)     │
│  - AUX-N Joystick Mapper      │     │  - UDP Bridge (Port 8888/9999)│
└──────────────┬────────────────┘     └──────────────┬────────────────┘
               │                                     │
               ▼                                     ▼
      ┌─────────────────┐                   ┌─────────────────┐
      │  Magistrala CAN │                   │    AgOpenGPS    │
      │  (PCAN / ESP32) │                   │  (Nawigacja GPS)│
      └─────────────────┘                   └─────────────────┘
```

---

## 🌾 Główne Funkcjonalności

### 1. ISO 11783-6 Virtual Terminal Server (VT/UT)
- **Kompletny parser i renderer obiektów ISOBUS:** Kontenery, przyciski aktywne, pola wejściowe/wyjściowe tekstowe i liczbowe, wskaźniki wskazówkowe (Meters), paski postępu, wielokąty, łuki, elipsy, bitmapy i palety kolorów ISO.
- **Fast-Boot IOP Cache:** Automatyczny zrzut i ładowanie puli obiektów (`.iop`) z dysku. Kolejne uruchomienie podłączonej maszyny następuje w ułamku sekundy bez konieczności retransmisji CAN ETP.
- **Wielojęzyczność:** Zgodność ze standardem komend językowych ISO 11783-7.
- **Wirtualne Klawiatury:** Wbudowane klawiatury numeryczne i alfanumeryczne dostosowane do ekranów dotykowych w ciągnikach.

### 2. Kontroler Zadań AgOpenGPS (TC-SC & TC-BAS)
- **Automatyczna Kontrola Sekcji (Section Control):** Obsługa do 64 sekcji roboczych. Wyłączanie poszczególnych sekcji na nakładkach oraz granicach pola wyznaczonych w AgOpenGPS.
- **Wirtualne TECU (Tractor ECU):** Przesyłanie rzeczywistej prędkości ciągnika z anteny GPS (PGN 0xFE48 / 0xFE49) bezpośrednio na magistralę ISOBUS do dawkowania nawozu lub nasion.
- **Płynna synchronizacja UDP:** Zero opóźnień komunikacji między AgIO a terminalem.

### 3. Obsługa Wielu Maszyn (Multi-Working Set & Mini-Views)
- Równoczesna praca wielu sterowników implementów podłączonych do jednego ciągnika (np. przedni zbiornik nawozowy + tylny siewnik precyzyjny).
- Pasek szybkiego wyboru maszyny w nagłówku.
- **Mini-Views (Picture-in-Picture):** Pływające kafelki podglądu dające podgląd na żywo na stan maszyn działających w tle z możliwością natychmiastowego przełączenia jednym kliknięciem.

### 4. Dźwiękowy System Alarmów (ISO 11783-6 Audio Beeper)
- Dedykowany, bezblokujący wątek akustyczny.
- Dźwięki kliknięcia dotyku (TouchClick 2400 Hz) oraz klawiszy bocznych (SoftKeyClick 2100 Hz).
- Akustyczne potrójne impulsy alarmowe ISO (980 Hz) ponawiane cyklicznie co 1.5 sekundy aż do potwierdzenia przyciskiem **ACK**.

### 5. Pływająca Nakładka Dokowalna (HUD Overlay F11)
- Klawisz **F11** przełącza terminal w tryb przezroczystej nakładki typu HUD (`WS_EX_TOPMOST` + `WS_EX_LAYERED`).
- Pasek ISOBUS dokuje się bezpośrednio do prawej lub lewej krawędzi okna AgOpenGPS z regulowaną przezroczystością, umożliwiając jednoczesną obserwację linii prowadzenia i parametrów maszyny.

### 6. Wejścia AUX-N i Joysticki
- Zgodność z ISO 11783-9 (Auxiliary Inputs New).
- Mapowanie fizycznych przycisków i osi dżojstików USB / CAN na funkcje maszyn ISOBUS.

---

## 📦 Pobieranie i Gotowe Wydanie (Release v1.0.0)

Gotowe, skompilowane wydanie dla systemu Windows (64-bit) można pobrać bezpośrednio z zakładki [**Releases**](../../releases):

| Plik | Platforma | Zawartość |
| :--- | :--- | :--- |
| `AG-ISOBUS-PRO-v1.0.0-Win64.zip` | Windows 10/11 x64 | Gotowy program `isobus_vt_terminal.exe`, skrypt startowy `run_terminal.bat`, instrukcja |

Program jest całkowicie przenośny (portable) – nie wymaga instalatora ani zewnętrznych środowisk uruchomieniowych.

---

## 🛠️ Budowanie ze Źródeł (CMake)

### Wymagania:
- System: **Windows 10 / 11** (x64)
- Środowisko: **Visual Studio 2019 / 2022** (MSVC C++17)
- Narzędzie: **CMake 3.16+**
- Karta graficzna z obsługą **DirectX 11**

### Instrukcja kompilacji:
```powershell
# 1. Klonowanie repozytorium
git clone https://github.com/horczakdamian24-sketch/ag-isobus-terminal.git
cd ag-isobus-terminal

# 2. Generowanie plików projektu (CMake automatycznie pobierze wymagane biblioteki)
cmake -B build -DCMAKE_BUILD_TYPE=Release

# 3. Kompilacja wersji Release
cmake --build build --config Release
```

Skompilowany plik binarny zostanie utworzony w katalogu:  
`build/isobus_vt_terminal/Release/isobus_vt_terminal.exe`

---

## 🚜 Konfiguracja z AgOpenGPS

1. Uruchom aplikację **AgOpenGPS** oraz moduł komunikacyjny **AgIO**.
2. W AgOpenGPS upewnij się, że włączona jest automatyczna kontrola sekcji (ikona opryskiwacza/sekcji na zielono).
3. Uruchom terminal poleceniem:
   ```cmd
   run_terminal.bat
   ```
4. Terminal automatycznie połączy się z AgIO po protokole UDP (port nasłuchu `8888`, port nadawania `9999`).
5. Naciśnij **F11**, aby zadokować terminal jako przezroczystą nakładkę HUD obok mapy prowadzenia pola.

---

## ⌨️ Klawiszologia i Skróty

| Klawisz | Akcja |
| :--- | :--- |
| **`F11`** | Przełącz tryb dokowanej nakładki HUD / Studio |
| **`F2`** | Otwórz okno Ustawień i Konfiguracji Terminala |
| **`F1` – `F6`** | Klawisze programowe SoftKeys prawej kolumny |
| **`F7` – `F12`**| Klawisze programowe SoftKeys lewej kolumny |
| **`ESC`** | Zamknij aktywne okno modalne / reset widoku |
| **`Enter`** | Potwierdzenie wprowadzonej wartości numerycznej |

---

## 🔌 Wspierany Sprzęt CAN

- **PEAK-System:** PCAN-USB, PCAN-USB Pro, PCAN-PCIe.
- **ESP32 / Arduino:** Dowolny mikrokontroler z transceiverem SN65HVD230 / MCP2515 wysyłający ramki CAN 250 kbps (J1939 / ISO 11783).
- **Tryb Virtual CAN:** Wbudowany symulator magistrali w pamięci RAM do testów bez konieczności podłączania fizycznego sprzętu.

---

## 📄 Licencja & Informacje Prawne

Projekt udostępniany na licencji **MIT**. Zgodny z normami międzynarodowymi:
- **ISO 11783-6:** Ciągniki i maszyny rolnicze i leśne – Część 6: Wirtualny Terminal.
- **ISO 11783-10:** Część 10: Kontroler Zadań i systemy wymiany danych.

Autor projektu: **Damian Horczak**  
Precision Agriculture & ISOBUS Architecture
