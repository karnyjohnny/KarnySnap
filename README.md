# 📸 KarnySnap

**Ultralekki rezydent Print Screen z trybem AOI (Area of Interest), zbudowany pod retro-sprzet (Windows 7 SP1, Core 2 Duo) — a dzialajacy tez na Windows 10/11.**

KarnySnap przejmuje klawisz `Print Screen`, siedzi cicho w zasobniku systemowym i pozwala Ci albo zlapac caly ekran, albo zaznaczyc dowolny fragment — bez okna glownego, bez GUI, bez zbednego zuzycia RAM.

![platform](https://img.shields.io/badge/platform-Windows%207%20SP1%20%7C%208.1%20%7C%2010%20%7C%2011-0078D6)
![arch](https://img.shields.io/badge/arch-x86%20%7C%20x64-informational)
![license](https://img.shields.io/badge/license-MIT-green)
![footprint](https://img.shields.io/badge/RAM-~1.5%20MB-success)

---

## ✨ Co potrafi

- **Globalny Print Screen** — przechwytuje klawisz systemowo (low-level keyboard hook) i blokuje jego domyslne dzialanie.
- **Tryb AOI (domyslny)** — ekran przyciemnia sie, a obszar, ktory zaznaczasz myszka, pozostaje w pelnej jasnosci, wiec dokladnie widzisz co zostanie wyciete.
- **Tryb pelnego ekranu** — jeden klawisz, natychmiastowy zrzut calego pulpitu (multi-monitor / wirtualny ekran).
- **ESC anuluje** — w trakcie zaznaczania AOI, `ESC` (lub ponowny `Print Screen`) wychodzi z trybu zaznaczania bez zapisu.
- **Trzy formaty zapisu** — PNG (domyslnie), JPG, BMP — wybierane z menu zasobnika.
- **Zapis + schowek jednoczesnie** — kazdy zrzut trafia do pliku i od razu jest dostepny w schowku (Ctrl+V).
- **Folder docelowy do wyboru** — domyslnie `Pulpit\Screenshots`, zmienialny z menu (`Zmien folder...`).
- **Bez GUI, bez konsoli, jedna ikona w trayu** — minimalny narzut pamieci i CPU (ok. 1.5 MB RAM w spoczynku).
- **Ustawienia trwale** — zapisywane w `%LOCALAPPDATA%\KarnySnap\config.ini`.

## 🖥️ Wymagania

- Windows 7 SP1 lub nowszy (7 / 8.1 / 10 / 11), **32-bit lub 64-bit**.
- Brak dodatkowych zaleznosci runtime — program korzysta wylacznie z bibliotek systemowych Windows (GDI, GDI+, Shell32).

## 📥 Instalacja

1. Pobierz gotowy plik z zakladki [Releases](../../releases) — wybierz `KarnySnap_x86.exe` (32-bit) lub `KarnySnap_x64.exe` (64-bit), zaleznie od systemu.
2. Uruchom plik. Program doda ikone w zasobniku (obok zegara) i od razu zacznie nasluchiwac `Print Screen`.
3. (Opcjonalnie) Dodaj skrot do folderu autostartu Windows (`Win+R` -> `shell:startup`), jesli chcesz, aby KarnySnap uruchamial sie razem z systemem.

Prawym przyciskiem na ikonie w trayu masz dostep do: trybu (AOI / caly ekran), formatu obrazu (PNG/JPG/BMP), folderu zapisu i wyjscia z programu.

## 🛠️ Budowanie ze zrodel

KarnySnap jest napisany w czystym C (**standard C23**) i celowo trzyma sie z dala od C++/ciezkich frameworkow, zeby dzialac plynnie na starszym sprzecie (Intel Core 2 Duo, 2 GB RAM, Intel GMA 4500MHD).

### Wymagany kompilator

- **GCC 13+** ze wsparciem `-std=c23` (referencyjnie: GCC 16.2.0, MinGW-w64).
- Docelowy sprzet referencyjny: Dell Latitude E5500 (Core 2 Duo T7250 / architektura Merom).

### Kompilacja lokalna (Windows, skrypt .bat)

```bat
cd build
build.bat
```

Skrypt uzywa dokladnie tych flag, ktore wyciagaja maksimum z rdzenia Core 2:

```bat
gcc -std=c23 -O2 -march=core2 -msse3 -mfpmath=sse -mwindows ^
    ..\src\karnysnap.c -o KarnySnap.exe ^
    -lgdiplus -lshell32 -lole32 -lcomdlg32
```

Aby zbudowac konkretna architekture (jesli masz zainstalowane oba toolchainy MinGW-w64):

```bat
build.bat x86    REM uzywa i686-w64-mingw32-gcc
build.bat x64    REM uzywa x86_64-w64-mingw32-gcc
```

### Kompilacja automatyczna (GitHub Actions)

Repozytorium zawiera workflow [`.github/workflows/build.yml`](.github/workflows/build.yml), ktory na kazdy push/PR do `main`:

1. Stawia srodowisko MSYS2/MinGW-w64 (osobno dla `MINGW32` i `MINGW64`),
2. Kompiluje `KarnySnap_x86.exe` oraz `KarnySnap_x64.exe` tymi samymi flagami co lokalny `build.bat`,
3. Publikuje oba pliki jako artefakty builda,
4. Przy wypchnieciu tagu `vX.Y.Z` — automatycznie tworzy Release z gotowymi binarkami.

## 📂 Struktura repozytorium

```
KarnySnap/
├── src/
│   └── karnysnap.c          # caly program (jeden plik, bez zaleznosci zewnetrznych)
├── build/
│   └── build.bat            # lokalny skrypt kompilacji pod Windows
├── .github/workflows/
│   └── build.yml            # CI: build x86 + x64, automatyczne Release
├── LICENSE                  # MIT
└── README.md
```

## ⚙️ Jak to dziala (w skrocie)

- `WH_KEYBOARD_LL` — globalny hook klawiatury przechwytuje `VK_SNAPSHOT` (Print Screen) i `VK_ESCAPE` (tylko gdy trwa zaznaczanie AOI).
- Zrzut ekranu robiony jest przez `BitBlt` do `CreateDIBSection` (32bpp, bottom-up DIB) — bez posrednich konwersji.
- W trybie AOI tworzona jest dodatkowa, przyciemniona kopia bufora (przemnożenie kanalow RGB), rysowana jako tlo nakladki; zaznaczony prostokat renderowany jest z oryginalnego, jasnego bufora.
- Zapis do PNG/JPG odbywa sie przez GDI+ (`Gdip*`, dolaczane bezposrednio jako plaskie funkcje `__stdcall`, bez naglowkow C++), BMP zapisywany jest recznie (najlzejsza sciezka, bez GDI+).
- Brak okna glownego widocznego dla uzytkownika — jedyne UI to ikona `Shell_NotifyIcon` w trayu i jej menu kontekstowe.

## 🤝 Wklad

Pull requesty i issues mile widziane — szczegolnie te dotyczace zgodnosci wstecznej z Windows 7/8.1 oraz optymalizacji pod starsze CPU.

## 📄 Licencja

Projekt udostepniony na licencji [MIT](LICENSE) — rob z nim, co chcesz, z zachowaniem informacji o autorze.

---

Zbudowane z uporem na laptopie, ktory pamieta Windows 7 w wersji beta. 🛠️
