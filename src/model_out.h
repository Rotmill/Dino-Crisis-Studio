// 08.10.2026 (player: "when we export - glTF, FBX and OBJ to choose from, the mesh and its animation"): the export's model
// in FBX (binary 7.4: meshes, materials and their textures, the skeleton with its skin, every animation as its own stack)
// or OBJ + MTL (the meshes in the bind pose with materials and vertex colours; OBJ keeps no skeleton and no animation),
// made from the very glTF the export builds - one place decides what goes out. Units and axes as in the glTF: metres, Y up,
// right-handed - what the game reads from mods (ufbx, dc_meshrep.cpp).
#pragma once
#include "gltf.h"

namespace dc {

enum class ModelFormat { Gltf = 0, Fbx = 1, Obj = 2 };

const char* model_format_name(ModelFormat f);                  // "glTF", "FBX", "OBJ"
const char* model_format_ext(ModelFormat f);                   // ".gltf", ".fbx", ".obj"
bool model_format_parse(const std::string& s, ModelFormat& f);  // "gltf" / "fbx" / "obj", any case

// image URIs of g are relative to the folder of the output file (as the export writes them)
bool write_fbx(const Gltf& g, const fs::path& path);
bool write_obj(const Gltf& g, const fs::path& path);           // + <name>.mtl next to it

}  // namespace dc
