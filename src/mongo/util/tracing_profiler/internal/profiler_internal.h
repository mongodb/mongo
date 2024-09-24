/**
 *    Copyright (C) 2024-present MongoDB, Inc.
 *
 *    This program is free software: you can redistribute it and/or modify
 *    it under the terms of the Server Side Public License, version 1,
 *    as published by MongoDB, Inc.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    Server Side Public License for more details.
 *
 *    You should have received a copy of the Server Side Public License
 *    along with this program. If not, see
 *    <http://www.mongodb.com/licensing/server-side-public-license>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the Server Side Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */

#pragma once

#include <absl/container/flat_hash_map.h>
#include <algorithm>
#include <boost/align/aligned_alloc.hpp>
#include <boost/align/aligned_allocator.hpp>
#include <boost/align/aligned_delete.hpp>
#include <mutex>
#include <shared_mutex>

#include "mongo/base/init.h"
#include "mongo/config.h"
#include "mongo/stdx/mutex.h"
#include "mongo/stdx/unordered_set.h"
#include "mongo/util/aligned.h"
#include "mongo/util/fixed_string.h"
#include "mongo/util/overloaded_visitor.h"
#include "mongo/util/tracing_profiler/internal/cycleclock.h"

namespace mongo::tracing_profiler::internal {

/**
 * A tag id representing a measurement span.
 */
typedef uint32_t TagId;

/**
A node id representing a node in the execution tree. Execution tree is formed of nested spans.
*/
typedef uint32_t NodeId;

#if !MONGO_CONFIG_USE_TRACING_PROFILER
static void failUnconfigured() {
    invariant(false,
              "ProfilerService is not enabled. Use --use-tracing-profiler=on build option to "
              "enable.");
}
#endif

struct TagIdHash {
    MONGO_COMPILER_ALWAYS_INLINE size_t operator()(TagId x) const noexcept {
        return x;
    }
};

/**
 * A profiled tag that combines tags id and it's string representation.
 */
struct ProfilerTag {
    TagId id;
    const char* name;
};

/**
 * Collection of all profiler tags in the system.
 */
class ProfilerTags {
public:
    static ProfilerTags* get();

    ProfilerTag getOrInsertTag(const char* name);

    MONGO_COMPILER_ALWAYS_INLINE const std::vector<ProfilerTag>& tags() const {
        return _tags;
    }

private:
    std::vector<ProfilerTag> _tags;
    absl::flat_hash_map<std::string_view, TagId> _tagIdByName;
};

/**
 * A utility that performs compile time binding of a template string argument to a sequential and
 * unique tags id.
 */
template <FixedString name>
struct ProfilerTagSource {

    static TagId id;

    MONGO_COMPILER_ALWAYS_INLINE static uint32_t getId() {
        return id;
    }

    MONGO_COMPILER_ALWAYS_INLINE static ProfilerTag getTag() {
        return ProfilerTag{id, name};
    }
};
template <FixedString name>
TagId ProfilerTagSource<name>::id = ProfilerTags::get()->getOrInsertTag(name).id;

/**
 * Represents a call tree composed of nested spans. Each span has sequential unique NodeId
 * that is associated with some TagId. The same TagId may appear in multiple places in the call
 * tree.
 */
class CallTree {
public:
    template <typename K, typename V, size_t N>
    struct InlinedMap {
        struct Entry {
            K tagId;
            V nodeId;
        };
        static constexpr size_t MaxSize = N;

        InlinedMap() : size(0) {}

        size_t size;
        Entry data[N];
    };

    typedef InlinedMap<TagId, NodeId, 4> ChildrenInlinedMap;
    typedef absl::flat_hash_map<
        TagId,
        NodeId,
        TagIdHash,
        std::equal_to<TagId>,
        boost::alignment::aligned_allocator<std::pair<TagId, NodeId>,
                                            stdx::hardware_destructive_interference_size>>
        ChildrenHashMap;

    /**
     * Used by a node to mapping of TagId to child NodeIds.
     * Small children map is represented using inlined array, large children maps are
     * represented using hash map.
     */
    class ChildrenMap : public std::variant<ChildrenInlinedMap, ChildrenHashMap> {
    public:
        using std::variant<ChildrenInlinedMap, ChildrenHashMap>::variant;
        using std::variant<ChildrenInlinedMap, ChildrenHashMap>::operator=;

        size_t size() const;
        void foreach (std::function<void(TagId, NodeId)>&& fn) const;
    };

    /**
     * A node in a call tree.
     */
    struct Node {
        Node(NodeId parentId, TagId tagId)
            : parentId(parentId), tagId(tagId), children(ChildrenInlinedMap()) {}

        // Id of the parent node.
        NodeId parentId;

        // TagId associated with this node.
        TagId tagId;

        // Collection of children nodes and their tag ids.
        ChildrenMap children;

        // int64_t startCycles;
    };

    typedef std::vector<
        Node,
        boost::alignment::aligned_allocator<Node, stdx::hardware_destructive_interference_size>>
        Nodes;

public:
    CallTree() {}

    MONGO_COMPILER_ALWAYS_INLINE const Nodes& nodes() const {
        return _nodes;
    }

    inline NodeId tryGetChildNode(NodeId parentId, TagId tagId) const {
        {
            if (_nodes.empty())
                return false;

            return std::visit(
                OverloadedVisitor{[tagId](const CallTree::ChildrenInlinedMap& map) -> NodeId {
                                      for (size_t i = 0; i < map.size; i++) {
                                          if (map.data[i].tagId == tagId)
                                              return map.data[i].nodeId;
                                      }
                                      return 0;
                                  },
                                  [tagId](const CallTree::ChildrenHashMap& map) -> NodeId {
                                      auto it = map.find(tagId);
                                      return it != map.end() ? it->second : 0;
                                  }},
                _nodes[parentId].children);
        }
    }

    NodeId getOrInsertChildNode(NodeId parentId, TagId tagId);

private:
    Nodes _nodes;
};

/**
 * Represents all metrics associated with the call tree.
 */
class CallMetrics {
public:
    /**
     * Represents metrics associated with a call tree node.
     */
    struct NodeMetrics {
        NodeMetrics() : cycles(0), count(0) {}
        NodeMetrics(int64_t cycles, int64_t count) : cycles(cycles), count(count) {}
        NodeMetrics(const NodeMetrics& other)
            : cycles(other.cycles.load()), count(other.count.load()) {}

        // Total number of cycles spent in this node.
        std::atomic_int64_t cycles;

        // Total number of entry-exit pairs for this node.
        std::atomic_int64_t count;
    };

public:
    CallMetrics() = default;
    CallMetrics(CallTree&& callTree, std::vector<NodeMetrics>&& nodeMetrics);
    static CallMetrics atomicCopy(const CallMetrics& other);

    MONGO_COMPILER_ALWAYS_INLINE CallTree& callTree() {
        return _callTree;
    }

    MONGO_COMPILER_ALWAYS_INLINE const CallTree& callTree() const {
        return _callTree;
    }

    MONGO_COMPILER_ALWAYS_INLINE std::vector<NodeMetrics>& nodeMetrics() {
        return _nodeMetrics;
    }

    MONGO_COMPILER_ALWAYS_INLINE const std::vector<NodeMetrics>& nodeMetrics() const {
        return _nodeMetrics;
    }

    MONGO_COMPILER_ALWAYS_INLINE NodeId tryGetChildNode(NodeId parentId, TagId tagId) const {
        return _callTree.tryGetChildNode(parentId, tagId);
    }

    NodeId getOrInsertChildNode(NodeId parentId, TagId tagId);

    void append(const CallMetrics& other);

private:
    void appendVisit(const CallMetrics& other, NodeId thisId, NodeId otherId);

private:
    CallTree _callTree;
    std::vector<NodeMetrics> _nodeMetrics;
};

/**
 * Represents all profiler metrics, including both collected metrics and computed metrics.
 */
class ProfilerMetrics {
public:
    /**
     * Represents estimated overhead that the profiler measurements per span introduce.
     */
    struct OverheadCycles {
        // Estimated cycles it takes to measure a span where parent node is using inlined map.
        double narrowNodeOverheadCycles;

        // Estimated cycles it takes to measure a span where parent node is using hash map.
        double wideNodeOverheadCycles;
    };

    /**
     * Computed metrics per node in call tree.
     */
    struct ComputedMetrics {
        ComputedMetrics()
            : totalOverheadCycles(0), nodeOverheadCycles(0), netCycles(0), exclusiveCycles(0) {}

        // Estimated count of profiler overhead cycles for the node and it's subtree.
        int64_t totalOverheadCycles;

        // Estimated count of profiler overhead cycles estimated for the node excluding it's
        // children.
        int64_t nodeOverheadCycles;

        // Estimated count of cycles for the node and it's subtee that excluses the profiler
        // overhead.
        int64_t netCycles;

        // Estimated count of cycles for the node exclusing it's children and exluding profiler
        // overhead.
        int64_t exclusiveCycles;
    };

    class ComputedMetricsBuilder {
    public:
        ComputedMetricsBuilder(CallMetrics& callMetrics, OverheadCycles& overheadCycles);
        std::vector<ComputedMetrics> build();

    private:
        void visit(NodeId nodeId);

    private:
        CallMetrics& _callMetrics;
        OverheadCycles& _overheadCycles;

        std::vector<ProfilerMetrics::ComputedMetrics> _computedMetrics;
    };

    ProfilerMetrics(CallMetrics&& callMetrics,
                    std::vector<ComputedMetrics>&& computedMetrics,
                    const ProfilerTags* profilerTags,
                    double frequency);

    void toBson(BSONObjBuilder* builder) const;

private:
    const CallMetrics _callMetrics;
    const std::vector<ComputedMetrics> _computedMetrics;
    const ProfilerTags* _profilerTags;
    const double _frequency;
};

/**
 * A tracing profiler that collects real-time metrics for explicitly annotated call tree.
 * This class supports concurrency and is thread-safe.
 */
class Profiler {
public:
    /**
     * A state that represents an active span (that has been entered).
     */
    struct SpanState {
        NodeId nodeId;
        int64_t startCycles;
    };

    /**
     * Shard of profiler state that is local to a specific thread.
     * This class supports concurrent reads, but not concurrent updates.
     *
     * The concurrency safety is achieved by only allowing updates from the thread that
     * this shard is associated with. External threads are only allowed to perform reads.
     */
    class ThreadLocalShard {
    public:
        ThreadLocalShard(Profiler* profiler);
        ~ThreadLocalShard();

        /**
         * Enters the span with given tag, and starts measurement.
         * Must be called from thread that is associated with this shard.
         */
        SpanState enterSpan(TagId tagId) {
            return enterSpanImpl<SystemCycleClock>(tagId, &SystemCycleClock::get());
        }

        SpanState enterSpan(TagId tagId, CycleClockIface* cycleClock) {
            return enterSpanImpl<CycleClockIface>(tagId, cycleClock);
        }

        /**
         * Leaves the span, stops and  records the measurement.
         * Must be called from thread that is associated with this shard.
         */
        void leaveSpan(const SpanState& state) {
            return leaveSpanImpl<SystemCycleClock>(state, &SystemCycleClock::get());
        }

        void leaveSpan(const SpanState& state, CycleClockIface* cycleClock) {
            return leaveSpanImpl<CycleClockIface>(state, cycleClock);
        }

        /**
         * Returns the snapshot of current call metrics. This method can be safely called from
         * other threads.
         */
        CallMetrics getCallMetrics();

    private:
        template <typename CycleClock>
        inline SpanState enterSpanImpl(TagId tagId, CycleClock* cycleClock) {
            auto parentId = _currentNodeId;

            // tryGetChildNode is read only and therefore thread safe for other threads
            // performing reads.
            NodeId nodeId = _metrics.tryGetChildNode(parentId, tagId);
            if (MONGO_unlikely(!nodeId)) {
                nodeId = getOrInsertChildNodeSafe(parentId, tagId);
            }

            _currentNodeId = nodeId;
            return Profiler::SpanState{nodeId, cycleClock->now()};
        }

        /**
         * Leaves the span, stops and  records the measurement.
         * Must be called from thread that is associated with this shard.
         */
        template <typename CycleClock>
        inline void leaveSpanImpl(const SpanState& state, CycleClock* cycleClock) {
            auto cyclesNow = cycleClock->now();

            auto nodeId = state.nodeId;
            invariant(_currentNodeId == nodeId);

            _currentNodeId = _metrics.callTree().nodes()[nodeId].parentId;

            // Aggregate the metrics is thread safe way. Make sure that writes are atomic.
            // Reads are thread safe as only this thread is allowed to perform updates.
            _metrics.nodeMetrics()[nodeId].cycles.store(
                _metrics.nodeMetrics()[nodeId].cycles.load(std::memory_order_relaxed) + cyclesNow -
                    state.startCycles,
                std::memory_order_relaxed);

            _metrics.nodeMetrics()[nodeId].count.store(
                _metrics.nodeMetrics()[nodeId].count.load(std::memory_order_relaxed) + 1,
                std::memory_order_relaxed);
        }

        MONGO_COMPILER_NOINLINE NodeId getOrInsertChildNodeSafe(NodeId parentId, TagId tagId);

    private:
        Profiler* _profiler;
        NodeId _currentNodeId;
        CallMetrics _metrics;
        mutable std::shared_mutex _sharedMutex;  // NOLINT
    };

    typedef std::unique_ptr<ThreadLocalShard, boost::alignment::aligned_delete> ShardUniquePtr;

    Profiler(const ProfilerTags* profilerTags, ProfilerMetrics::OverheadCycles overheadMetrics);
    ~Profiler();

    /**
     * Returns all profiler metrics based on collected measurements up to this point.
     */
    ProfilerMetrics getMetrics() {
        return getMetricsImpl(SystemCycleClock::get().frequency());
    }

    ProfilerMetrics getMetrics(CycleClockIface* cycleClock) {
        return getMetricsImpl(cycleClock->frequency());
    }

    /**
     * Creates a shard that can be used to record and access metrics associated with specific
     * thread.
     */
    ShardUniquePtr createShard();

private:
    Profiler(const Profiler&) = delete;
    Profiler& operator=(const Profiler&) = delete;

    void registerShard(ThreadLocalShard* shard);
    void unregisterShard(ThreadLocalShard* shard);

    ProfilerMetrics getMetricsImpl(double frequency);

private:
    const ProfilerTags* _profilerTags;
    ProfilerMetrics::OverheadCycles _overheadCycles;
    CallMetrics _callMetrics;
    stdx::unordered_set<ThreadLocalShard*> _shards;
    mutable std::shared_mutex _sharedMutex;  // NOLINT
};

/**
 * A guard utility that ensures that profiler is notified when the associated profile span is
 * closed.
 */
class [[maybe_unused]] ProfilerSpan {
    friend class GlobalProfilerService;

public:
    ProfilerSpan() : _shard(nullptr), _state{} {}
    ProfilerSpan(Profiler::ThreadLocalShard* shard, Profiler::SpanState state)
        : _shard(shard), _state(state) {}

    ProfilerSpan(const ProfilerSpan&) = delete;
    ProfilerSpan(ProfilerSpan&&) = delete;
    ProfilerSpan& operator=(const ProfilerSpan&) = delete;
    ProfilerSpan& operator=(ProfilerSpan&&) = delete;

    ~ProfilerSpan() {
        release();
    }

    void release() {
        if (_shard) {
            _shard->leaveSpan(_state);
            _shard = nullptr;
        }
    }

private:
    Profiler::ThreadLocalShard* _shard;
    Profiler::SpanState _state;
};

/**
 * A self benchmark utility that is used to estimate the performance / overhead of the profiler.
 */
class ProfilerBenchmark {
public:
    ProfilerBenchmark(Profiler::ThreadLocalShard* shard) : _shard(shard) {}

    MONGO_COMPILER_ALWAYS_INLINE Profiler::SpanState enterSpan(TagId tagId) {
        return _shard->enterSpan(tagId);
    }

    MONGO_COMPILER_ALWAYS_INLINE void leaveSpan(const Profiler::SpanState& state) {
        _shard->leaveSpan(state);
    }

    /**
     * Enters 5 spans using narrow nodes.
     */
    int doX5Narrow();

    /**
     * Enters 25 spans using wide nodes.
     */
    int doX25Wide();

private:
    Profiler::ThreadLocalShard* _shard;
};

// Include GlobalProfilerService only if enabled, as it introduces global and thread local members.
#if MONGO_CONFIG_USE_TRACING_PROFILER
class GlobalProfilerService {
public:
    GlobalProfilerService() = delete;
    ~GlobalProfilerService() = delete;

    MONGO_COMPILER_ALWAYS_INLINE static Profiler* getProfiler() {
        return &_profiler;
    }

    MONGO_COMPILER_ALWAYS_INLINE static Profiler::ThreadLocalShard* getShard() {
        Profiler::ThreadLocalShard* shard = _tlShard.get();
        return shard ? shard : (_tlShard = _profiler.createShard()).get();
    }

    template <FixedString name>
    MONGO_COMPILER_ALWAYS_INLINE [[nodiscard]] static ProfilerSpan enterSpan() {
        auto shard = GlobalProfilerService::getShard();
        return ProfilerSpan(shard,
                            shard ? shard->enterSpan(ProfilerTagSource<name>::getId())
                                  : Profiler::SpanState());
    }

private:
    static Profiler _profiler;
    static thread_local Profiler::ShardUniquePtr _tlShard;
};
#endif
}  // namespace mongo::tracing_profiler::internal
