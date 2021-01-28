#include "voxelization.hpp"

#include "voxelio/filetype.hpp"
#include "voxelio/format/png.hpp"
#include "voxelio/format/qef.hpp"
#include "voxelio/format/vl32.hpp"
#include "voxelio/parse.hpp"
#include "voxelio/stream.hpp"
#include "voxelio/stringmanip.hpp"
#include "voxelio/voxelio.hpp"

#include <map>
#include <memory>
#include <set>
#include <vector>

// tinobj implementation must be included last because it is alos included by voxelization.hpp (but no implementation)
#define TINYOBJLOADER_IMPLEMENTATION 1
#include "3rd_party/tinyobj.hpp"

// MACROS ==============================================================================================================

// comment out when building
//#define OBJ2VOXEL_TEST
//#define OBJ2VOXEL_DUMP_STL

#ifndef OBJ2VOXEL_TEST
#define OBJ2VOXEL_MAIN_PARAMS int argc, char **argv
#else
#define OBJ2VOXEL_MAIN_PARAMS
#endif

// this macro helps us run the executable with hardcoded parameters without IDE help
#ifdef OBJ2VOXEL_TEST
#define OBJ2VOXEL_TEST_STRING(arg, def) def
#else
#define OBJ2VOXEL_TEST_STRING(arg, def) arg
#endif

// IMPLEMENTATION ======================================================================================================

namespace obj2voxels {
namespace {

using namespace voxelio;

#ifdef OBJ2VOXEL_DUMP_STL
static ByteArrayOutputStream globalDebugStl;
void writeVecAsBinary(OutputStream &stream, Vec3 v)
{
    stream.writeLittle<float>(v[0]);
    stream.writeLittle<float>(v[1]);
    stream.writeLittle<float>(v[2]);
}

void writeTriangleAsTextToDebugStl(const Triangle &triangle)
{
    Vec3 normal = triangle.normal();

    globalDebugStl.writeString("facet normal ");
    writeVecAsText(globalDebugStl, normal);
    globalDebugStl.write('\n');
    globalDebugStl.writeString("  outer loop\n");
    for (usize i = 0; i < 3; ++i) {
        globalDebugStl.writeString("    vertex ");
        writeVecAsText(globalDebugStl, triangle.vertex(i));
        globalDebugStl.write('\n');
    }
    globalDebugStl.writeString("  endloop\n");
    globalDebugStl.writeString("endfacet\n");
}

void writeTriangleAsBinaryToDebugStl(const Triangle &triangle)
{
    Vec3 normal = triangle.normal();
    normal /= length(normal);

    writeVecAsBinary(globalDebugStl, normal);
    writeVecAsBinary(globalDebugStl, triangle.vertex(0));
    writeVecAsBinary(globalDebugStl, triangle.vertex(1));
    writeVecAsBinary(globalDebugStl, triangle.vertex(2));
    globalDebugStl.writeLittle<u16>(0);
}
#endif

void findBoundaries(const std::vector<real_type> &points, Vec3 &outMin, Vec3 &outMax)
{
    Vec3 min = {points[0], points[1], points[2]};
    Vec3 max = min;

    for (size_t i = 0; i < points.size(); i += 3) {
        Vec3 p{points.data() + i};
        min = obj2voxels::min(min, p);
        max = obj2voxels::max(max, p);
    }

    outMin = min;
    outMax = max;
}

bool loadObj(const std::string &inFile,
             tinyobj::attrib_t &attrib,
             std::vector<tinyobj::shape_t> &shapes,
             std::vector<tinyobj::material_t> &materials)
{
    std::string warn;
    std::string err;

    bool tinyobjSuccess = tinyobj::LoadObj(&attrib, &shapes, &materials, &warn, &err, inFile.c_str());
    trim(warn);
    trim(err);

    if (not warn.empty()) {
        std::vector<std::string> warnings = splitAtDelimiter(warn, '\n');
        for (const std::string &warning : warnings) {
            VXIO_LOG(WARNING, "TinyOBJ: " + warning);
        }
    }
    if (not err.empty()) {
        VXIO_LOG(ERROR, "TinyOBJ: " + err);
    }

    return tinyobjSuccess;
}

Texture loadTexture(const std::string &name)
{
    std::optional<FileInputStream> stream = FileInputStream::open(name);
    if (not stream.has_value()) {
        VXIO_LOG(ERROR, "Failed to open texture file \"" + name + '\"');
        std::exit(1);
    }

    std::optional<Image> image = voxelio::png::decode(*stream, 4);
    if (not stream.has_value()) {
        VXIO_LOG(ERROR, "Failed to decode texture file \"" + name + '"');
        std::exit(1);
    }

    VXIO_LOG(INFO, "Loaded texture \"" + name + "\"");
    return std::move(*image);
}

std::map<Vec3u, WeightedColor> voxelize_obj(const std::string &inFile,
                                            const unsigned resolution,
                                            const ColorStrategy colorStrategy)
{
    // Load obj model
    tinyobj::attrib_t attrib;
    std::vector<tinyobj::shape_t> shapes;
    std::vector<tinyobj::material_t> materials;

    if (not loadObj(inFile, attrib, shapes, materials)) {
        std::exit(1);
    }
    if (attrib.vertices.empty()) {
        VXIO_LOG(WARNING, "Model has no vertices, aborting and writing empty voxel model");
        return {};
    }
    VXIO_LOG(INFO, "Loaded OBJ model with " + stringify(attrib.vertices.size() / 3) + " vertices");

    // Determine mesh to voxel space transform

    Vec3 meshMin, meshMax;
    findBoundaries(attrib.vertices, meshMin, meshMax);

    Voxelizer voxelizer{colorStrategy};
    voxelizer.initTransform(meshMin, meshMax, resolution);

    // Load textures
    for (tinyobj::material_t &material : materials) {
        std::string name = material.diffuse_texname;
        Texture tex = loadTexture(name);
        voxelizer.textures.emplace(std::move(name), std::move(tex));
    }
    VXIO_LOG(INFO, "Loaded all diffuse textures (" + stringifyDec(voxelizer.textures.size()) + ")");

    // Loop over shapes
    for (usize s = 0; s < shapes.size(); s++) {
        tinyobj::shape_t &shape = shapes[s];

        // Loop over faces(polygon)
        usize index_offset = 0;
        for (usize f = 0; f < shape.mesh.num_face_vertices.size(); f++) {
            usize vertexCount = shape.mesh.num_face_vertices[f];
            VXIO_DEBUG_ASSERT_EQ(vertexCount, 3);

            VisualTriangle triangle;
            bool hasTexCoords = true;

            // Loop over vertices in the face.
            for (usize v = 0; v < vertexCount; v++) {
                // access to vertex
                tinyobj::index_t idx = shape.mesh.indices[index_offset + v];
                if (idx.vertex_index < 0) {
                    VXIO_LOG(ERROR, "Vertex without vertex coordinates found");
                    exit(1);
                }
                Vec3 &vertex = triangle.v[v];
                vertex = Vec3{attrib.vertices.data() + 3 * idx.vertex_index};

                if (idx.texcoord_index >= 0) {
                    triangle.t[v] = Vec2{attrib.texcoords.data() + 2 * idx.texcoord_index};
                }
                else {
                    // Even if this value will never be used, it's not good practice to leave it unitialized.
                    // This could lead to accidental denormalized float operations which are expensive.
                    // Remove this line if we ever create a special case for meshes with no UV coordinates.
                    triangle.t[v] = {};
                    hasTexCoords = false;
                }
            }
            index_offset += vertexCount;

            int materialIndex = shape.mesh.material_ids[f];
            if (materialIndex < 0) {
                triangle.type = TriangleType::MATERIALLESS;
            }
            else if (hasTexCoords) {
                const std::string &textureName = materials[static_cast<usize>(materialIndex)].diffuse_texname;
                auto location = voxelizer.textures.find(textureName);
                if (location == voxelizer.textures.end()) {
                    VXIO_LOG(ERROR, "Face has invalid texture name \"" + textureName + '"');
                    std::exit(1);
                }
                triangle.texture = &location->second;
                triangle.type = TriangleType::TEXTURED;
            }
            else {
                Vec3 color{materials[static_cast<usize>(materialIndex)].diffuse};
                triangle.color = color.cast<float>();
                triangle.type = TriangleType::UNTEXTURED;
            }

            voxelizer.voxelize(triangle);
        }
    }
    VXIO_LOG(INFO, "Voxelized " + stringify(voxelizer.triangleCount) + " triangles");

    return std::move(voxelizer.voxels);
}

constexpr usize VOXEL_BUFFER_BYTE_SIZE = 8192;
constexpr usize VOXEL_BUFFER_32_SIZE = VOXEL_BUFFER_BYTE_SIZE / sizeof(Voxel32);

static Voxel32 VOXEL_BUFFER_32[VOXEL_BUFFER_32_SIZE];

AbstractListWriter *makeWriter(OutputStream &stream, FileType type)
{
    switch (type) {
    case FileType::QUBICLE_EXCHANGE: return new qef::Writer{stream};
    case FileType::VL32: return new vl32::Writer{stream};
    default: VXIO_ASSERT_UNREACHABLE();
    }
}

[[nodiscard]] int convert_map_voxelio(std::map<Vec3u, WeightedColor> &map,
                                      usize resolution,
                                      FileType outFormat,
                                      FileOutputStream &out)
{
    std::unique_ptr<AbstractListWriter> writer{makeWriter(out, outFormat)};
    writer->setCanvasDimensions(Vec<usize, 3>::filledWith(resolution).cast<u32>());

    const bool usePalette = requiresPalette(outFormat);

    if (usePalette) {
        Palette32 &palette = writer->palette();
        for (auto [pos, color] : map) {
            palette.insert(color.toColor32());
        }
    }

    usize voxelCount = 0;
    usize voxelIndex = 0;

    const auto flushBuffer = [&writer, &voxelIndex]() -> bool {
        voxelio::ResultCode writeResult = writer->write(VOXEL_BUFFER_32, voxelIndex);
        if (not voxelio::isGood(writeResult)) {
            VXIO_LOG(ERROR, "Flush/Write error: " + informativeNameOf(writeResult));
            return false;
        }
        voxelIndex = 0;
        return true;
    };

    for (auto [pos, weightedColor] : map) {
        Color32 color = weightedColor.toColor32();
        VOXEL_BUFFER_32[voxelIndex].pos = pos.cast<i32>();
        if (usePalette) {
            VOXEL_BUFFER_32[voxelIndex].index = writer->palette().indexOf(color);
        }
        else {
            VOXEL_BUFFER_32[voxelIndex].argb = color;
        }

        ++voxelCount;
        if (++voxelIndex == VOXEL_BUFFER_32_SIZE) {
            if (not flushBuffer()) {
                return 1;
            }
        }
    };

    VXIO_LOG(INFO, "Flushing remaining " + stringify(voxelIndex) + " voxels ...");
    VXIO_LOG(INFO, "All voxels written! (" + stringifyLargeInt(voxelCount) + " voxels)");

    bool finalSuccess = flushBuffer();
    if (not finalSuccess) {
        return 1;
    }

    VXIO_LOG(INFO, "Done!");
    return 0;
}

int main_impl(std::string inFile, std::string outFile, std::string resolutionStr, std::string colorStratStr)
{
    VXIO_LOG(INFO,
             "Converting \"" + inFile + "\" to \"" + outFile + "\" at resolution " + resolutionStr + " with strategy " +
                 colorStratStr);

    if (inFile.empty()) {
        VXIO_LOG(ERROR, "Input file path must not be empty");
        return 1;
    }

    std::optional<FileType> outType = detectFileType(outFile);
    if (not outType.has_value()) {
        VXIO_LOG(ERROR, "Can't detect file type of \"" + inFile + "\"");
        return 1;
    }

    std::optional<FileOutputStream> outStream = FileOutputStream::open(outFile);
    if (not outStream.has_value()) {
        VXIO_LOG(ERROR, "Failed to open \"" + outFile + "\" for write");
        return 1;
    }

    unsigned resolution;
    if (not voxelio::parseDec(resolutionStr, resolution)) {
        VXIO_LOG(ERROR, resolutionStr + " is not a valid resolution");
        return 1;
    }

    ColorStrategy colorStrategy;
    toUpperCase(colorStratStr);
    if (not parseColorStrategy(colorStratStr, colorStrategy)) {
        VXIO_LOG(ERROR, "Invalid color strategy \"" + colorStratStr + "\"");
        return 1;
    }

#ifdef OBJ2VOXEL_DUMP_STL
    globalTriangleDebugCallback = writeTriangleAsBinaryToDebugStl;
#endif

    std::map<Vec3u, WeightedColor> weightedVoxels = voxelize_obj(inFile, resolution, colorStrategy);

#ifdef OBJ2VOXEL_DUMP_STL
    u8 buffer[80]{};
    std::optional<FileOutputStream> stlDump = FileOutputStream::open("/tmp/obj2voxel_debug.stl");
    VXIO_DEBUG_ASSERT(stlDump.has_value());
    stlDump->write(buffer, sizeof(buffer));
    VXIO_DEBUG_ASSERT_EQ(globalDebugStl.size() % 50, 0);
    stlDump->writeLittle<u32>(static_cast<u32>(globalDebugStl.size() / 50));

    ByteArrayInputStream inStream{globalDebugStl};
    do {
        inStream.read(buffer, 50);
        if (inStream.eof()) break;
        stlDump->write(buffer, 50);
    } while (true);
#endif

    VXIO_LOG(INFO, "Model was voxelized, writing voxels to disk ...");
    bool success = convert_map_voxelio(weightedVoxels, resolution, *outType, *outStream);

    return not success;
}

}  // namespace
}  // namespace obj2voxels

int main(OBJ2VOXEL_MAIN_PARAMS)
{
#ifndef OBJ2VOXEL_TEST
    if (argc < 4) {
        VXIO_LOG(ERROR, "Usage: <in_file:path> <out_file:path> <resolution:uint> [color_strat:(max|blend)=max]");
        return 1;
    }
#endif
    if constexpr (voxelio::build::DEBUG) {
        voxelio::logLevel = voxelio::LogLevel::DEBUG;
        VXIO_LOG(DEBUG, "Running debug build");
    }

    std::string inFile = OBJ2VOXEL_TEST_STRING(argv[1], "/tmp/obj2voxel/in.obj");
    std::string outFile = OBJ2VOXEL_TEST_STRING(argv[2], "/tmp/obj2voxel/out.qef");
    std::string resolutionStr = OBJ2VOXEL_TEST_STRING(argv[3], "1024");
    std::string colorStratStr = OBJ2VOXEL_TEST_STRING(argc >= 5 ? argv[4] : "max", "max");

    obj2voxels::main_impl(std::move(inFile), std::move(outFile), std::move(resolutionStr), std::move(colorStratStr));
}
