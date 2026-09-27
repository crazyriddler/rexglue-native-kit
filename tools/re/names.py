"""Recovered names for the Conan (2007) statically linked XDK D3D library + renderer layer.
Evidence for each is in docs/RENDERER_ANALYSIS.md."""
import statetables
N = {
 0x822DE388: 'Direct3D_CreateDevice',
 0x822DE128: 'D3DDevice_Release',
 0x822DE000: 'D3D_AllocDevice',
 0x822E6C30: 'D3DDevice_Init(CreateDevice body)',
 0x822E6500: 'D3D__ResetAllState(dispatch-table seed)',
 0x822E6EB0: 'D3DDevice_Destroy',
 0x822DFBA0: 'D3DDevice_SetRingBufferParameters',
 0x822DF848: 'D3D_RingMakeSpace(KickOff+wrap)',
 0x822DEC70: 'D3D_AddCallsToPrimaryBuffer(INDIRECT_BUFFER)',
 0x822DF100: 'D3D_KickOff',
 0x822DFA68: 'D3D_RingAlloc(dwords)',
 0x822DFAD0: 'D3DDevice_BlockUntilIdle',
 0x822E8EB8: 'D3DDevice_Swap',
 0x822E8EB0: 'D3DDevice_Swap(thunk)',
 0x822E2F48: 'D3DDevice_SetRenderTarget',
 0x822E3BE8: 'D3DDevice_SetRenderTarget(thunk)',
 0x822E32B0: 'D3DDevice_SetDepthStencilSurface',
 0x822E3588: 'D3DDevice_SetSurfaces(tiling)',
 0x822E2878: 'D3DDevice_GetRenderTarget',
 0x822E2E10: 'D3DDevice_SetViewport',
 0x822E2B98: 'D3DDevice_SetViewportF',
 0x822E25C8: 'D3DDevice_SetScissorRect',
 0x822E2A50: 'D3DDevice_SetClipPlane',
 0x822E26C8: 'D3DDevice_SetStreamSource',
 0x822E27E8: 'D3DDevice_SetIndices',
 0x822E5BE0: 'D3DDevice_SetTexture',
 0x822E8200: 'D3DDevice_SetVertexDeclaration',
 0x822E86C0: 'D3DDevice_SetFVF',
 0x822E7CE8: 'D3DDevice_SetVertexShader',
 0x822E7FE8: 'D3DDevice_SetPixelShader',
 0x822E7EA8: 'D3DDevice_GetVertexShader',
 0x822E81B8: 'D3DDevice_GetPixelShader',
 0x822E7B78: 'D3DDevice_SetVertexShaderConstantB',
 0x822E7BD8: 'D3DDevice_SetPixelShaderConstantB',
 0x822E7C38: 'D3DDevice_SetVertexShaderConstantI',
 0x822E7C90: 'D3DDevice_SetPixelShaderConstantI',
 0x822E7A48: 'D3DDevice_GpuBeginShaderConstantF4',
 0x822E8390: 'D3DDevice_SetShaderGPRAllocation',
 0x82580918: 'D3DDevice_DrawVertices',
 0x82580D00: 'D3DDevice_DrawIndexedVertices',
 0x825803F8: 'D3DDevice_BeginVertices',
 0x82580898: 'D3DDevice_EndVertices',
 0x825808B8: 'D3DDevice_DrawVerticesUP',
 0x822F5028: 'D3DDevice_Resolve',
 0x822F3EF8: 'D3DDevice_BeginTiling',
 0x822F4480: 'D3DDevice_EndTiling',
 0x822F3D88: 'D3DDevice_SetPredication',
 0x822F75F8: 'D3D_FlushShaders(SetPending VS/PS/decl)',
 0x822F6128: 'D3D_FlushTiledRenderState',
 0x822F64C0: 'D3D_WriteDirtyRegisterRange',
 0x822F6860: 'D3D_WriteDirtyConstants',
 0x822E5878: 'D3DDevice_CreateTexture',
 0x822E50B8: 'XGSetTextureHeaderEx',
 0x822E9FC0: 'D3DDevice_CreateVertexBuffer',
 0x822EA0E8: 'D3DDevice_CreateIndexBuffer',
 0x822EA088: 'D3DVertexBuffer_Lock',
 0x822EA0D8: 'D3DVertexBuffer_Unlock',
 0x822EA198: 'D3DIndexBuffer_Lock',
 0x822EA1E0: 'D3DIndexBuffer_Unlock',
 0x822E9EF0: 'D3DResource_Release',
 0x822E98C0: 'D3DResource_GetType',
 0x822EA978: 'D3D_BeginVizQuery',
 0x822EAA48: 'D3D_EndVizQuery',
}
def all_names():
    d = dict(N)
    for a, (n, info) in statetables.load().items():
        d.setdefault(a, n)
    return d

if __name__ == '__main__':
    # Emit a TSV symbol map (address, name, static call sites) for hook authoring.
    from disdb import DB
    db = DB.load(); c = db.calls()
    for a, n in sorted(all_names().items()):
        print('0x%08X\t%s\t%d' % (a, n, len(c.get(a, []))))
