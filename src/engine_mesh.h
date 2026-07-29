#pragma once

#include <string>
#include <vector>

namespace nc {

struct EngineMeshVertex {
    float x = 0, y = 0, z = 0;
    float nx = 0, ny = 1, nz = 0;
    float u = 0, v = 0;
    float material = 0;
};

struct EngineMesh {
    std::vector<EngineMeshVertex> vertices;
    int materialCount = 0;

    bool load(const std::string& path);
    bool loadFbx(const std::string& path);
    bool loadObj(const std::string& path);
};

}  // namespace nc
