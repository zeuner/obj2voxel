#include "filevoxelization.hpp"

#include "io.hpp"
#include "threading.hpp"

#include "voxelio/stringify.hpp"

#include <ostream>
#include <thread>

namespace obj2voxel {

namespace {

enum class CommandType {
    /// Instructs a worker thread to voxelize a triangle.
    VOXELIZE_TRIANGLE,
    /// Instructs a worker thread to merge two voxel maps and clear the source map.
    MERGE_MAPS,
    /// Instructs a worker to exit.
    EXIT
};

struct WorkerCommand {
    static WorkerCommand exit()
    {
        return {};
    }

    struct MergeMaps {
        VoxelMap<WeightedColor> *target;
        VoxelMap<WeightedColor> *source;
    };

    CommandType type;

    union {
        std::nullptr_t nothing;
        VisualTriangle triangle;
        MergeMaps mergeMaps;
    };

    constexpr explicit WorkerCommand(VisualTriangle triangle) : type{CommandType::VOXELIZE_TRIANGLE}, triangle{triangle}
    {
    }

    constexpr WorkerCommand(VoxelMap<WeightedColor> *target, VoxelMap<WeightedColor> *source)
        : type{CommandType::MERGE_MAPS}, mergeMaps{target, source}
    {
        VXIO_ASSERT_NE(target, source);
    }

    constexpr WorkerCommand() : type{CommandType::EXIT}, nothing{nullptr} {}
};

/// Thread-safe queue of worker commands.
class CommandQueue {
private:
    async::RingBuffer<WorkerCommand, 128> buffer;
    async::Counter<uintmax_t> commandCounter;

public:
    /// Signals completion of one command. To be called by workers.
    void complete()
    {
        --commandCounter;
    }

    /// Receives one command. To be called by workers.
    WorkerCommand receive()
    {
        return buffer.pop();
    }

    /// Issues a command to workers. To be called by main thread.
    void issue(WorkerCommand command)
    {
        ++commandCounter;
        buffer.push(std::move(command));
    }

    /// Waits until all commands have been completed.
    void waitForCompletion()
    {
        commandCounter.waitUntilZero();
    }
};

class WorkerThread final
    : public Voxelizer
    , public std::thread {
public:
// we ignore warnings because emplace() calls below don't detect the usage of the constructor
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wunused-member-function"
    WorkerThread(AffineTransform meshTransform, ColorStrategy colorStrat, CommandQueue &queue)
        : Voxelizer{meshTransform, colorStrat}, std::thread{&WorkerThread::run, this, std::ref(queue)}
    {
    }

    WorkerThread(const WorkerThread &) = delete;
    WorkerThread(WorkerThread &&) = default;
#pragma clang diagnostic pop

private:
    void run(CommandQueue &queue);
};

void WorkerThread::run(CommandQueue &queue)
{
    VXIO_LOG(DEBUG, "VoxelizerThread " + stringify(get_id()) + " started");
    bool looping = true;
    do {
        WorkerCommand command = queue.receive();
        switch (command.type) {
        case CommandType::VOXELIZE_TRIANGLE: {
            this->voxelize(std::move(command.triangle));
            break;
        }

        case CommandType::MERGE_MAPS: {
            VXIO_DEBUG_ASSERT_NOTNULL(command.mergeMaps.target);
            VXIO_DEBUG_ASSERT_NOTNULL(command.mergeMaps.source);
            this->merge(*command.mergeMaps.target, *command.mergeMaps.source);
            command.mergeMaps.source->clear();
            break;
        }

        case CommandType::EXIT: {
            looping = false;
            break;
        }
        }
        queue.complete();
    } while (looping);
}

VoxelMap<WeightedColor> mergeVoxelMaps(std::vector<WorkerThread> &threads, CommandQueue &queue)
{
    while (true) {
        VoxelMap<WeightedColor> *mergeTarget = nullptr, *mergeSource = nullptr;
        usize commandsIssued = 0;

        for (auto &voxelizer : threads) {
            if (voxelizer.voxels().empty()) {
                continue;
            }

            if (mergeTarget == nullptr) {
                mergeTarget = &voxelizer.voxels();
                continue;
            }
            else {
                mergeSource = &voxelizer.voxels();

                if (mergeTarget->size() < mergeSource->size()) {
                    std::swap(mergeTarget, mergeSource);
                }
                ++commandsIssued;
                queue.issue(WorkerCommand{mergeTarget, mergeSource});
                mergeTarget = mergeSource = nullptr;
            }
        }

        if (commandsIssued == 0) {
            return mergeTarget == nullptr ? VoxelMap<WeightedColor>() : std::move(*mergeTarget);
        }

        queue.waitForCompletion();
    }
}

void joinWorkers(std::vector<WorkerThread> &threads, CommandQueue &queue)
{
    for (usize i = 0; i < threads.size(); ++i) {
        queue.issue(WorkerCommand::exit());
    }
    for (auto &worker : threads) {
        worker.join();
    }
}

template <typename Float>
void findBoundaries(const float data[], usize vertexCount, Vec<Float, 3> &outMin, Vec<Float, 3> &outMax)
{
    Vec<Float, 3> min = {data[0], data[1], data[2]};
    Vec<Float, 3> max = min;

    usize limit = vertexCount * 3;
    for (size_t i = 0; i < limit; i += 3) {
        Vec<Float, 3> p{data + i};
        min = obj2voxel::min(min, p);
        max = obj2voxel::max(max, p);
    }

    outMin = min;
    outMax = max;
}

}  // namespace

bool voxelize(VoxelizationArgs args, ITriangleStream &stream, VoxelSink &sink)
{
    if (stream.vertexCount() == 0) {
        VXIO_LOG(WARNING, "Model has no vertices, aborting and writing empty voxel model");
        sink.flush();
        return true;
    }
    VXIO_LOG(INFO, "Loaded model with " + stringifyLargeInt(stream.vertexCount()) + " vertices");

    CommandQueue queue;

    usize totalTriangleCount = 0;
    usize threadCount = std::thread::hardware_concurrency();

    Vec3 meshMin, meshMax;
    findBoundaries(stream.vertexBegin(), stream.vertexCount(), meshMin, meshMax);
    AffineTransform meshTransform = Voxelizer::computeTransform(meshMin, meshMax, args.resolution, args.permutation);

    std::vector<WorkerThread> workers;
    workers.reserve(threadCount);
    for (usize i = 0; i < threadCount; ++i) {
        auto &voxelizer = workers.emplace_back(meshTransform, args.colorStrategy, queue);
        VXIO_ASSERT(voxelizer.joinable());
    }

    // Loop over shapes
    while (stream.hasNext()) {
        ++totalTriangleCount;
        queue.issue(WorkerCommand{stream.next()});
    }
    VXIO_LOG(DEBUG, "Pushed all triangles, waiting until buffer is empty");

    queue.waitForCompletion();

    VXIO_LOG(INFO, "Voxelized " + stringifyLargeInt(totalTriangleCount) + " triangles, merging results ...");

    VoxelMap<WeightedColor> result = mergeVoxelMaps(workers, queue);

    joinWorkers(workers, queue);

    if (args.downscale) {
        VXIO_LOG(INFO,
                 "Downscaling from " + stringifyLargeInt(args.resolution) + " to output resolution " +
                     stringifyLargeInt(args.resolution / 2) + " ...")
        result = downscale(result, args.colorStrategy);
    }

    VXIO_LOG(INFO, "Writing voxels to disk ...");

    for (auto &[index, color] : result) {
        if (not sink.canWrite()) {
            VXIO_LOG(ERROR, "No more voxels can be written after I/O error, aborting ...");
            return false;
        }

        Voxel32 voxel;
        voxel.pos = result.posOf(index).cast<i32>();
        voxel.argb = Color32{color.value.x(), color.value.y(), color.value.z()};

        sink.write(voxel);
    }

    return true;
}

}  // namespace obj2voxel
