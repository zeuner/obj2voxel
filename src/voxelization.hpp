#ifndef OBJ2VOXEL_VOXELIZATION_HPP
#define OBJ2VOXEL_VOXELIZATION_HPP

#include "triangle.hpp"
#include "util.hpp"

#include "voxelio/color.hpp"
#include "voxelio/log.hpp"
#include "voxelio/vec.hpp"

#include <map>

namespace obj2voxel {

using namespace voxelio;

// SIMPLE STRUCTS AND TYPEDEFS =========================================================================================

/// A function called on debug builds which can be used to dump triangles to an STL file and such.
extern void (*globalTriangleDebugCallback)(Triangle);

/// An enum which describes the strategy for coloring in voxels from triangles.
enum class ColorStrategy {
    /// For the maximum strategy, the triangle with the greatest area is chosen as the color.
    MAX,
    /// For the blending strategy, the voxel color is determined by blending the triangle colors using their areas
    /// within the voxel as weights.
    BLEND
};

constexpr const char *nameOf(ColorStrategy strategy)
{
    return strategy == ColorStrategy::MAX ? "MAX" : "BLEND";
}

/// Parses the color strategy. This function is case sensitive.
inline bool parseColorStrategy(const std::string &str, ColorStrategy &out)
{
    if (str == "MAX") {
        out = ColorStrategy::MAX;
        return true;
    }
    if (str == "BLEND") {
        out = ColorStrategy::BLEND;
        return true;
    }
    return false;
}

/// Throwaway class which manages all necessary data structures for voxelization and simplifies the procedure from the
/// caller's side to just using voxelize(triangle).
///
/// Before the Voxelizer can be used, a transform from model space must be initialized with initTransform(...);
class Voxelizer {
    static constexpr real_type ANTI_BLEED = 0.5f;

public:
    static AffineTransform computeTransform(Vec3 min, Vec3 max, unsigned resolution, Vec3u permutation);

private:
    std::vector<TexturedTriangle> buffers[3]{};
    VoxelMap<WeightedUv> uvBuffer;
    VoxelMap<WeightedColor> voxels_;
    WeightedCombineFunction<Vec3f> combineFunction;

public:
    Voxelizer(ColorStrategy colorStrategy);

    Voxelizer(const Voxelizer &) = delete;
    Voxelizer(Voxelizer &&) = default;

    void voxelize(const VisualTriangle &triangle, Vec3u32 min, Vec3u32 max);

    void mergeResults(VoxelMap<WeightedColor> &out)
    {
        merge(out, voxels_);
    }

    void merge(VoxelMap<WeightedColor> &target, VoxelMap<WeightedColor> &source);

    /**
     * @brief Scales down the voxels of the voxelizer to half the original resolution.
     */
    void downscale();

    VoxelMap<WeightedColor> &voxels()
    {
        return voxels_;
    }

private:
    /**
     * @brief Voxelizes a triangle.
     *
     * Among the parameters is an array of three buffers.
     * This array must be notnull.
     * The contents of the buffers are cleared by the callee, this parameter is only used so that allocation of new
     * vectors can be avoided for each triangle.
     *
     * @param triangle the input triangle to be voxelized
     * @param buffers three buffers which are used for intermediate operations
     * @param out the output map of voxel locations to weighted colors
     */
    void voxelizeTriangleToUvBuffer(const VisualTriangle &inputTriangle, Vec3u32 min, Vec3u32 max);

    void consumeUvBuffer(const VisualTriangle &inputTriangle);
};

}  // namespace obj2voxel

#endif  // VOXELIZATION_HPP
