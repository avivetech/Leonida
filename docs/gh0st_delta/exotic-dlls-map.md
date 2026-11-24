# CARTA DE EQUIVALENCIAS — DLLs exóticas del exe → equivalente Linux (Track B) — GH0ST 2026-11-23

Fuente: imports_map.tsv (587×68) + disco + MicrosoftGame.Config + código de G6Recomp.
Método: los 23+12+7+... símbolos EXACTOS están en el TSV; aquí cada DLL → qué hace,
cuánto cuesta implementarla, y con qué.

## RESUMEN (por costo, de menor a mayor)

### 1-función (triviales: stub + detrás, la vtable)
- GameInput.dll: ordinal_1 = GameInputCreate (la ÚNICA). Shim: 5 líneas.
- XCRDAPI.dll: DStorageGetFactory (la ÚNICA). → G6Recomp YA la implementa
  (runtime/io/src/direct_storage.cpp:707 — sirve IDStorageFactoryX/FileX/QueueX/
  StatusArrayX). PERO: ellos registran "IDStorageFactoryX" sin DLL; el juego la
  BUSCA en XCRDAPI.dll (import estático). Fix: registrar/ exportar como XCRDAPI.

### 3-funciones (el 90% de la GPU es vtable, NO esto)
- d3d12_xs.dll: D3D12XboxCreateDevice + D3D12ConfigureVirtualMemoryX +
  D3D12SerializeVersionedRootSignature.
  → CONFIRMA el stop-13: todo el weight D3D12 = la vtable de 193 slots.

### 4-9 funciones (pequeñas, nativas-fáciles)
- amd_fidelityfx_loader_xbox.dll: ffxCreateContext/DestroyContext/Dispatch/Query
  → sí: AMD公开 ffx-SDK (GPL/MIT); 4 entry-points, el resto por contexts.
- xg_xs.dll: XGCreateTextureComputer + XGSuggestSwizzleMode → tiling/swizzle Xbox:
 VKD3D tiene D3D12X equivalente; o C++-puro (swizzle = bit-remap documentado).
- XboxBvhBuilder.dll: 7 = ctor/dtor/BuildRaytracingASBatched2X/CopyRTAS/
  RehydrateRTASBatched2X/Set64BitMortonCodes + rva_067971c8 (¡decodeado del raw:
  = GetRaytracingAccelerationStructurePrebuildInfo, mangle de 196+ chars —
  la ventana-200B lo cortaba: names>196 EXISTEN).
  → RT-AS builder: VKD3D construye AS (vkCreateAccelerationStructureKHR);
    Mapeo 1:1. DIFÍCIL-PERO-MAPEADO.
- MFPlat+MFReadWrite: 7 (MFStartup/Shutdown/GetSystemTime + 4 factories)
  → FilmTitan?? No: solo creation-de-attrs/mediatype/sourceresolver/reader —
    en Linux: GStreamer-o-ffmpeg-wrapper; O neutrales (los BK2 no-pasan-por-MF).

### 12-23 funciones (medianas, YA-CONOCIDAS)
- xmem.dll: 12 = XMemAlloc/Free(+Default)/AllocatePhysicalPages/
  MapPhysicalPages(Scatter)/TransferMemory/VirtualAlloc/GetWorkingSetStatistics/
  IsToolsMemAvailable
  → TODO-malloc+posix_memalign+mmap. 1:1. (NO es el descompresor — la equivalencia
    con BCPack era hipótesis mía: CORREGIDO: xmem=memoria, no compresión. La
    compresión XSA/cet NO ESTÁ en imports: ver 23-aside abajo).
- Bink2Scarlett.dll: 23 = BinkOpen/DoFrame(GAsyncMulti/AsyncWait)/NextFrame/Wait/
  Close/Goto/GetKeyFrame/GetFrameBuffersInfo/RegisterFrameBuffers/Pause/
  SetVolume/SetSoundSystem/SetMemory/SetOSFileCallbacks/SetWillLoop/ShouldSkip/
  StartAsyncThread/RequestStopAsyncThreadsMulti/WaitStopAsyncThreadsMulti/
  FreeGlobals/GetPlatformInfo/OpenWithOptions
  → → → ←→ librempeg (nuestro, 12/12 .bk2 decodificados). EQUIVALENCIA 1:1
    (cada Bink* → llamada-librempeg). La única-DE-VERDAD:Difícil:
    BinkDoFrameAsyncMulti (GPU-decode-hint) — en Linux: decodificación-CPU-atomic.
- AcpHal.dll: 9 = AcpHal{Create,Allocate/ReleaseShapeContexts} +
  Apu{Alloc,Free,CreateHeap,IsVirtualAddressValid,MapApu/VirtualAddress}
  → Audio-DSP-processor (XMA→PCM): SDL3Audio/ALSOFT+decoder. Direcciones-APU=
   .Memory-Nothing-s tructures → stub con memory-pool.
- xapu.dll: 5 = XApu{Connect,Disconnect,EnqueueCommand,DequeueResult,
  GetResultCount} → cola-de-comandos-DSP. Stub+ring-buffer.
- chad.dll: 5 = XDsp{Activate,Deactivate,ConnectWithMaximumStreamLimit,
  Disconnect,SubmitCommand} → mando-DSP. Stub passthrough. (5+9+5=19 calls
  audio-DSP: la CADENA-ENTERA-Xbox-audio = 19 stubs + un mixer).

## HALLAZGO DEL CONFIG (2.5KB, plist)
TitleId 7952987A, Store 9NL3WWNZLZZN, MSA 000000004060F900, SizeMB 2048,
SMTEnabled true, **XboxLockhart/AnacondaTitleMemory=Advanced** → el título usa
memory-Advanced-annotations en Series-S (Lockhart!) Y-X: la shim-de-memoria-Track-B
debe-HONOR-annotations-Advanced (8GB-heap-pooling-X, 10GB-S-fast-ESRAM-path).
Declare-0-side-DLLs: MSIX-VIC-TODAS-via-imports (68): no-hay-nada-oculto-en-manifest.

## SIDE-DLLs: en disco pero NO-en-imports (carga-DINÁMICA via LoadLibraryW)
- XCurl.dll (199KB, 0-imports-públicos): la-biblioteca-HTTP-de-GDK. Cargada via
  LoadLibrary (9-LoadLibrary*-imports en la tabla-core). Espera: FREE-público sí:
  los 38-de-libHttpClient.GDK.dll-si-están. XCurl = Internal-helper-de-libHttp.
- amd_fidelityfx_upscaler_xbox.dll (786KB): cargada-POR-el-loader (ffxCreateContext
  la-popula). DENTRO-de-ffx-sdk-flow. 0-imports-estáticos.
- dstorage.dll / CetAdvancedCompress.dll: NO-EXISTEN-en-disco. DirectStorage X =
  XCRDAPI (1-import) + kernel-DM-que-lo-sirve (G6Recomp-direct_storage.cpp ✓).

## CONCLUSIÓN Track B (el mapa costos-completo)
- 482-imports (82%): CRT+Win32-core → msvcxx-abi+wine-headers. MECHANIC.
- 105-imports (18%): 15-DLLs-exóticas → arriba: 1 GameInput-stub, 1 DStorage
  (ya-existe-en-G6Recomp), 3-d3d12(→vtable), 4-ffx, 2-xg, 8-BVH(RTAS→VKD3D-AS),
  7-MF(→ffmpeg), 12-xmem(malloc), 23-Bink(→librempeg ✓), 19-audio-DSP(stubs).
- TOTAL-exótico: 105. De-los-cuales-58 (Bink23+xmem12+audio19+MF7-3) = YA-MAPEADOS
  1:1-a-componentes-Linux-EXISTENTES. El-85% del-exótico-NO-requiere-inventar.

## Método+trampas
- Los-ordinales-GameInput (ordinal_1)= única-forma-de-verlo: hint-name-AUSENTE.
- delay-imports=EMPTY (DataDir[13]=0) — NO-hay carga-diferida: TODO-está-en-el-587.
- Bound-imports-EMPTY. LoadConfig-EMPTY: sin-CFG/CTL-guard.
- 8º-BVH-decodificado-DEL-RAW-rva: "GetRaytracingAccelerationStructurePrebuildInfo"
  (mi-ventana-200B-lo-truncaba-a-rva:names>196-chars-son-RAROS-pero-EXISTEN).
