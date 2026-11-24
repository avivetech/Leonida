# RPF9 TOC — Cadena Ghidra completa (contribución GH0ST, 2026-11-23)

Fuente: análisis estático de `game_scarlett_master.exe` (build 1.0.2.1,
sha256 e95020b2fa4a721db3df59fdecf75621730ac559d8835252f6c3b658d5a08993) con Ghidra 12.2
headless. 13 passes, ~35 funciones decompiladas. Todas las addresses verificables
en nuestro proyecto (ver colofón). Target: colaboradores de G6Recomp/Leonida.

## TL;DR para el runtime

1. El TOC NO está cifrado con AES-128 estándar: el "cipher id 0" del exe es un
   NO-OP (todas las slots de su vtable son stubs de 3 bytes). Los 208 GiB de
   .rpf NO necesitan ninguna clave.
2. La ruta completa del exe al TOC está mapeada (abajo) — es lo que el runtime
   debe reimplementar para navegar los archivos SIN carve.

## Cadena de apertura (todas FUN_14xxxxx,ImageBase 0x140000000)

```
FUN_140fc2540  openRPF(obj, path, mode, ...):
  FUN_140a57f70(obj)        ← clave = copy del FILENAME (strlen @obj+0x78)
  FUN_141fb2600(...)        ← lee TOC: valida magic 0x52504639; TOC-main [0,hdr_len);
                              si (mode==1) añade los 32B finales (tabla2, stride 9/10)
  FUN_141fb2780 → FUN_14087aec0(...)   ← modo: (byte@hdr+8)&1: 0=decrypt,1=index-only
  FUN_1408ec200→FUN_1408ec210(obj, flags)  ← orquestador del descifrado (cond. bit0)
  FUN_141fb2300→FUN_144e6f740(obj,1)       ← accessor del tag-table (u16@hdr+0x1A = #tags)
  FUN_1406bd050→FUN_1406bd090(...)         ← elige región: FUN_1406bd100 (toc_end-
                                             subtable; 16B) / FUN_1406bd110 ([f(gen)
                                             +0x40+0x150, hdr_len))
  FUN_1408ec360: FUN_140a34970(dst,hdr+4,hdr+8,u64@obj+0x70) = registro (ptr, delta)
  FUN_1406bd160(ssl,handle):  ← EL CICLO: por cada bloque 16B:
      FUN_140613720(**(*vtable+0x18))(update) y FUN_140613740(**(*vtable+0x20))(final)
  FUN_140536a70(ssl,hdr+2) ← cipher-id = FUN_140536ac0(u16@hdr+2)  [tabla @1462fe860]
  FUN_140613880(ssl,1,id,MAGIC,hdr+6,16) ← FÁBRICA:
      FUN_140193f90(MAGIC,id,1) = key-lookup (hash→key)
      FUN_144c11250(&cipher,id) = dispatcher; id 1/2/3→FUN_145e90390(1/2/3):
          416B obj, init FUN_141b43cb5, AES-NI (FUN_14215f06c setkey 128/192/256;
          FUN_14216127c AESKEYGENASSIST; 10/12/14 rounds @+0xF0)
      id 0 → 64B obj, vtable 1466fdb70: TODOS LOS SLOTS = stubs (MOV AL,1 / XOR EAX,EAX)
```

## Por qué "TOC cifrado" es mito (para 1.0.2.1)

- FUN_140536ac0(u16) → 0 si u16 ≥ 0x13. En TODOS nuestros .rpf, u16@hdr+2 = 0x5046
  ("FP" del magic, LE) → SIEMPRE 0 → siempre no-op.
- La alta entropía (~7.99) del TOC no es cifrado: son (u64-id, u32, u32) records,
  y los u64 son ids criptográficos (no-xor-cifrables: probamos fnv1a64/joaat64/
  xxh64(0/1) × 36,952 nombres = 0 hits; también AES-128 ECB/CBC/CTR/CFB/OFB ×
  5 keys × 6 IVs × 5 subranges = 0 plausibles).
- Los primeros 32B @0x40 = CONSTANTES en todos los .rpf:
  5f08daabecbdd40258c3516807024c24fb9f756755ef4be458785bfed8aa6989
  (=KNOWN_TOC_PREFIX_HEX del parser público: tags, no ciphertext).

## Layout del TOC (FUN_14087aff0 = CALCULADORA, counts cruzados)

```
0x040  4×8B    tags (u32,u32)×4                        ← count = u16@0x1A
0x060  21×16B  (u64-id, u32-a, u32-b) = 336B           ← = u32@0x1C (0x150) ✓
0x1B0  3×24B   (u64,u64,u64)×3                         ← count = u16@0x20
0x200  2×32B   (u64×4)×2                               ← count = u32@0x24
0x240  2×9B    (u32,u32,u8)×2 + pad                    ← count = u32@0x24; stride 8+byte@0x2C
```
(script.rpf de muestra: 4+21+3+2+2 = 32 records; todos los counts auto-consistentes).

## Qué NO resuelve esto

Los u64-ids del TOC no mapean 1:1 a paths públicos: los nombres están IN-STREAM
(vía rpf9-research: 215,028/2,235,084 streams carry a name; 168,660 paths únicos).
El TOC = árbol de seek+Merkle-ids; la navecación por nombres requiere correr el
propio juego (o nuestro runtime) — por eso esta cadena importa.

## Reproducibilidad

- Proyecto Ghidra persistente (280MB, 13 hunts): `../ghidra_projects/gta6_gh0st`
- Pseudocódigos: `../ghidra_analysis_out/` (68 archivos; hunts 1-13 + tests .py)
- Exe: sha256 e95020b2fa4a721db3df59fdecf75621730ac559d8835252f6c3b658d5a08993
- Bitácora completa: `../BITACORA.md` (comandos + salidas reales, 350+ líneas)

*GH0ST, 2026-11-23. Verificado contra: notre copia (223,880,932,870B, bit-exact),
el corpus 4,330-header (SentinelInteractive/0x52504639), rpf9-research, y
G6Recomp/known_builds.toml (exe_sha256 COINCIDE).*
