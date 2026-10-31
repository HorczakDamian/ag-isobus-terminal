# ISOBUS Universal Terminal (VT Server) z obsługą AUX-N (ImGui + PEAK PCAN)

Kompletna implementacja serwera wirtualnego terminala (Virtual Terminal / Universal Terminal wg ISO 11783-6) ze zintegrowaną obsługą funkcji pomocniczych (**AUX-N / AUX-O**) oraz sterownikiem sprzętowym dla konwerterów **PEAK PCAN (PCAN-USB)**.

---

## 1. Architektura i kluczowe moduły

1. **Stos protokołów (`AgIsoStack++`)**:
   - Pełna obsługa warstwy sieciowej ISOBUS (ISO 11783-5 Address Claiming, ISO 11783-3 Transport Layer TP i ETP).
   - Odbiór i parsowanie binarnej puli obiektów (`.iop`) wg ISO 11783-6.
   - Obsługa protokołów diagnostycznych i cyklicznych ramek stanu.

2. **Warstwa sprzętowa CAN (`CanManager`)**:
   - Bezpośrednia integracja z konwerterem **PEAK PCAN** przez bibliotekę `PCANBasic` (kanały `PCAN_USBBUS1` do `PCAN_USBBUS8`).
   - Szybkość transmisji: **250 kbit/s** (standard magistrali ISOBUS).
   - Tryb **Virtual Loopback CAN** – umożliwia testowanie terminala bez fizycznego adaptera.
   - Wbudowany monitor magistrali CAN (podgląd PGN, adresów źródłowych, danych i częstotliwości ramek w czasie rzeczywistym).

3. **Serwer terminala wirtualnego (`CustomVTServer`)**:
   - Dziedziczy po `isobus::VirtualTerminalServer`.
   - Adres preferowany: `0x26` (38 – standardowy adres VT #1 w ciągniku).
   - Obszar roboczy Data Mask: domyślnie **800 x 600 px** (skalowalny).
   - Pasek boczny Soft Keys: 6 fizycznych/wirtualnych klawiszy funkcyjnych (F1–F6).
   - Obsługa zdarzeń dotykowych i kliknięć: przyciski, klawisze funkcyjne, wprowadzanie liczb (Keypad modalny), edycja tekstu.

4. **Moduł AUX-N (`AuxManager`)**:
   - **Wykrywanie urządzeń wejściowych**: nasłuchuje cyklicznych komunikatów `AuxiliaryInputTypeTwoMaintenanceMessage` (0x23) od joysticków i konsol sterowniczych na magistrali.
   - **Wykrywanie funkcji maszyny**: automatycznie przeszukuje aktywny Object Pool w poszukiwaniu obiektów `AuxiliaryFunctionType2` (typ 31).
   - **Mapowanie (Assignment GUI)**: interfejs pozwalający operatorowi powiązać daną funkcję maszyny (np. "Podnieś belkę") z wejściem fizycznego lub wirtualnego joysticka (przycisk, oś analogowa).
   - **Wysyłanie rozkazów przypisania**: generuje ramki `AuxiliaryAssignmentTypeTwoCommand` (0x24) przesyłane bezpośrednio do ECU maszyny.
   - **Handshake preferowanych przypisań**: odpowiada twierdząco (ACK) na zapytania maszyn `PreferredAssignmentCommand` (0x22).
   - **Wirtualny joystick (Simulator)**: wbudowany symulator joysticka ISOBUS (8 przycisków + 2 osie proporcjonalne), generujący ramki `AuxiliaryInputTypeTwoStatusMessage` (0x26) do natychmiastowych testów.

5. **Silnik renderowania (`VTRenderer` + Dear ImGui + DirectX 11)**:
   - Rysuje Data Masks, Soft Key Masks, kontenery, przyciski, wykresy słupkowe, teksty i wartości liczbowe przy użyciu `ImDrawList`.
   - Paleta 16/256 kolorów ISO 11783 przeliczana na przestrzeń barwną RGBA.

---

## 2. Uruchomienie

Aplikację można uruchomić natychmiast:
- Uruchomienie pliku: `run_terminal.bat` (w katalogu głównym)
- Lub bezpośrednio: `build_vt\Release\isobus_vt_terminal.exe`

---

## 3. Kompilacja ze źródeł (opcjonalnie)

Wymagania: CMake 3.16+, Visual Studio 2022/2026 (MSVC x64), sterowniki PEAK PCAN.

```powershell
cmake -S isobus_vt_terminal -B build_vt -G "Visual Studio 17 2022" -A x64
cmake --build build_vt --target isobus_vt_terminal --config Release
```
