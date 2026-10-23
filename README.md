# NFS Carbon PC — Android Evolved

Static recompilation of the **PC version** of *Need for Speed: Carbon* (2006, `NFSC.exe` 1.4, x86) to native Android ARM64 and ARMv7. Fourth game of the *NFS Android Evolved* family, after [Underground 2](https://github.com/codepdbh/nfsu2-recompiled), [Underground 1](https://github.com/codepdbh/nfsu1-recompiled) and [Most Wanted](https://github.com/codepdbh/nfsmwpc-recompiled), whose runtime this port starts from.

It is **not** an emulator: every function of the original PC executable is translated ahead of time from x86 to C with [pcrecomp](https://github.com/sp00nznet/pcrecomp) (modified) and compiled with the Android NDK.

> **Status: playable (v0.1.0).** Career, quick races and NFSCO online run on Android (tested on a Galaxy S25 and a Redmi Note 8). APKs: [Releases](../../releases).

No game files are included. You need your own copy of the PC game. Generated C code, analysis copies and logs are produced locally and never committed.

### What is translated

| Code | Origin | Guest base |
|---|---|---|
| `NFSC.exe` 1.4 (SHA-256 `baaaaec3…2fd3`) | your copy | `0x00400000` |
| `d3dx9_30.dll` | your local DirectX runtime (`Windows/SysWOW64`) | `0x00D00000` |
| `msvcr71.dll`, `msvcp71.dll` | your game folder (Carbon links the VC++ 7.1 runtime dynamically) | `0x00F60000`, `0x00FC0000` |
| `server.dll` (LAN server) | your game folder | `0x01270000` |
| x86 routines inside `SOUND/**/*.abk` | your game folder | matched by content |

### Online: NFSCO

The port includes the community online mod **NFSCO** ([nfsco.club](https://nfsco.club)). Its authors allowed translating their binaries for this Android port; their source code is not shared and is not needed. `tools/nfsco_capture.c` records, on Windows, the patches NFSCO's `palmont.dll` applies to the game at fixed guest addresses (palmont at `0x01060000`, its trampolines at `0x01200000`); see [docs/BINARY_REPORT.md](docs/BINARY_REPORT.md).

### Game language

Carbon picks its language from the registry value `Language`. The launcher writes it: Spanish (Spain), Spanish (Mexico), English and the other languages whose `LANGUAGES/<language>_Global.bin` is in your copy.

### Building

Requirements: Python 3 with `capstone` and `pefile`, the patched `pcrecomp` and `nfsu2-recompiled` (prepared DXVK tree, libadrenotools) checked out next to this folder, Android SDK/NDK, and the game in `../Need for Speed Carbon`.

```powershell
python scripts/analyze_binary.py            # work/speed.analysis.exe, work/binary.json
python scripts/run_lift.py catalog work/speed.analysis.exe work/seeds_code.json work/catalog.json
python scripts/run_lift.py lift work/speed.analysis.exe work/catalog.json src/recomp/gen
python scripts/prepare_modules.py           # d3dx9_30, msvcr71, msvcp71, server.dll
python scripts/abk_module.py "../Need for Speed Carbon" work/abk_analysis.dll work/abk_seeds.json src/recomp/gen_abk/abk_blocks.c
python scripts/run_lift.py catalog work/abk_analysis.dll work/abk_seeds.json work/abk_catalog.json --seeds-only
python scripts/run_lift.py lift work/abk_analysis.dll work/abk_catalog.json src/recomp/gen_abk --prefix abk_
cd android; ./gradlew assembleDebug
```

On the phone, copy the game folder (with `NFSC.exe`, `msvcr71.dll`, `msvcp71.dll`, `server.dll`) to `Internal storage/nfsc`, plus `d3dx9_30.dll` from your Windows `SysWOW64`.

### License and credits

Code in this repository: see each file. pcrecomp by sp00nznet. NFSCO by its authors (nfsco.club), translated with their permission. *Need for Speed* is a trademark of Electronic Arts; this project is not affiliated with EA.

---

## Español

Recompilación estática de la **versión de PC** de *Need for Speed: Carbon* (2006, `NFSC.exe` 1.4, x86) a Android ARM64 y ARMv7. Cuarto juego de la familia *NFS Android Evolved*, después de Underground 2, Underground 1 y Most Wanted, de cuyo runtime parte.

> **Estado: jugable (v0.1.0).** Modo historia, carreras rápidas y el online de NFSCO funcionan en Android (probado en un Galaxy S25 y un Redmi Note 8). APK en [Releases](../../releases).

No se incluyen archivos del juego: necesitas tu propia copia de PC. El código C generado, las copias de análisis y los registros se producen localmente y nunca se suben.

**Online:** incluye el mod comunitario **NFSCO** ([nfsco.club](https://nfsco.club)), cuyos autores han autorizado traducir sus binarios para este port.

**Idioma:** el launcher escribe el valor de registro `Language` que lee Carbon. Ofrece Español (España), Español (México), Inglés y los demás idiomas presentes en tu copia.
