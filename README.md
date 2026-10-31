# AG-ISOBUS PRO — ISOBUS Universal Terminal & AgOpenGPS Task Controller

> **Precision Ag Operating System & Universal Terminal**  
> System Architecture & Precision Ag Engineering by **D. Horczak**

Komercyjnej klasy wirtualny terminal rolniczy **ISOBUS Universal Terminal (VT Server)** zgodny z normą **ISO 11783-6** wraz ze zintegrowanym kontrolerem zadań **Task Controller (TC Server)** zgodnym z **ISO 11783-10** oraz mostkiem sekcji do **AgOpenGPS**.

---

## 🌾 Główne Funkcjonalności

- **ISO 11783-6 Virtual Terminal Server (VT Server):**
  - Pełny silnik renderujący bazujący na **DirectX 11** i **Dear ImGui**.
  - Obsługa wszystkich obiektów puli ISOBUS: Kontenery, Przyciski, Pola Liczbowe i Tekstowe, Paski Postępu, Wskaźniki Wskazówkowe (Meters), Łuki, Wielokąty, Bitmapy/Grafiki i Palety Barw.
  - Szybki start (**Fast Boot Disk Cache**) – ładowanie puli obiektów maszyn bezpośrednio z pamięci podręcznej w ~2 ms.
  - Dynamiczne skalowanie widoku (Zoom 100%, 125%, 150%, Dopasuj do okna).
  - Dotykowa klawiatura numeryczna oraz alfanumeryczna dla ekranów kabinowych.

- **ISO 11783-10 Task Controller (TC-BAS & TC-SC) & AgOpenGPS Bridge:**
  - Dwukierunkowa komunikacja UDP (port 8888 nasłuch, port 9999 nadawanie) z ekosystemem **AgOpenGPS / AgIO**.
  - Automatyczna kontrola sekcji (**Section Control** do 64 sekcji na belce) – wyłączanie sekcji na nakładkach i poprzeczniakach.
  - Interfejs prędkości ciągnika **TECU** (PGN 0xFE48 / 0xFE49) przekazujący prędkość GPS do maszyn ISOBUS.

- **System Wielu Maszyn (Multi-Working Set & Mini-Views):**
  - Równoczesna obsługa wielu sterowników implementów (np. siewnik + opryskiwacz + przedni zbiornik).
  - Pasek wyboru aktywnej maszyny z sygnalizacją alarmów.
  - Okno **Mini-Views (Picture-in-Picture)** wyświetlające miniatury pulpitów maszyn pracujących w tle.

- **Dźwiękowy System Alarmów i Brzęczyka (ISO 11783-6 Audio Beeper):**
  - Dedykowany asynchroniczny wątek dźwiękowy (zero lagów na renderowaniu DirectX).
  - Akustyczne impulsy alarmowe (potrójny ton 980 Hz ponawiany co 1.5s aż do wciśnięcia ACK).
  - Dźwięki dotyku ekranu i klawiszy funkcyjnych Soft Keys.

- **Tryb Pływającej Nakładki Dokowalnej (Dockable HUD Overlay na AgOpenGPS):**
  - 1-kliknięciowe przełączanie trybu (`F11`).
  - Terminal unosi się stabilnie nad oknem AgOpenGPS (`WS_EX_TOPMOST`).
  - Regulowana przezroczystość (Acrylic Glass) oraz dokowanie do prawej/lewej krawędzi ekranu.

- **Obsługa Magistrali CAN:**
  - Natywna obsługa interfejsów **PEAK PCAN-USB**.
  - Tryb wirtualnej magistrali Loopback CAN do testów bez fizycznego adaptera.

---

## 🛠️ Wymagania i Budowanie Projektu

### Wymagania:
- System: **Windows 10 / 11** (x64)
- Kompilator: **MSVC (Visual Studio 2019 / 2022)** z obsługą C++17
- Narzędzie: **CMake 3.16+**
- Karta graficzna z obsługą **DirectX 11**

### Instrukcja kompilacji:
```powershell
# 1. Konfiguracja projektu
cmake -B build -DCMAKE_BUILD_TYPE=Release

# 2. Kompilacja
cmake --build build --config Release
```

Gotowy plik wykonywalny znajduje się w folderze `build/isobus_vt_terminal/Release/isobus_vt_terminal.exe`.

---

## 🚜 Uruchamianie z AgOpenGPS

1. Uruchom **AgOpenGPS** i włącz sterowanie sekcjami (Section Control).
2. Uruchom terminal poleceniem:
   ```cmd
   run_terminal.bat
   ```
3. Wciśnij **F11**, aby zadokować terminal jako przezroczystą nakładkę HUD obok mapy AgOpenGPS.

---

## 📄 Licencja & Autor

Projekt stworzony przez: **Damian Horczak**  
Kompatybilny ze standardami **ISO 11783-6** (Virtual Terminal) oraz **ISO 11783-10** (Task Controller).
