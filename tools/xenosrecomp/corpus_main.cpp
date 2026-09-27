// XenosRecompCorpus: per-shader driver around (patched) XenosRecomp's
// ShaderRecompiler, used by tools/shaders/build_corpus.sh.
//
//   XenosRecompCorpus <shader_common.h> <out_dir> <container.bin>...
//
// For every input container writes <out_dir>/<stem>.hlsl (stem = file name
// without ".bin") and prints one line per shader to stdout:
//   OK <stem> <ps|vs> specConstantsMask=<hex> structured=<0|1>
// Crashes/asserts are isolated by the caller (one process per shader).
// DXIL/SPIR-V compilation is done by the caller with dxc.exe so failures are
// captured per shader with full diagnostics.

#include "shader_recompiler.h"

#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#ifdef _WIN32
#include <crtdbg.h>
#endif

static bool readFile(const std::filesystem::path& p, std::vector<uint8_t>& out)
{
    std::ifstream f(p, std::ios::binary);
    if (!f)
        return false;
    out.assign(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
    return true;
}

int main(int argc, char** argv)
{
#ifdef _WIN32
    // Asserts must print to stderr and terminate, never pop a dialog.
    _set_error_mode(_OUT_TO_STDERR);
    _set_abort_behavior(0, _WRITE_ABORT_MSG | _CALL_REPORTFAULT);
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX);
#endif
    if (argc < 4)
    {
        fprintf(stderr, "usage: XenosRecompCorpus <shader_common.h> <out_dir> <container.bin>...\n");
        return 1;
    }

    std::vector<uint8_t> includeData;
    if (!readFile(argv[1], includeData))
    {
        fprintf(stderr, "cannot read %s\n", argv[1]);
        return 1;
    }
    std::string includeText;
#ifdef CONAN_RECOMP
    // Make the emitted HLSL self-contained: shader_common.h selects the Conan
    // shared-constant layout / loop / bool helpers on this define.
    includeText = "#define CONAN_RECOMP 1\n";
#endif
    includeText.append(reinterpret_cast<const char*>(includeData.data()), includeData.size());
    std::string_view include(includeText);
    std::filesystem::path outDir = argv[2];
    std::filesystem::create_directories(outDir);

    int failures = 0;
    for (int a = 3; a < argc; a++)
    {
        std::filesystem::path in = argv[a];
        std::vector<uint8_t> data;
        if (!readFile(in, data))
        {
            printf("FAIL %s read-error\n", in.stem().string().c_str());
            failures++;
            continue;
        }
        // Pad so over-reads past the container end stay in bounds.
        data.resize(data.size() + 4096, 0);

        ShaderRecompiler recompiler;
        recompiler.recompile(data.data(), include);

        std::string stem = in.stem().string();
        std::filesystem::path outPath = outDir / (stem + ".hlsl");
        std::ofstream o(outPath, std::ios::binary);
        o << recompiler.out;
        o.close();

        bool structured = recompiler.out.find("switch (pc)") == std::string::npos;
        printf("OK %s %s specConstantsMask=%X structured=%d\n", stem.c_str(),
            recompiler.isPixelShader ? "ps" : "vs", recompiler.specConstantsMask, structured ? 1 : 0);
        fflush(stdout);
    }
    return failures ? 2 : 0;
}
