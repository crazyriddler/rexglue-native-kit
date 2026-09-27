#pragma once

#include "shader.h"
#include "shader_code.h"

struct StringBuffer
{
    std::string out;

    template<class... Args>
    void print(fmt::format_string<Args...> fmt, Args&&... args)
    {
        fmt::vformat_to(std::back_inserter(out), fmt.get(), fmt::make_format_args(args...));
    }

    template<class... Args>
    void println(fmt::format_string<Args...> fmt, Args&&... args)
    {
        fmt::vformat_to(std::back_inserter(out), fmt.get(), fmt::make_format_args(args...));
        out += '\n';
    }
};

struct ShaderRecompiler : StringBuffer
{
    uint32_t indentation = 0;
    bool isPixelShader = false;
    const uint8_t* constantTableData = nullptr;
    std::unordered_map<uint32_t, VertexElement> vertexElements;
    std::unordered_map<uint32_t, std::string> interpolators;
    std::unordered_map<uint32_t, const ConstantInfo*> float4Constants;
    std::unordered_map<uint32_t, const char*> boolConstants;
    std::unordered_map<uint32_t, const char*> samplers;
    std::unordered_map<uint32_t, uint32_t> ifEndLabels;
    // Structured if/else emission: instruction indices where a "} else {" is
    // emitted (then-branch close is part of the event, not ifEndLabels).
    std::unordered_set<uint32_t> elseLabels;
    uint32_t specConstantsMask = 0;

#ifdef CONAN_RECOMP
    // Structured loops: unique id per emitted loop (iterator / aL save names).
    uint32_t loopCounter = 0;
    std::vector<uint32_t> loopStack;
    // Literal loop constants from the definition table (defi), keyed by loop id.
    std::unordered_map<uint32_t, uint32_t> literalLoopConstants;
    std::string loopConstantExpr(uint32_t loopId) const
    {
        auto it = literalLoopConstants.find(loopId);
        if (it != literalLoopConstants.end())
            return fmt::format("0x{:X}u", it->second);
        return fmt::format("g_LoopConstant({})", loopId);
    }
#endif

#ifdef UNLEASHED_RECOMP
    bool hasMtxProjection = false;
    bool hasMtxPrevInvViewProjection = false;
#endif

#ifdef REBLUE_RECOMP
    bool hasShadowTexture = false;
    uint32_t shadowTapUVCount = 0;
    std::unordered_map<uint32_t, uint32_t> shadowTapSlots;
#endif

    void indent()
    {
        for (uint32_t i = 0; i < indentation; i++)
            out += '\t';
    }

    void printDstSwizzle(uint32_t dstSwizzle, bool operand);
    void printDstSwizzle01(uint32_t dstRegister, uint32_t dstSwizzle);

    void recompile(const VertexFetchInstruction& instr, uint32_t address);
    void recompile(const TextureFetchInstruction& instr, bool bicubic);
    void recompile(const AluInstruction& instr);

    void recompile(const uint8_t* shaderData, const std::string_view& include);
};
