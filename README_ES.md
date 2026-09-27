# Kit de ports nativos ReXGlue: guía rápida

Este kit sirve para convertir un juego de Xbox 360 en un port nativo de PC con render
nativo D3D12, usando lo aprendido en el port de Conan (2007).

## Qué contiene

| Carpeta | Contenido |
|---|---|
| `sdk/` | Nuestra versión de ReXGlue v0.10.0 (rama native-render), lista para compilar sin descargas |
| `game/` | **Aquí va el juego**: `default.xex` y todas sus carpetas de datos, sin recompilar |
| `docs/` | La base de conocimiento: guía por fases, lecciones aprendidas (problema -> causa -> solución), arquitectura del render nativo, notas del XDK, shaders, rendimiento, validación, launcher y release, toolchain |
| `reference/conan/` | El port de Conan completo como ejemplo: código fuente, manifiesto comentado, registro de errores E001-E051 y experimentos EXP-001-048 |
| `tools/` | Herramientas de benchmark, perfilado, comparación de imágenes y shaders (XenosRecomp parcheado, DXC), análisis del ejecutable (incluido `xdk_sigs.py`, que encuentra solo las funciones D3D del XDK en un juego nuevo) y creación de la release |
| `bench/` | Scripts para lanzar el juego de forma automática y reproducible, comparación A/B fotograma a fotograma y medición de rendimiento |
| `scripts/` | Entorno del compilador (`dev_env.sh`) y utilidades del port (descifrado del XEX, detectores de errores de codegen...) |
| `.claude/` | Agentes y skills de Claude Code actualizados con este proceso |
| `CLAUDE.md`, `START_PROMPT.md`, `RESUME_PROMPT.md` | Instrucciones de control y prompts de inicio y reanudación |
| `kit.env` | Configuración del proyecto (nombre del juego, rutas del toolchain), que rellena Claude en la fase 0 |

## Cómo empezar un port nuevo

1. Copia esta carpeta entera con otro nombre (por ejemplo `C:\Users\jrbar\port-<juego>`).
   Si puedes, usa una ruta sin espacios. Deja este kit sin tocar como plantilla.
2. Copia el juego dentro de `game/` (el `default.xex` y todas sus carpetas).
3. Abre Claude Code en esa carpeta y pega el contenido de `START_PROMPT.md`, o ejecuta
   `START_CLAUDE_FULL_ACCESS.cmd`, que lo lanza con permisos completos.
4. Para retomar otro día: `RESUME_CLAUDE_FULL_ACCESS.cmd` o pega `RESUME_PROMPT.md`.

## Qué hará Claude, por fases (detalle en `docs/NATIVE_PORT_PLAYBOOK.md`)

0. Prepara el proyecto: comprueba el compilador, rellena `kit.env`, compila la herramienta `rexglue` y crea el proyecto con `rexglue init`.
1. Recompila el ejecutable sin errores de análisis (codegen).
2. Compila, arranca y corrige los cierres hasta tener menús, partida, guardado y vídeos con el render emulado (Xenos).
3. Mide el rendimiento de base.
4. Localiza las funciones gráficas del juego y del XDK.
5. Traduce todos los shaders a DXIL por adelantado.
6. Crea el render nativo a partir del de Conan, comparándolo fotograma a fotograma con Xenos.
7. Deja el port completamente nativo, sin plugin de GPU emulada.
8. Optimiza el rendimiento.
9. Añade el launcher, las opciones gráficas y las mejoras.
10. Genera la release portable.

## Requisitos de este PC (ya instalados)

LLVM/Clang 23 portable, xwin (CRT y Windows SDK sin Visual Studio), ninja, CMake,
Python 3.14 con numpy, Pillow, xxhash y pycryptodome, ProcDump, windbg-tool y RenderDoc.
Rutas en `kit.env` y `docs/TOOLCHAIN_SETUP.md`.

## Qué depende del juego (lo que no se puede garantizar)

Cada juego tiene sus propios fallos de recompilación, su propia versión del XDK y su
propio motor. El kit reduce mucho el trabajo porque ya contiene los mecanismos, las
herramientas y las soluciones a los problemas conocidos, pero cada juego nuevo tendrá
problemas nuevos. El método para diagnosticarlos está documentado.
