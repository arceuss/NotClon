#include "engine_mesh.h"

#include "ufbx/ufbx.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <fstream>
#include <map>
#include <sstream>

namespace nc {
namespace {

struct Vec3 { float x, y, z; };
struct Vec2 { float x, y; };
struct Ref { int vertex = 0, texture = 0, normal = 0; };

int objIndex(int index, int size) {
    return index > 0 ? index - 1 : size + index;
}

Ref parseRef(const std::string& token) {
    Ref out;
    const size_t first = token.find('/');
    if (first == std::string::npos) {
        out.vertex = std::stoi(token);
        return out;
    }
    out.vertex = std::stoi(token.substr(0, first));
    const size_t second = token.find('/', first + 1);
    if (second == std::string::npos) {
        if (first + 1 < token.size())
            out.texture = std::stoi(token.substr(first + 1));
    } else {
        if (second > first + 1)
            out.texture = std::stoi(token.substr(first + 1,
                                                  second - first - 1));
        if (second + 1 < token.size())
            out.normal = std::stoi(token.substr(second + 1));
    }
    return out;
}

Vec3 faceNormal(const Vec3& a, const Vec3& b, const Vec3& c) {
    const float ux = b.x - a.x, uy = b.y - a.y, uz = b.z - a.z;
    const float vx = c.x - a.x, vy = c.y - a.y, vz = c.z - a.z;
    Vec3 n{uy * vz - uz * vy, uz * vx - ux * vz, ux * vy - uy * vx};
    const float length = std::sqrt(n.x * n.x + n.y * n.y + n.z * n.z);
    if (length > 0.0f) {
        n.x /= length; n.y /= length; n.z /= length;
    } else {
        n = {0, 1, 0};
    }
    return n;
}

Vec3 transformed(const ufbx_matrix& matrix, ufbx_vec3 value) {
    const ufbx_vec3 result = ufbx_transform_position(&matrix, value);
    return {float(result.x), float(result.y), float(result.z)};
}

Vec3 transformedNormal(const ufbx_matrix& matrix, ufbx_vec3 value,
                       const Vec3& fallback) {
    const ufbx_vec3 result = ufbx_vec3_normalize(
        ufbx_transform_direction(&matrix, value));
    if (!std::isfinite(result.x) || !std::isfinite(result.y) ||
        !std::isfinite(result.z)) {
        return fallback;
    }
    const float length = std::sqrt(float(result.x * result.x +
                                         result.y * result.y +
                                         result.z * result.z));
    if (length < 0.5f) return fallback;
    return {float(result.x), float(result.y), float(result.z)};
}

ufbx_matrix relativeGeometryTransform(const ufbx_node* node,
                                      ufbx_real unitScale) {
    (void)unitScale;
    const ufbx_node* root = node;
    while (root->parent && !root->parent->is_root) root = root->parent;
    const ufbx_matrix inverseRoot = ufbx_matrix_invert(&root->node_to_world);
    ufbx_matrix matrix = ufbx_matrix_mul(&inverseRoot,
                                         &node->geometry_to_world);
    // NO unit scaling of the translation: vertices stay in raw file units
    // (the callers' instance scales carry the x0.01 file-scale fold), so a
    // child node's translation must stay in file units too. Scaling it here
    // shrank the fret colour shell's +0.302 offset to 0.003 and let the base
    // plate swallow the key caps. Single-node meshes are identity either way.
    return matrix;
}

bool hasExtension(const std::string& path, const char* extension) {
    const size_t length = std::char_traits<char>::length(extension);
    if (path.size() < length) return false;
    for (size_t i = 0; i < length; ++i) {
        const unsigned char a = static_cast<unsigned char>(
            path[path.size() - length + i]);
        const unsigned char b = static_cast<unsigned char>(extension[i]);
        if (std::tolower(a) != std::tolower(b)) return false;
    }
    return true;
}

}  // namespace

bool EngineMesh::load(const std::string& path) {
    return hasExtension(path, ".fbx") ? loadFbx(path) : loadObj(path);
}

bool EngineMesh::loadFbx(const std::string& path) {
    vertices.clear();
    materialCount = 0;

    ufbx_load_opts options{};
    options.generate_missing_normals = true;
    options.ignore_animation = true;
    options.ignore_embedded = true;
    ufbx_error error{};
    ufbx_scene* scene = ufbx_load_file(path.c_str(), &options, &error);
    if (!scene) return false;

    int materialBase = 0;
    auto appendNode = [&](const ufbx_node* node) {
        const ufbx_mesh* mesh = node->mesh;
        if (!mesh || !mesh->vertex_position.exists) return;

        int localMaterialCount = 0;
        for (size_t faceIndex = 0; faceIndex < mesh->face_material.count;
             ++faceIndex) {
            localMaterialCount = std::max(
                localMaterialCount,
                int(mesh->face_material.data[faceIndex] + 1));
        }
        if (mesh->faces.count != 0)
            localMaterialCount = std::max(localMaterialCount, 1);
        // Unity's FBX importer creates submeshes in the order materials are
        // FIRST USED by a face, not in the node's material-connection order
        // (which is what ufbx's face_material indexes). Remap so a vertex's
        // material index always means "Unity submesh index" -- the index the
        // theme prefab's m_Materials array is written against. The two orders
        // coincide for every engine FBX except the three Rectangular note
        // meshes; the fret matching under BOTH orders is what let the
        // connection-order assumption survive as long as it did.
        std::vector<int> unitySlot(size_t(localMaterialCount), -1);
        {
            int next = 0;
            for (size_t faceIndex = 0; faceIndex < mesh->faces.count;
                 ++faceIndex) {
                const uint32_t fm =
                    faceIndex < mesh->face_material.count
                        ? mesh->face_material.data[faceIndex]
                        : 0;
                const uint32_t slot = fm == UFBX_NO_INDEX ? 0 : fm;
                if (slot < unitySlot.size() && unitySlot[slot] < 0)
                    unitySlot[slot] = next++;
            }
            for (int& s : unitySlot)
                if (s < 0) s = next++; // unused slots keep a stable tail
        }

        const ufbx_matrix geometryMatrix = relativeGeometryTransform(
            node, scene->settings.unit_meters);
        const ufbx_matrix normalMatrix =
            ufbx_matrix_for_normals(&geometryMatrix);
        std::vector<uint32_t> triangleIndices(
            std::max<size_t>(mesh->max_face_triangles * 3, 3));

        for (size_t faceIndex = 0; faceIndex < mesh->faces.count; ++faceIndex) {
            const ufbx_face face = mesh->faces.data[faceIndex];
            const uint32_t triangleCount = ufbx_triangulate_face(
                triangleIndices.data(), triangleIndices.size(), mesh, face);
            const uint32_t faceMaterial =
                faceIndex < mesh->face_material.count
                    ? mesh->face_material.data[faceIndex]
                    : 0;
            // Faces with no material assignment come back as UFBX_NO_INDEX;
            // Unity assigns them to submesh 0.
            const uint32_t material =
                faceMaterial == UFBX_NO_INDEX ? 0 : faceMaterial;
            const int outputMaterial = materialBase + unitySlot[material];

            for (uint32_t triangle = 0; triangle < triangleCount; ++triangle) {
                const uint32_t* indices = &triangleIndices[triangle * 3];
                Vec3 positions[3];
                for (int corner = 0; corner < 3; ++corner) {
                    const ufbx_vec3 p = ufbx_get_vertex_vec3(
                        &mesh->vertex_position, indices[corner]);
                    positions[corner] = transformed(geometryMatrix, p);
                }
                const Vec3 fallback = faceNormal(positions[0], positions[1],
                                                 positions[2]);

                for (int corner = 0; corner < 3; ++corner) {
                    Vec3 normal = fallback;
                    if (mesh->vertex_normal.exists) {
                        const ufbx_vec3 n = ufbx_get_vertex_vec3(
                            &mesh->vertex_normal, indices[corner]);
                        normal = transformedNormal(normalMatrix, n, fallback);
                    }
                    ufbx_vec2 uv{};
                    if (mesh->vertex_uv.exists)
                        uv = ufbx_get_vertex_vec2(&mesh->vertex_uv,
                                                 indices[corner]);
                    vertices.push_back({positions[corner].x,
                                        positions[corner].y,
                                        positions[corner].z,
                                        normal.x, normal.y, normal.z,
                                        float(uv.x), float(uv.y),
                                        float(outputMaterial)});
                }
            }
        }
        materialBase += localMaterialCount;
    };

    // Mesh order is FBX Geometry order, which is also Unity's imported
    // submesh order. Each instance still gets its own hierarchy transform.
    for (size_t meshIndex = 0; meshIndex < scene->meshes.count; ++meshIndex) {
        const ufbx_mesh* mesh = scene->meshes.data[meshIndex];
        for (size_t instanceIndex = 0; instanceIndex < mesh->instances.count;
             ++instanceIndex) {
            appendNode(mesh->instances.data[instanceIndex]);
        }
    }

    materialCount = materialBase;
    ufbx_free_scene(scene);
    if (!vertices.empty()) return true;
    materialCount = 0;
    return false;
}

bool EngineMesh::loadObj(const std::string& path) {
    std::ifstream input(path);
    if (!input) return false;

    vertices.clear();
    materialCount = 0;
    std::vector<Vec3> positions, normals;
    std::vector<Vec2> textureCoords;
    std::map<std::string, int> materials;
    int material = 0;
    std::string line;
    while (std::getline(input, line)) {
        std::istringstream stream(line);
        std::string kind;
        stream >> kind;
        if (kind == "v") {
            Vec3 p{}; stream >> p.x >> p.y >> p.z;
            positions.push_back(p);
        } else if (kind == "vn") {
            Vec3 n{}; stream >> n.x >> n.y >> n.z;
            normals.push_back(n);
        } else if (kind == "vt") {
            Vec2 uv{}; stream >> uv.x >> uv.y;
            textureCoords.push_back(uv);
        } else if (kind == "usemtl") {
            std::string name; stream >> name;
            auto found = materials.find(name);
            if (found == materials.end()) {
                material = int(materials.size());
                const std::string prefix = "material_";
                if (name.compare(0, prefix.size(), prefix) == 0) {
                    try { material = std::stoi(name.substr(prefix.size())); }
                    catch (...) {}
                }
                materials[name] = material;
            } else {
                material = found->second;
            }
        } else if (kind == "f") {
            std::vector<Ref> face;
            std::string token;
            while (stream >> token) face.push_back(parseRef(token));
            for (size_t triangle = 1; triangle + 1 < face.size(); ++triangle) {
                const Ref refs[3] = {face[0], face[triangle], face[triangle + 1]};
                Vec3 p[3];
                for (int i = 0; i < 3; ++i)
                    p[i] = positions[objIndex(refs[i].vertex, int(positions.size()))];
                const Vec3 fallback = faceNormal(p[0], p[1], p[2]);
                for (int i = 0; i < 3; ++i) {
                    Vec3 n = fallback;
                    if (refs[i].normal != 0)
                        n = normals[objIndex(refs[i].normal, int(normals.size()))];
                    Vec2 uv{};
                    if (refs[i].texture != 0)
                        uv = textureCoords[objIndex(refs[i].texture,
                                                    int(textureCoords.size()))];
                    vertices.push_back({p[i].x, p[i].y, p[i].z,
                                        n.x, n.y, n.z, uv.x, uv.y,
                                        float(material)});
                }
            }
        }
    }
    for (const auto& entry : materials)
        materialCount = std::max(materialCount, entry.second + 1);
    return !vertices.empty();
}

}  // namespace nc
