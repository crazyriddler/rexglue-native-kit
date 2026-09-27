> **Nota del kit (2026-09-27):** este informe es una propuesta externa, no una instrucción.
> Su evaluación punto por punto contra las mediciones del port de Conan está en
> `docs/STRATEGY_REVIEW.md`: varias ideas ya están hechas, otras quedan condicionadas a una
> medición (PERFORMANCE_GUIDE.md §"Gated optimizations") y algunas se rechazan porque rompen
> la exactitud frente a Xenos (reordenar/cullear draws, PSOs placeholder). Un agente no
> debe seguir este documento directamente.

# Backend de Renderizado Nativo en DirectX 12 para Ports ReXGlue (Xbox 360 → PC)

**Tipo de documento:** Especificación de arquitectura + guía de implementación optimizada
**Audiencia:** Agentes de IA / ingenieros que porten juegos de Xbox 360 recompilados con ReXGlue a un renderer nativo DX12
**Repositorio de referencia:** `crazyriddler/Conan2007Recomp`
**Stack objetivo:** C++20, DirectX 12 (Agility SDK), Shader Model 6.6+, HLSL/DXIL, `d3dx12.h`, PIX

---

## 0. Resumen ejecutivo

`Conan2007Recomp` demuestra que es viable eliminar el backend de GPU de Xenia (emulación de comandos Xenos) y ejecutar el juego con un renderer nativo DX12, obteniendo resolución nativa arbitraria, >60 fps, MSAA, SSAO, FXAA, sombras mejoradas, etc. Su implementación es correcta pero arquitectónicamente de "primera generación": serialización rígida de PSOs, despacho iterativo de draws desde CPU, traducción clásica de vértices por Input Assembler y emulación de eDRAM por copias.

Este documento define un backend de **segunda generación**: genérico (no hardcodeado a un juego), **GPU-driven** (instancing, culling y despacho de draws en compute), con caché de pipelines robusto, traducción de shaders Xenos→DXIL como pipeline offline, y gestión de memoria/eDRAM pensada para eliminar los cuellos de botella que el enfoque clásico introduce.

Los objetivos de rendimiento cuantitativos:

| Métrica | Objetivo |
|---|---|
| CPU frame time (main thread, 1% low) | < 2.0 ms a 120 fps |
| Draw calls despachados por frame | Consolidados en batches indirectos (100–500 `ExecuteIndirect`) |
| Stalls por compilación de PSO en runtime | 0 (precaching total desde disco + threads) |
| Banda ancha desperdiciada en resolves eDRAM | Eliminada o reducida ≥ 80 % vía tiling/aliasing |
| Sincronización GPU↔CPU | Solo 1 fence por frame presentado; ninguna espera intra-frame |

---

## 1. Anatomía del problema: qué hay que reemplazar

### 1.1 Dónde vive el trabajo

Con ReXGlue, el código PPC del juego ya es C++ nativo. El juego "habla GPU" llamando a las rutinas del kernel/XAM que en la consola enviaban comandos al **Command Processor (CP)** Xenos: un ring buffer de comandos binarios (draws, cambios de estado, resolves de eDRAM, transferencias, waits de semáforo).

En la cadena ReXGlue/Xenia por defecto, esa cadena de comandos se interpreta/emula. En un port nativo hay que **interceptar la capa de comandos** (el punto donde el código recompilado invoca al backend gráfico inyectado en el SDK) y convertirla en una representación intermedia propia:

```
Código recompilado (nativo)
   └── Llamadas a capa gráfica (injección en ReXGlue SDK / overlay)
         └── [NUEVO] Traductor de comandos Xenos → IR interno
               └── Frame scheduler → command lists DX12 → GPU
```

Recomendación arquitectónica clave: **no traducir comando-a-comando 1:1 a llamadas DX12 en el momento en que llegan** (lo que hace un backend naive y lo que produce el overhead CPU que se quiere evitar). En su lugar:

1. **Registrar** cada frame completo en una lista de "render items" ligeros (estructuras POD planas, cache-friendly).
2. **Post-procesar** esa lista (ordenación por PSO/estado, consolidación de instancias, detección de draws degenerados, cálculo de contadores).
3. **Emitir** trabajo al command list, agrupando y usando paths indirectos allá donde sea posible.

Esto desacopla la frecuencia de la API invocada por el juego de la frecuencia de trabajo real de la GPU, y es la base de todo lo que sigue.

### 1.2 Lo que la GPU Xenos hace distinto a D3D12 (y hay que reconciliar)

Conocer estas diferencias define el 80 % del diseño:

1. **eDRAM embebida (10 MB).** Los render targets de color/profundidad viven en una memoria embebida; al terminar una pasada se hace *resolve* (copia) a memoria principal. Los juegos X360 hacen cientos de estos resolves por frame. Emularlos 1:1 con copias DX12 es el mayor desperdicio de ancho de banda del enfoque clásico.
2. **Microcódigo de shaders Xenos (XU).** Los shaders existen como microcódigo R700-like de la consola, no como DXBC/DXIL. Hay que traducirlos **offline** a DXIL. No hay Input Assembler en Xenos: los vertex shaders hacen `vfetch` arbitrario desde memoria.
3. **MEMEXPORT.** Vertex/geometry shaders pueden exportar datos arbitrarios a memoria (stream out artesanal). D3D12 tiene Stream Output nativo, pero la semántica difiere; muchas veces es mejor reimplementarlo con UAV + compute.
4. **Formatos y layouts de textura.** Texturas swizzled/tiled con formatos como `A2R10G10B10`, `16f`, `DXT` (BC1-3) con variantes de la consola. El swizzle debe resolverse **una vez al cargar** (convertir a layout estándar de D3D12, o usar `D3D12_TEXTURE_LAYOUT_64KB_UNDEFINED_SWIZZLE` con shaders adaptados — ver §6.3).
5. **Estados de fixed function.** Xenos tiene estados de raster/blend/depth razonablemente mapeables a PSOs, pero con semánticas divergentes (alpha test en PS, slope-scaled depth bias para shadow map acne, cull modes, MSAA forzado a nivel de render target eDRAM).
6. **Semáforos/eventos del CP.** El juego sincroniza CPU↔GPU con semáforos Xenos (kb=... signals/waits). Hay que mapearlos a fences/eventos DX12 sin bloquear threads innecesariamente.
7. **Render a tiles.** Algunos juegos (post-procesados pesados, deferred) estructuran el frame en tiles sobre eDRAM. El backend debe detectar patrones de tile y mapearlos a renders locales, no a copias globales.

---

## 2. Arquitectura del backend (módulos)

```
┌────────────────────────────────────────────────────────────────┐
│                    Game code (recompilado)                      │
└──────────────────────────────┬─────────────────────────────────┘
                               │ interfaz inyectada (overlay / hooks)
┌──────────────────────────────▼─────────────────────────────────┐
│ 1. Command Recorder    · convierte comandos Xenos en IR POD    │
│    (ring buffer → items: draw, statechange, resolve, event)    │
├───────────────────────────────────────────────────────────────┤
│ 2. Resource Manager    · texturas, buffers, vistas, heaps,     │
│    (LRU residency, aliasing, tiled resources, swizzle decode)  │
├───────────────────────────────────────────────────────────────┤
│ 3. Pipeline Manager    · PSO cache, PSO libraries en disco,    │
│    variantes, precarga asíncrona, fallback material            │
├───────────────────────────────────────────────────────────────┤
│ 4. Shader System       · DXIL precompilado, variantas por      │
│    (define-table, estado fijo como #define, hot reload debug)  │
├───────────────────────────────────────────────────────────────┤
│ 5. Frame Scheduler     · ordenación, consolidación, culling,   │
│    generación de argumentos indirectos, barreras               │
├───────────────────────────────────────────────────────────────┤
│ 6. RHI DX12            · device, queues (direct/compute/copy), │
│    command allocators/lists, fences, swap chain, present       │
├───────────────────────────────────────────────────────────────┤
│ 7. PostFX (opcional)   · bloom, SSAO, FXAA/TAA, tonemap HDR    │
└────────────────────────────────────────────────────────────────┘
```

Reglas de diseño transversales:

- **Cero allocations en hot path.** Todo el frame se graba en arena allocators reseteados por frame (`Reset()` del command allocator, arenas con bump pointer). Ninguna llamada a `new`/STL en el camino de grabación.
- **Estructuras de datos planas.** Arrays de PODs (SoA preferible a AoS) para render items, descriptor indices y argumentos indirectos.
- **Todo lo que se pueda hacer offline, offline.** Traducción de shaders, swizzle de texturas precargadas, generación de PSO caches, y (idealmente) el volcado del protocolo de comandos del juego para tests de regresión.
- **Paralelismo de grabación.** El recorder es single-thread (orden del juego), pero la fase de grabación de command lists DX12 debe poder paralelizarse por pasadas (ver §4.2).

---

## 3. Sistema de pipelines (PSOs): solución al problema `conan_pipelines.bin`

El enfoque de `Conan2007Recomp` (binario propietario de PSOs) es frágil por tres razones: está atado a un hardware/driver concretos, no cubre combinaciones dinámicas, y mezcla caché con formato propietario.

### 3.1 PSO Library del driver + disco

DX12 ofrece `ID3D12PipelineLibrary` (soporta `LoadPipeline`/`StorePipeline`) y los **PSO Libraries** del driver (documentados en Windows Dev Center, usados por juegos AAA): almacenan blobs compilados por el driver específico, con invalidación automática por driver. Estrategia:

1. **Capa 1 — librería del driver (`D3D12_SERIALIZED_DATA_DRIVER_...`)**: guardar el blob que `ID3D12PipelineLibrary::Serialize` devuelve en `%LOCALAPPDATA%/<game>/pso_cache.bin`. Si el driver cambia, se ignora silenciosamente y se reconstruye.
2. **Capa 2 — caché de aplicación**: hash estable (ver §3.3) → descriptor PSO (stream) → archivo `pso_manifest.json`/binario propio con los *inputs* (no los PSO compilados) para diagnóstico y warm rebuild.
3. **Capa 3 — precarga en hilo**: al arrancar, lanzar N threads que llaman a `CreatePipelineState` para todos los PSOs del manifest del título mientras el menu carga. `CreatePipelineState` es thread-safe por dispositivo.
4. **Capa 4 — fallback de último recurso**: si en runtime aparece un PSO no cacheado, usar un **PSO placeholder monolítico** (VS/PS dummy por defecto, depth read-only) mientras un thread worker lo compila de verdad, y registrar el hash en `missing_psos.txt` para incluirlo en el precache del siguiente arranque. Jamás bloquear el frame.

### 3.2 PSO Streams en vez de descriptores monolíticos

Usar `ID3D12Device2::CreatePipelineState` con `CD3DX12_PIPELINE_STATE_STREAM_*` (incluido en `d3dx12.h`). Ventajas: estructura extensible, más barato de hashear/deduplicar, y permite crear **familias de variantes** baratas (p. ej. mismo shader, distinto blend) modificando un solo token del stream.

Regla de oro: **minimizar el espacio de variación**. Cuantos menos PSOs distintos exista, mejor. Técnicas:

- Convertir estados frecuentes de "cambio de PSO" a **parámetros dinámicos**: alpha test → `alpha-to-coverage` o discard paramétrico en PS con root constant; viewport/scissor ya son dinámicos; estencil ref con `OMSetStencilRef` si se usa `D3D12_DEPTH_STENCIL_DESC1` con depth bounds.
- Agrupar los blends/raster/depth en un set canónico de ~30-60 combinaciones y mapear el estado Xenos al más cercano canónico (documentar las diferencias visuales en tests).
- Shaders con **variantas por `#define` controladas por una tabla**, no permutaciones libres: solo compilar las variantas realmente emitidas por el juego (se descubren en el primer run y se fijan en el manifest).

### 3.3 Hash estable de estado

El hash del PSO debe cubrir exactamente los tokens del stream (root signature hash, VS/PS hash, formatos de RTV/DSV, blend/raster/depth, sample desc, input layout si aplica) y nada más. Implementar con XXH3 (rápido, no criptográfico, perfecto para hot path). Guardar el mapa hash→PSO en un `unordered_map` reservado de antemano; lookup < 50 ns.

### 3.4 Numeros reales esperables

Un juego X360 típico usa 100–800 pixel shaders distintos y un espacio de estados de fijo función de 300–3000 combinaciones tras deduplicación. Con precarga, el tiempo de creación de todos los PSOs es absorbido en la carga; en runtime, **cero stalls de compilación**.

---

## 4. GPU-driven rendering: eliminar el cuello de botella de CPU

El patrón "por cada draw: cambiar PSO, bind, `DrawIndexedInstanced`" de Conan2007Recomp escala mal: a 120 fps con 2.000–5.000 draws nativos, el solo coste de API puede comerse varios ms de CPU.

### 4.1 Consolidación de draws

Pasos, en este orden:

1. **Detección de instanciación.** Muchos juegos X360 dibujan el mismo mesh con distintas constantes (skinned meshes por hueso-batch, vegetación, props). Detectar (VB, IB, PSO hash, vertex buffer range) repetidos → convertir en un solo `DrawIndexedInstanced` con instancias, moviendo las constantes por-instancia a un structured buffer indexado por `SV_InstanceID`.
2. **Batching por pasada.** Ordenar el frame por (render target → PSO → material) para minimizar cambios de estado. La ordenación debe ser estable para no romper transparencias: usar key de 64 bits con pasada en los bits altos y un contador de secuencia en los bajos.
3. **Argumentos indirectos.** Generar un buffer de `D3D12_DRAW_INDEXED_ARGUMENTS` (VBV/IBV implícitos si se usa un vertex buffer global concatenado + `StartIndexLocation`/`BaseVertexLocation`) y emitir un **único `ExecuteIndirect`** por lote de draws compatibles. Esto convierte 500 draws en 1–5 llamadas API.

Nota importante: `ExecuteIndirect` con vertex buffers requiere que los argumentos indirectos lleven `D3D12_VERTEX_BUFFER_VIEW` si los VB cambian por draw. Alternativa superior (ver §5.2): vertex fetch en shader desde buffers globales con `SV_VertexID`/`SV_InstanceID`, de modo que los argumentos indirectos solo varían índices/offsets (tamaño fijo 20 bytes → máximo throughput).

### 4.2 Grabación paralela de command lists

- Un thread graba la "lista maestra" de items del juego.
- El frame se divide en **pasadas independientes** (shadow map, opacos, alpha, postFX). Cada pasada la graba un command list distinto en su propio thread (command allocator por thread por frame en vuelo).
- Se submiten en orden con una única `ExecuteCommandLists` al final. Regla DX12: los command lists de distintos threads nunca deben compartir allocator; cada thread tiene su pool de allocators rotados por frame-in-flight.
- Presupuesto: 2–4 threads de grabación son suficientes; más sube el coste de stitching. Medir con PIX.

### 4.3 Culling en compute

Aunque el juego original ya emite solo lo visible (su propia frustum culling de 2007), a resoluciones modernas con overdraw alto compensa:

- Cada consolidado de instancias genera una entrada en un **indirect dispatch**: compute shader hace frustum culling por instancia (+ opcional culling por oclusión con un HZB de la pasada anterior) y hace compaction con `Append`-style counter (atomic en UAV) escribiendo los `DrawIndexedInstanced` sobrevivientes en un buffer de argumentos.
- Luego `ExecuteIndirect` con `pArgumentBuffer` = resultado. CPU ni siquiera sabe cuántos draws sobrevivieron.
- Este es el patrón clásico GPU-driven (GpuCulling → ExecuteIndirect) y suele dar **2–5× menos trabajo de rasterización** en escenas X360 densas renderizadas a 4K.

Si el tiempo de implementación importa, fase 1 = solo consolidación de instancias + ExecuteIndirect estático (argumentos generados en CPU pero con coste amortizado); fase 2 = culling en compute.

### 4.4 Cola async compute

Usar una segunda queue (compute) para:

- Deswizzle/conversión de texturas en caliente (ver §6.3).
- Filtros de partículas, skinning alternativo, generación de mipmaps.
- PostFX (SSAO, bloom) ejecutados en paralelo con la siguiente pasada de opacos cuando no haya dependencia.

Sincronización: semáforos implementados con `ID3D12Fence` y waits en el command list (`Wait`/`Signal` de queues cruzadas) — nunca con `WaitForSingleObject` en el thread de render salvo en present.

---

## 5. Shaders: traducción Xenos → DXIL

### 5.1 Pipeline offline (obligatorio)

El microcódigo Xenos se traduce **offline** con una herramienta propia (basada en el disassembler de ucode de Xenia, que ya entiende el ISA Xenos):

1. **Dump**: instrumentar el backend una única vez (o usar volcados de Xenia) para extraer todos los shaders del juego.
2. **Traducción a IR**: levantar el CFG, reconstruir control flow estructurado (if/loop/switch), tipar registros.
3. **Emisión HLSL/DXIL**:
   - `vfetch` → loads desde `ByteAddressBuffer`/`StructuredBuffer` con `SV_VertexID` (posición del índice en el IB global).
   - Constantes float4 Xenos → root constants o CBV según frecuencia de cambio (ver §5.3).
   - `tfetch` → `Texture2D.SampleGrad` con los gradientes explícitos que el shader Xenos ya calculaba (Xenos no tiene derivatives implícitos automáticos como DX; preservar la semántica exacta evita artefactos).
   - MEMEXPORT → escrituras a UAV (y/o Stream Output si el patrón encaja).
   - Precisión: usar `precise`/`fmul` nativo donde el juego dependa del redondeo del hardware Xenos (p. ej. animaciones faciales de algunos títulos); documentar por juego.
4. **Validación**: compilar con `dxc` (-T vs_6_6/ps_6_6, `-all-resources-bound`, `-WX`), test visual automatizado por shader (goldens de frame).

Jamás traducir shaders en runtime: el coste y la fragilidad no se justifican. El runtime solo ve DXIL ya compilado.

### 5.2 Sin Input Assembler: vertex fetch en shader

Decisión arquitectónica central y mejora directa sobre Conan2007Recomp:

- **No usar `D3D12_INPUT_LAYOUT_DESC` salvo fallback.** En su lugar: IB global concatenado (formato UINT16 expandido a UINT32 en carga o al vuelo con compute si el juego los mezcla), VB globales con layout Xenos **tal cual** en GPU, y el vertex shader hace los `Load` con offsets calculados desde `SV_VertexID`.
- Beneficios: (1) cero reformato de vértices por CPU; (2) argumentos indirectos uniformes (sin VBV variables); (3) layouts Xenos exóticos (semánticas extra, compresión de normales 10:10:10, etc.) se interpretan en shader sin copias; (4) los mismos shaders sirven para cualquier juego con el mismo convenio de fetch.
- Coste: los `Load` en VS consumen más instrucciones que la IA fija. En la práctica es neutral o positivo en GPUs modernas y elimina un cuello de botella entero.

### 5.3 Root signature: una sola, estática, bindless

Una root signature monolítica para todo el juego (o unas pocas: opacos/alpha/compute/post):

```
Root Signature (versión estática):
- Descriptor Table: SRV texturas (heap principal, bindless)
- Descriptor Table: UAV (heap principal)
- Descriptor Table: samplers (static samplers para los ~16 estados canónicos + heap para el resto)
- Root Constants: índices de material, draw, parámetros rápidos (≤ 8 DWORDs)
- Root CBV: constantes de frame (view/projection, tiempo, resolución)
- Root CBV: constantes de material (per-draw, en ring buffer)
```

Con **SM 6.6** se hace aún mejor: `ResourceDescriptorHeap[]` / `SamplerDescriptorHeap[]` acceden directamente a los heaps del device desde HLSL. Declarar la root signature con `D3D12_ROOT_SIGNATURE_FLAG_CBV_SRV_UAV_HEAP_DIRECTLY_INDEXED | D3D12_ROOT_SIGNATURE_FLAG_SAMPLER_HEAP_DIRECTLY_INDEXED` permite indexar cualquier descriptor sin root tables en absoluto: el juego asigna índices estables por recurso y los shaders los reciben por root constants. Esto reduce el coste por draw al mínimo absoluto y simplifica el código C++.

Reglas:
- Static samplers para los estados de filtrado más usados (los juegos X360 usan pocos: point/bilinear/trilinear/aniso con wraps limitados) → cero coste de bind.
- Nunca más de 1 root CBV que cambie por draw; el resto por root constants o por índices a un structured buffer de constantes (más barato aún: 1 DWORD).
- Marcar la root signature con los flags de versión y optimización (`LOCAL_ROOT_SIGNATURE` solo si se usa DXR, que no es necesario).

### 5.4 Tabla de defines / variante mínima

Los pocos bits de estado fijo que se quieren fuera del PSO (alpha test, fog on/off, gamma de textura, etc.) se implementan con **una tabla de variantas enumeradas** compiladas offline, seleccionadas por un root constant. Esto evita la explosión combinatoria de PSOs.

---

## 6. Recursos y memoria

### 6.1 Heaps y residencia

- `D3D12_HEAP_TYPE_DEFAULT` con `D3D12_HEAP_FLAG_CREATE_NOT_ZEROED` (SM 6.6+/Agility) para recursos de juego: la GPU no debe pagar el cero de grandes allocations.
- Recursos colocados (`CreatePlacedResource`) sobre heaps grandes (1–4 GB en Tier 1/2 según VRAM) → menos fragmentación y posibilidad de aliasing.
- **Residency LRU**: el juego puede referenciar más recursos de los que caben. Mantener un LRU de texturas con `MakeResident`/`Evict` en un thread de streaming, con el set residente dimensionado a VRAM−margen. D3D12 da el control que D3D11 no daba; usarlo.
- Budget awareness: `IDXGIAdapter3::QueryVideoMemoryInfo` cada N frames para adaptar el LRU en tiempo real.

### 6.2 Upload path

- Ring buffer de upload por frame-in-flight (double/triple buffer) con `WriteBufferImmediate` o map+copy por rango; nunca `UpdateSubresources` genérico en hot path (hace allocations y copias intermedias).
- Subidas mayores (texturas) por cola **copy** dedicada con su propio fence, solapadas con el frame.
- Vértices/índices del juego: como el código recompilado puede escribir en memoria guest que mapea a recursos GPU, usar heaps de upload persistentes con rangos rotados por fence, o `WRITE_WATCH` sobre rangos para detectar modificaciones y subir solo dirty pages.

### 6.3 Texturas: swizzle y formatos

- **Offline/pre-carga**: convertir texturas swizzled Xenos a layouts lineales estándar D3D12 en la primera carga y cachear el resultado en disco (cache por hash del recurso). Nunca swizzlear en hot path.
- **Formato**: mapear formatos Xenos a DXGI (tabla en anexo). Casos especiales:
  - `10:10:10:2` → `R10G10B10A2_UNORM` (existe en DXGI, cero coste).
  - DXT/BC con semánticas de consola → BC1-3 estándar si la variante coincide; si no, transcodificar offline (texconv-style propio).
  - Formatos de render target Xenos raros → el más cercano con shader-side fix documentado.
- **Tiled resources opcionales (avanzado)**: para texturas gigantes (algunos juegos con streaming), `D3D12_TILED_RESOURCE_TIER_3` permite mapear solo los tiles residentes. Fase tardía del proyecto.
- **Sampler Feedback (SM 6.4+)**: para streaming por demanda real, usar `WriteSamplerFeedback`/min-mipmap con predicción. Solo si el juego lo justifica; la mayoría de ports X360 caben completos en VRAM moderna.

### 6.4 eDRAM: la optimización con mayor ROI del proyecto

Estrategia escalonada:

**Nivel 0 (baseline, lo que hace Conan2007Recomp):** emular eDRAM como texturas normales; cada resolve = copia. Correcto, lento. Solo como fallback y para validación.

**Nivel 1 (detección de superficies):** mantener un mapa de las "superficies" eDRAM (color+depth+MSAA+rect). Si el resolve es completo y la superficie no se reutiliza como textura intermedia por el juego en el sentido estricto Xenos, **omitir la copia y dejar el resultado directamente en el render target** (el juego simplemente lo muestreará después). Esto ya elimina la mayoría de resolves triviales (los juegos resolvían por arquitectura, no por necesidad lógica).

**Nivel 2 (aliasing por frame):** las superficies eDRAM que no se solapan en tiempo dentro del frame se asignan con **resource aliasing** sobre el mismo heap físico (barriers de alias entre usos). Con 10 MB de eDRAM original y 8+ GB de VRAM moderna esto no es por espacio, sino por *cache*: las superficies vivas simultáneamente caben en L2/VRAM caliente, y las copias eDRAM→textura son reemplazadas por transiciones de estado sobre el mismo recurso.

**Nivel 3 (patrones tile):** si el juego hace rendering por tiles sobre eDRAM (detectable por viewport/scissor estáticos repetidos + resolves parciales), implementar el tile loop nativo: render directo a un tile cache en memoria local con `RSSetViewports` por tile, o compute-based tiling. Esto además habilita aplicar AA/efectos por tile modernos.

Medir con PIX: los resolves deben desaparecer del perfil de memoria salvo casos semánticamente necesarios.

### 6.5 MEMEXPORT / stream out

Recomendación por defecto: reimplementar como **UAV writes desde el shader traducido** (con allocators en GPU si hay compaction) o como compute post-paso, antes que Stream Output API (SO limita la portabilidad de los argumentos indirectos y añade estado). Si el juego usa MEMEXPORT masivamente (p. ej. para GPU skinning), un compute shader dedicado es más rápido y más simple de depurar.

---

## 7. Frame pacing, present y sincronización

### 7.1 Present moderno

- Swap chain: `CreateSwapChainForHwnd` con `DXGI_SWAP_EFFECT_FLIP_DISCARD`, buffer count 3, `DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING` (con chequeo de `CheckFeatureSupport`), y **waitable object** (`SetMaximumFrameLatency` + `IDXGISwapChain2::SetMaximumFrameLatency` / frame latency waitable) para un pacing de sub-frame preciso.
- Formatos: backbuffer `R10G10B10A2` o `R16G16B16A16_FLOAT` + `DXGI_COLOR_SPACE_RGB_FULL_G10_NONE_P709`/`P2020` para HDR opcional (la ruta HDR convierte el render a scRGB y aplica tonemap propio; muchos juegos X360 ya trabajan en espacio lineal internamente).
- VSync desactivado por defecto con tearing permitido, o limitador propio con política configurable.

### 7.2 Sincronización

- **Frames in flight: 2** (3 solo si hay async compute pesado). Más in-flight = más latencia de input sin beneficio a fps fijo.
- Un fence por frame; cada frame al inicio hace `SetEventOnCompletion(frameN-2)` y solo se espera ese evento **una vez por frame**, antes de resetear allocators/reciclar heaps. Nada de esperas selectivas intra-frame.
- Los semáforos Xenos del CP se mapean a `Signal`/`Wait` de queue cuando hay dependencia GPU→GPU, y a fence de frame cuando hay CPU→GPU; esperas de CPU solo en el punto de reciclaje.
- Frame pacing: `WaitForSingleObjectEx` sobre el frame latency waitable, nunca bucles de sleep.

### 7.3 Latencia

- El juego fue diseñado para 30/60 Hz de consola. A 120+ fps desbloqueados (Conan2007Recomp soporta hasta 120) puede romperse física — mantener limitador configurable y delta-time clamping en la capa de tiempo (ReXGlue ya expone timebase).

---

## 8. Post-procesado y mejoras visuales nativas

El port nativo permite ir más allá de replicar la consola. En orden de ROI:

1. **Resolución render arbitraria + upscale opcional** (FSR/XeSS/DLSS si se integra SDK correspondiente; la estructura de RTs propia lo facilita).
2. **MSAA real** (4×/8×) — barato en GPUs actuales comparado con 2007 y el juego ya estaba diseñado para MSAA 4×.
3. **Anisotropía forzada configurable** (16×) — reemplaza el trilinear de la consola.
4. **Bloom/SSAO/FXAA propios** con parámetros por juego guardados en `postfx.toml`.
5. **HDR** con tonemap configurado por título (algunos juegos X360 tienen efectos medidos en gamma space; el tonemap debe respetarlo para no lavar la imagen).
6. Extra: TAA, sharpening por CAS, dithering opcional (incluido en Conan2007Recomp).

Implementación: cada efecto es una pasada de compute/PS sobre la superficie post-resolve, encadenada en la cola async cuando no bloquee opacos. Nada de efectos en el critical path de la cadena de opacos.

---

## 9. Infraestructura de depuración y perfilado (hacer esto desde el día 1)

- **PIX** en cada máquina de desarrollo: marcadores de evento por pasada (`PIXBeginEvent`), capturas con `PIXCaptureInterface` programáticas ante artefactos.
- **GPU-based validation** en debug (`D3D12_MESSAGE_SEVERITY_CORRUPTION`, break on error) y DRED (`ID3D12DeviceRemovedExtendedData`) en release con auto-report.
- **Timestamp queries** por pasada con overlay propio (o ImGui ya integrado en ReXGlue) mostrando ms de GPU por etapa: opacos, alpha, sombras, postFX, present.
- **Contadores de renderer**: draws emitidos, draws consolidados, PSOs activados, misses de caché, MB copiados por resolves, tamaño de heaps. Volcado a CSV para regresiones.
- **Tests de imagen**: golden frames comparados por hash perceptual en CI para cada shader traducido y cada escena de referencia. Cambio de shader = revalidación automática.
- **Reproducción determinista**: grabar N segundos del IR interno (lista de render items) y poder reproducirlos sin el juego, para perfilar el renderer aislado.

---

## 10. Plan de implementación por fases

| Fase | Contenido | Criterio de salida |
|---|---|---|
| **0. Instrumentación** | Interceptar capa gráfica del juego; dumper de comandos/shaders/texturas; renderer "null" que solo cuenta draws/estados | Estadísticas completas del título (cuántos shaders, qué formatos, cuántos resolves, cuántos PSOs) |
| **1. Baseline correcto** | Traductor IR, renderer clásico (IA + draws directos, eDRAM por copias, PSO sync), un solo juego funcionando pixel-perfect frente a Xenia | Paridad visual en escenas de referencia; 0 crashes en 1 h de gameplay |
| **2. Robustez PSO** | PSO libraries disco + driver, precarga paralela, fallback placeholder, manifest por título | 0 stalls de PSO en runtime |
| **3. Optimización CPU** | Consolidación de instancias, ordenación, ExecuteIndirect estático, grabación paralela, arena allocators | Frame time CPU < 4 ms en main thread |
| **4. GPU-driven** | Vertex fetch en shader (sin IA), bindless SM 6.6, culling en compute con HZB, argumentos indirectos dinámicos | −60 % draws rasterizados típicos; CPU < 2 ms |
| **5. Memoria** | Placed resources + aliasing, residency LRU, upload rings, deswizzle offline cacheado | Resolves eDRAM −80 %; sin evictions visibles (hitches) |
| **6. Calidad** | PostFX propios, HDR, AA, upscale; pixel-perfect opcional "modo clásico" | Feature parity + mejoras configurables por usuario |
| **7. Generalización** | Segundo título portado cambiando solo datos (manifests, tablas de formatos, quirks), no código | Mismo binario backend soporta 2 juegos |

---

## 11. Checklist de cuellos de botella (autodiagnóstico)

| Síntoma | Causa probable | Fix |
|---|---|---|
| Hitches regulares con disco activo | Creación de PSO/precarga perezosa | Precache total offline + PSO library |
| CPU alto, GPU ociosa, muchas llamadas API | Despacho iterativo tipo v1 | Consolidación + ExecuteIndirect |
| Banda ancha memoria saturada, fps bajo en 4K | Resolves eDRAM 1:1 | Nivel 1–2 de §6.4 |
| Stalls de 5–20 ms aleatorios | `WaitForSingleObject` intra-frame / reciclaje mal sincronizado | Un solo wait por frame; frames in flight fijos |
| Coste de bind alto por draw | Root tables por draw / root signatures múltiples | Bindless SM 6.6, una root signature |
| Texturas con artefactos de canal/precisión | Formato o swizzle mal mapeado | Tabla de formatos + tests goldens |
| Alpha sorting roto tras reordenar | Ordenación no estable por pasada | Key con contador de secuencia (§4.1.2) |
| Física rota > 120 fps | Timebase del juego | Limitador + clamp de delta time |

---

## Anexo A. Mapeo de estados Xenos → D3D12 (tablas de referencia)

### A.1 Blend / raster / depth
| Xenos (nombre Xenia) | D3D12 |
|---|---|
| kBlendFactor_Zero/One | D3D12_BLEND_ZERO / ONE |
| kBlendFactor_SrcAlpha / InvSrcAlpha | SRC_ALPHA / INV_SRC_ALPHA |
| kBlendFactor_DestColor | DEST_COLOR |
| kBlendOp_Add/RevSub/Min/Max | BLEND_OP_ADD / REV_SUBTRACT / MIN / MAX |
| CullMode Front/Back/None | CULL_MODE_FRONT / BACK / NONE |
| FrontCounterClockwise | según constate el juego (fijar por título en manifest) |
| DepthFunc LessEqual / Greater | COMPARISON_FUNC_LESS_EQUAL / GREATER |
| PolygonOffset (slope units) | DepthBias / SlopeScaledDepthBias |

### A.2 Formatos frecuentes
| Xenos | DXGI | Notas |
|---|---|---|
| k_8_8_8_8 (ARGB) | R8G8B8A8_UNORM | swizzle de canales en carga si aplica |
| k_10_11_11 / k_11_11_10 | R11G11B10_FLOAT | HDR interno frecuente |
| k_2_10_10_10 | R10G10B10A2_UNORM | depth-as-color en algunos juegos |
| k_16_16_16_16_FLOAT | R16G16B16A16_FLOAT | RT HDR típico |
| DXT1/3/5 | BC1/BC2/BC3 | verificar variantes de consola |
| k_16_16_FLOAT (depth) | D32_FLOAT / R16_FLOAT | replicar precisión para sombras |
| EDRAM color 32/64/128bpp | R8G8B8A8 / R16G16B16A16_FLOAT / R11G11B10_FLOAT | |

### A.3 Sampler states
Mapear los wraps/filters Xenos a ~16 static samplers canónicos; el resto a heap. Aniso forzado por usuario = override en static sampler table.

---

## Anexo B. Estructura de datos sugerida (IR interno)

```cpp
struct RenderItem {
    uint64_t  sortKey;        // pass | psoHash | material | seq
    uint32_t  psoIndex;       // índice a array de PSO
    uint32_t  materialParams; // root constants empaquetadas
    uint32_t  cbIndex;        // slot en ring buffer de constantes
    uint32_t  srvBase;        // índice base en heap bindless
    uint32_t  indexOffset;    // en IB global
    uint32_t  vertexOffset;   // en VB global
    uint32_t  indexCount;
    uint32_t  instanceCount;
    uint16_t  flags;          // alphaTest, doubleSided, depthOnly...
    uint16_t  passId;
};
```

Generación de argumentos indirectos: un compute lee el array de `RenderItem` visible y emite `D3D12_DRAW_INDEXED_ARGUMENTS` compactados; `ExecuteIndirect` con `MaxCommandCount` fijo y `pCountBuffer`.

---

## Anexo C. Referencias

- `crazyriddler/Conan2007Recomp` — port de referencia (baseline v1).
- `rexglue/rexglue-sdk` — SDK de recompilación; el backend gráfico es una interfaz inyectable por diseño (entrevista del creador, readonlymemo 2026): el objetivo declarado del proyecto es precisamente sustituir el backend de GPU de Xenia por renderizado nativo.
- Xenia (xenia-project) — fuente del disassembler de ucode Xenos y semánticas del CP/eDRAM.
- Microsoft DirectX 12 documentation — PSO Libraries, ExecuteIndirect, Enhanced Barriers, SM 6.6 dynamic resources, sampler feedback.
- `Delt06/dx12-renderer` y similares — ejemplos de pipelines DX12 (deferred, HDR, postFX) reutilizables para las pasadas propias.
