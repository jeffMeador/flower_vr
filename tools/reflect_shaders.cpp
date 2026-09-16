// Offline shader-reflection scanner.
// Loads every precompiled .cso in Data/Shaders, reflects its constant buffers,
// and prints any cbuffer variable whose name looks like a camera matrix
// (view/projection/world/etc). Goal: find which cbuffer slot + byte offset
// holds the data we need to override for stereo VR rendering.
#include <Windows.h>
#include <d3d11.h>
#include <d3d11shader.h>
#include <d3dcompiler.h>
#include <iostream>
#include <fstream>
#include <vector>
#include <string>
#include <filesystem>

namespace fs = std::filesystem;

static bool LooksLikeCameraVar(const std::string& name)
{
    static const char* needles[] = {
        "view", "proj", "world", "camera", "cam", "eye", "mvp", "wvp", "vp"
    };
    std::string lower;
    for (char c : name) lower += (char)tolower((unsigned char)c);
    for (auto n : needles)
        if (lower.find(n) != std::string::npos) return true;
    return false;
}

static void DumpFile(const fs::path& path, bool all)
{
    std::ifstream f(path, std::ios::binary);
    std::vector<char> blob((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    if (blob.empty()) return;

    ID3D11ShaderReflection* refl = nullptr;
    HRESULT hr = D3DReflect(blob.data(), blob.size(), IID_ID3D11ShaderReflection, (void**)&refl);
    if (FAILED(hr) || !refl) return;

    D3D11_SHADER_DESC shaderDesc = {};
    refl->GetDesc(&shaderDesc);

    for (UINT cb = 0; cb < shaderDesc.ConstantBuffers; ++cb)
    {
        ID3D11ShaderReflectionConstantBuffer* cbRefl = refl->GetConstantBufferByIndex(cb);
        D3D11_SHADER_BUFFER_DESC cbDesc = {};
        cbRefl->GetDesc(&cbDesc);

        D3D11_SHADER_INPUT_BIND_DESC bindDesc = {};
        HRESULT bindHr = refl->GetResourceBindingDescByName(cbDesc.Name, &bindDesc);

        for (UINT v = 0; v < cbDesc.Variables; ++v)
        {
            ID3D11ShaderReflectionVariable* varRefl = cbRefl->GetVariableByIndex(v);
            D3D11_SHADER_VARIABLE_DESC varDesc = {};
            varRefl->GetDesc(&varDesc);

            D3D11_SHADER_TYPE_DESC typeDesc = {};
            varRefl->GetType()->GetDesc(&typeDesc);

            bool isMatrixLike = (typeDesc.Class == D3D_SVC_MATRIX_ROWS || typeDesc.Class == D3D_SVC_MATRIX_COLUMNS)
                && typeDesc.Rows == 4 && typeDesc.Columns == 4;

            if (all || isMatrixLike || LooksLikeCameraVar(varDesc.Name))
            {
                std::cout << path.filename().string()
                    << " | cbuffer=\"" << cbDesc.Name << "\""
                    << " cbSize=" << cbDesc.Size
                    << " slot=" << (SUCCEEDED(bindHr) ? (int)bindDesc.BindPoint : -1)
                    << " var=\"" << varDesc.Name << "\""
                    << " offset=" << varDesc.StartOffset
                    << " size=" << varDesc.Size
                    << " class=" << (int)typeDesc.Class
                    << " rows=" << typeDesc.Rows
                    << " cols=" << typeDesc.Columns
                    << " type=" << (isMatrixLike ? "float4x4" : "other")
                    << "\n";
            }
        }
    }
    refl->Release();
}

int main(int argc, char** argv)
{
    if (argc < 2)
    {
        std::cerr << "usage: reflect_shaders.exe <ShadersDir> | reflect_shaders.exe --all <file.cso>\n";
        return 1;
    }

    if (std::string(argv[1]) == "--all" && argc >= 3)
    {
        DumpFile(argv[2], true);
        return 0;
    }

    for (auto& entry : fs::recursive_directory_iterator(argv[1]))
    {
        if (entry.path().extension() != ".cso") continue;
        DumpFile(entry.path(), false);
    }
    return 0;
}
