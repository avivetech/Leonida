# D3D12X stop-13: el bloqueo #1 del boot — RESUELTO por análisis estático (GH0ST, 2026-11-23)

Fuente: Ghidra 12.2 headless sobre game_scarlett_master.exe 1.0.2.1
(sha256 e95020b2...08993), hunts 14-17. Proyecto: gta6_gh0st.

## El síntoma (Leonida, runtime 180s)
Tras el primer CreateDescriptorHeap (slot 73): assert/int3.
"the game expects the CPU handle of the heap to equal a value it already holds".

## La cadena (direcciones absolutas, ImageBase 0x140000000)

FUN_142478440 (4661B) = WINMAIN del juego:
  - RegisterClassExA + CreateWindow (wndproc DAT_14247a660)
  - D3D12XboxCreateDevice(0, desc, &DAT_1461e0638, &DAT_1475df048)  [device @1475df048]
  - loop: 5 pares de heaps (i=0..4; array @DAT_1475e72a0 = handles, @1475e7300 = ptrs;
    slots 5..10 del array = reservados, NUNCA creados)
    * slot 0x48/9   = (*device+0x48)(device,0,&TPL_1461e0658,&out)  → "placeholder/handle" (64B
      template: [16B GUID {6102dee4-af59-4b09-b999-b44d73f09b24}]...[ptr .text @1461e06A0=0x1424752a0]
      ...[ptr @1461e06A8=0x1455accc0])
    * slot 0x248/73 = (*device+0x248)(device,&DESC,handle,0,&IID_1461e0668,&out)
      DESC = {cbSize=0(@14615c190), style=2(@14615c194), @(14615c198)=wndproc-unk, 1, 0}
      → 73 = CreateDescriptorHeap (CONFIRMA el número de Leonida)
    * slot 0x420/132 = (heap, 0x700000,0,0,0xFFFFFFFFFF00) → RESERVA 7 MiB (heap0)
                       (heaps 1-4: 0x100000 = 1 MiB c/u)
    * slot 0x418/131 = (heap, 0xC3C0, 0x80020000,0,0xFFFFFFFFFF00) → reserve/map #2
                       (heaps 1-4: 0xC380, 0x20000)
    * slot 0x48/9 del HEAP = GetCPUDescriptorHandleForHeapStart (control)

FUN_141cf3280 (300B, llamada vía thunk 145a54700) = WRAPPER con EL ASSERT:
  lVar5 = (*(*heap + 0x48))();                     // GetCPUDescriptorHandleForHeapStart REAL
  if (expected != 0 && lVar5 != expected) → int3;  // ← STOP 13. expected = LO QUE EL JUEGO ASIGNÓ
  *(obj+0x20) = lVar5;                             // cpuBase real
  *(obj+0x28) = expected + count*incr;             // cpuEnd
  *(obj+0x30) = (u32)incr;                         // STRIDE
  *(obj+0x38) = (flags&1) ? GetGPUHandle(+0x50) - GetCPUHandle(+0x48)   // DELTA GPU=CPU−CPU
                          : 0xBADADD0000000000;         // -0x4525230000000000 = SENTINEL GXDK

FUN_14156c240 (70B) = SUB-ALLOCATOR (bump, atómico) — EL "VALOR QUE YA TIENE":
  uVar4 = ALIGN(curs+count-1, align);  uVar1 = uVar4 + align * (*(*obj+0x30));
  if (uVar1 > *(obj+2)) → 0;  LOCK/CAS: curs=uVar1;  *out = uVar4;
  → el juego ASIGNA las direcciones ÉL MISMO (base+ordinal×stride) y EXIGE que el
    handle del driver COINCIDA. No confianza en el driver: FILTRO DE DIRECCIONES.

## Los 4 pools (registro, llamada FUN_145a54700 → FUN_141cf3280)
  (obj@1475e73c0, id=1, INCR=0x80 (128B!), flags=1 → GPU-delta REAL)
  (obj@1475e7408, id=1, INCR=0x80,         flags=0 → GPU-handle = 0xBADADD0000000000)
  (obj@1475e7450, id=2, INCR=0x1000 (4096B), flags=0)   // "large" (SAM/BVH-class)
  (obj@1475e7498, id=3, INCR=0x800 (2048B),  flags=0)
  → STRIDE 128B en Series (NO el 32B de PC). 7MiB/128 = 57344 descs (heap0); 1MiB/128 = 8192.

FUN_145c5ff00 (267B) = PLANIFICADOR de memoria previo:
  mask = DAT_14615c228 [0xffff,0,...] + _DAT_14a943918 (runtime, 2×i64: SEGMENT IDs)
  stride = DAT_14a943914 (default 0x20=32B);  slots = DAT_14a943910 (default 0x40000)
  total  = ALIGN(slots,0x10000) * stride = 0x800000 = 8 MiB
  → FUN_141e86440(...,total,segA,segB) reserva; en éxito:
    _DAT_1475df158=32, _DAT_1475df160=0x40000, DAT_1475df168=8MiB, DAT_1475df0e0=1
  → después FUN_145cabb00(DAT_1475df188, DAT_1475df190+DAT_1475df168) = LE PASA EL RANGO AL DEVICE.
  Cookie DAT_14615c1c0 = 02 02 01 00 (vpmovsxbd → [2,2,1,0]) = la firma 16B que llena
  el desc; DAT_14615c228 = 0x0000ffff (segmento "any").

## EL FIX (para Leonida/G6Recomp) — 4 reglas:
1. CreateDescriptorHeap (slot 73) + el par placeholder (slot 9): EL HEAP DEBE DEVOLVER,
   en GetCPUDescriptorHandleForHeapStart, EXACTAMENTE la VA que el juego calculó con su
   propio bump-allocator (FUN_14156c240) — i.e. adoptar el "placeholder-VA" pedido, no
   inventar una. En Wine/VKD3D:Requires CreatePlacedResource-style fixed VA or the
   shim must pre-map its internal heap base == expected.
2. STRIDE = 128B (0x80) para los pools 1; 0x1000/0x800 para los 2/3 — no PC sizes.
3. GetGPUDescriptorHandleForHeapStart − GetCPUDescriptorHandleForHeapStart debe ser
   consistente por-heap y cacheable: (delta = GPU−CPU) cuando flags&1; si flags&1==0
   el handle GPU = 0xBADADD0000000000 (comprobar: Ghidra muestra -0x4525230000000000
   = 0xBADADD0000000000, el sentinel clásico GXDK "BADADDress").
4. La RVA reserva del planificador: 8 MiB = 0x40000 slots × 32B en los segmentos
   (mask+delta runtime) — la shim debe HONESTAR esa reserva (o no-op con VA fija).

## Validación estática (todo desde el EXE, sin correr nada)
  - Datos .rdata/.data leídos por /tmp/cookie_extract.py (script 2.6K, stdlib):
    DAT_14615c1c0 = 0202010000000000...  DAT_14615c190 = 00 00 00 00 02 00 00 00 01...
    DAT_1461e0658 (template 64B, GUID+vtable-ptrs)  DAT_14615c228 = ffff0000...
    DAT_1461e0628 = IID 8efb471d-6161... (16B, usado en +0x70)
  - Hunto pseudo-C: /tmp/hunt16_result.txt (WinMain 4661B completo), /tmp/hunt17_result.txt
    (FUN_141cf3280 + FUN_14156c240 + FUN_145c5ff00 + FUN_1417f6a30).
  - Artifact: ghidra_analysis_out/hunt16_result.txt, hunt17_result.txt, cookie_extract.py.

## Qué GANA la comunidad
- El slot-73 + stride-128B + 0xBADADD0000000000 + placeholder-VA-adopt = el mecanismo
  EXACTO del stop 13. Nadie lo tenía. El primer cuadro ya no depende de adivinar: el
  heap shim = "devolver la VA que el juego pidió, con stride 128, y el sentinel".
