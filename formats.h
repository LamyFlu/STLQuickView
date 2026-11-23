#pragma once

// Model loading for the viewer: STL, 3MF, OBJ and PLY. Only geometry is loaded; colours,
// materials and textures are detected (for an on-screen notice) but never read.

#include "mesh.h"
#include <span>
#include <string>
#include <vector>

struct IStream;

namespace model {
struct Part {
    std::wstring name;
    size_t first{}, count{};   // triangle range in Model::mesh.triangles
    stl::Point min{}, max{};   // bounds in normalised model space
    double size[3]{};          // X/Y/Z extent in file units
};

struct Model {
    stl::Mesh mesh;
    std::vector<Part> parts;   // at least one
    bool hasColor{};           // the file carries colour / material / texture data
    std::wstring format;       // L"STL", L"3MF", L"OBJ" or L"PLY"
    std::wstring unit;         // e.g. L" mm" when the file states its unit, otherwise empty
};

// Picks the format from the extension, falling back to the file signature.
Model Load(const std::wstring& path, std::span<const char> bytes);
std::string ReadFirstPlateImage(IStream& stream);

// File-dialog filter and the extensions the viewer accepts.
inline constexpr wchar_t DialogFilter[] =
    L"3D 模型 (*.stl;*.3mf;*.obj;*.ply)\0*.stl;*.3mf;*.obj;*.ply\0"
    L"STL (*.stl)\0*.stl\0" L"3MF (*.3mf)\0*.3mf\0" L"OBJ (*.obj)\0*.obj\0" L"PLY (*.ply)\0*.ply\0"
    L"所有檔案 (*.*)\0*.*\0";
}
