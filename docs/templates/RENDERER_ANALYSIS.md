# Renderer analysis - <GAME>

Addresses are guest VAs. Confidence: H (several independent facts), M (behaviour + one
strong clue), L (label only). Template: reference/conan/docs/RENDERER_ANALYSIS.md.

## 1. Big picture and call graph
## 2. Global anchors (device pointer, front buffer, tables)
## 3. XDK D3D functions (tools/re/xdk_sigs.py match + confirmation)
| Guest addr | Name | Role / signature | Evidence | Conf | Hook |
|---|---|---|---|---|---|
## 4. Data structures (device offsets if they differ from docs/XDK_D3D_NOTES.md)
## 5. Game renderer layer (present, pass runner, batch renderer, materials, post effects)
## 6. Render passes
## 7. Frame render-target graph (from a capture/trace)
