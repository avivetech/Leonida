# IMPORTS-MAP: la superficie completa del exe (Track B-F1) — GH0ST, 2026-11-23

Fuente: análisis estático del import-table (DataDir[1], RVA 0x6791AB4, 69 descriptores)
de game_scarlett_master.exe 1.0.2.1 (sha256 e95020b2...08993).
Herramienta: imports_dump3.py (parse-PE stdlib, sin dependencias).
Datos completos: imports_map.tsv (587 funciones × 68 DLLs, mismo directorio).

## Números clave
- 587 import-pares, 68 DLLs, 69 descriptores (1 = terminador... 68 reales + trailer)
- IAT (DataDir[12] @RVA 0x6793490, 655 slots) = losbound-entries correspondientes
- El import-table ESTÁTICO es la superficie COMPLETA: no hay加载 dinámica oculta
  (GetProcAdresses residuales aparte; los 656 hints = exactamente estos 587+nulos)

## Lo que Track B debe implementar (shim nativo), por familia:
- CRT/STL:      MSVCP140 113, VCRUNTIME140 23 (+1 _1), api-ms-win-crt-* 166
                → trivial: biblioteca-glibc hace el 90%; la mezcla = msvcxx-abi
- Win32 core:   api-ms-win-core-* 121 (synch 17+1+2, file 22+1, processthreads 15,
                memory 3, heap 1,COM 7...) →.casimiro: wine-headers o-escrito a mano
- Red:          WS2_32 25, WINHTTP 15, libHttpClient.GDK 38
                → 78: la MAYOR superficie NO-CRT. libHttpClient=HTTP/2+JSON+XSTS
- Multimedia:   MFPlat 6 + MFReadWrite 1 + Bink2Scarlett 23 + XCRDAPI 1
                → Bink2: YA LO TENEMOS (librempeg decodifica 12/12; mapeo 1:1)
- GPU:          d3d12_xs SOLO 3 (D3D12XboxCreateDevice + 2), xg_xs 2, AcpHal 9,
                amd_fidelityfx_loader_xbox 4
                → Toda la capa-D3D12 va POR-IMPORTS-FEED (vtables), no por import-table:
                  la shim-193-slots de G6Recomp es el trabajo real; aquí SOLO 3 símbolos
- Sistema:      GameInput 1 (GameInputCreate), XboxBvhBuilder 7, xmem 12 (decomp),
                xapu 5 (audio), chad 5 (¿Console-Audio-Device?), psm-appnotify 4,
                ADVAPI32 3, XCurl (delay, en DataDir[13]... verificar)

## Qué gana G6Recomp
Su docs/game-package.md pinea 16 side-DLLs (los que VIAJAN con el juego). Este mapa es
el OTRO lado: las 68 que el EXE-ESPERA del sistema. Ellos no lo tenían publicado:
ahora pueden priorizar el shim por frecuencia (MSVCP140 113 primero, luego el 78-red).

## Método (reproducible)
python3 imports_dump3.py  # offsets: DataDir[1]→69 descs; OFT u64-thunks;
                          # hint-name @VA+2, ventana 200B (nombres C++ 113+ chars)
Trampas: (1) Bulk-read obligatorio (8B-iter = timeout); (2) ventana-64B corta los
mangles-MSVCP; (3) va2fo debe chequear rsize (BSS sin backing→None).
