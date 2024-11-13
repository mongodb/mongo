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
#include <algorithm>

#include "mongo/db/commands/server_status.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/service_context.h"
#include "mongo/logv2/log.h"
#include "mongo/util/tracing_profiler/internal/profiler_internal.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kDefault

namespace mongo::tracing_profiler::internal {

ProfilerTags* ProfilerTags::get() {
#if MONGO_CONFIG_USE_TRACING_PROFILER
    static ProfilerTags profilerTags;
    return &profilerTags;
#else
    failUnconfigured();
    return nullptr;
#endif
}

ProfilerTag ProfilerTags::getOrInsertTag(StringData name) {
    auto it = _tagIdByName.find(name);
    if (it != _tagIdByName.end()) {
        return _tags[it->second];
    }

    ProfilerTag tag{(TagId)_tags.size(), name};
    _tags.push_back(tag);
    _tagIdByName.insert({name, tag.id});

    return tag;
}

size_t CallTree::ChildrenMap::size() const {
    return std::visit(
        OverloadedVisitor{[](const ChildrenInlinedMap& map) -> size_t { return map.size; },
                          [](const ChildrenHashMap& map) -> size_t {
                              return map.size();
                          }},
        *this);
}

void CallTree::ChildrenMap::foreach (std::function<void(TagId, NodeId)>&& fn) const {
    std::visit(OverloadedVisitor{[fn](const ChildrenInlinedMap& map) -> void {
                                     for (size_t i = 0; i < map.size; i++) {
                                         fn(map.data[i].tagId, map.data[i].nodeId);
                                     }
                                 },
                                 [fn](const ChildrenHashMap& map) -> void {
                                     for (auto [tagId, nodeId] : map) {
                                         fn(tagId, nodeId);
                                     }
                                 }},
               *this);
}

NodeId CallTree::getOrInsertChildNode(NodeId parentId, TagId tagId) {
    if (_nodes.empty()) {
        _nodes.emplace_back(0, 0);
    }

    return std::visit(
        OverloadedVisitor{[this, parentId, tagId](CallTree::ChildrenInlinedMap& map) -> NodeId {
                              for (size_t i = 0; i < map.size; i++) {
                                  if (map.data[i].tagId == tagId)
                                      return map.data[i].nodeId;
                              }
                              NodeId nodeId = _nodes.size();
                              if (map.size < map.MaxSize) {
                                  NodeId nodeId = _nodes.size();
                                  map.data[map.size] = {tagId, nodeId};
                                  map.size++;
                              } else {
                                  CallTree::ChildrenHashMap newMap;
                                  newMap.reserve(map.MaxSize + 1);
                                  for (size_t i = 0; i < map.size; i++) {
                                      newMap.emplace(map.data[i].tagId, map.data[i].nodeId);
                                  }
                                  newMap.emplace(tagId, nodeId);
                                  _nodes[parentId].children = std::move(newMap);
                              }
                              _nodes.emplace_back(parentId, tagId);
                              return nodeId;
                          },
                          [this, parentId, tagId](CallTree::ChildrenHashMap& map) -> NodeId {
                              auto [it, r] = map.try_emplace(tagId, _nodes.size());
                              if (r) {
                                  _nodes.emplace_back(parentId, tagId);
                              }
                              return it->second;
                          }},
        _nodes[parentId].children);
}

CallMetrics::CallMetrics(CallTree&& callTree, std::vector<NodeMetrics>&& nodeMetrics)
    : _callTree(std::move(callTree)), _nodeMetrics(std::move(nodeMetrics)) {}

NodeId CallMetrics::getOrInsertChildNode(NodeId parentId, TagId tagId) {
    NodeId nodeId = _callTree.getOrInsertChildNode(parentId, tagId);
    if (_nodeMetrics.size() <= nodeId) {
        _nodeMetrics.resize(nodeId + 1);
    }

    return nodeId;
}

void CallMetrics::append(const CallMetrics& other) {
    if (other._callTree.nodes().empty()) {
        return;
    }

    appendVisit(other, 0, 0);
}

void CallMetrics::appendVisit(const CallMetrics& other, NodeId thisId, NodeId otherId) {
    if (thisId > 0) {
        _nodeMetrics[thisId].count += other._nodeMetrics[otherId].count;
        _nodeMetrics[thisId].cycles += other._nodeMetrics[otherId].cycles;
    }

    auto appendChild = [this, other, thisId, otherId](TagId tagId, NodeId nodeId) {
        appendVisit(other, getOrInsertChildNode(thisId, tagId), nodeId);
    };

    other._callTree.nodes()[otherId].children.foreach (appendChild);
}

ProfilerMetrics::ProfilerMetrics(CallMetrics&& callMetrics,
                                 std::vector<ProfilerMetrics::ComputedMetrics>&& computedMetrics,
                                 const ProfilerTags* profilerTags,
                                 double frequency)
    : _callMetrics(std::move(callMetrics)),
      _computedMetrics(std::move(computedMetrics)),
      _profilerTags(profilerTags),
      _frequency(frequency) {}

void ProfilerMetrics::toBson(BSONObjBuilder* builder) const {
    BSONArrayBuilder spansBuilder = builder->subarrayStart("spans");

    const auto cyclesToNanos = [this](int64_t cycles) {
        return (int64_t)(cycles / _frequency * 1e9);
    };

    for (NodeId nodeId = 1; nodeId < _callMetrics.nodeMetrics().size(); nodeId++) {
        BSONObjBuilder spanBuilder = spansBuilder.subobjStart();

        spanBuilder.append("id", (int64_t)nodeId);
        spanBuilder.append(
            "name", _profilerTags->tags()[_callMetrics.callTree().nodes()[nodeId].tagId].name);
        spanBuilder.append("parentId", (int64_t)_callMetrics.callTree().nodes()[nodeId].parentId);
        spanBuilder.append("totalNanos", cyclesToNanos(_callMetrics.nodeMetrics()[nodeId].cycles));
        spanBuilder.append("netNanos", cyclesToNanos(_computedMetrics[nodeId].netCycles));
        spanBuilder.append("exclusiveNanos",
                           cyclesToNanos(_computedMetrics[nodeId].exclusiveCycles));
        spanBuilder.append("count", (int64_t)_callMetrics.nodeMetrics()[nodeId].count);
    }
}

ProfilerMetrics::ComputedMetricsBuilder::ComputedMetricsBuilder(
    CallMetrics& callMetrics, ProfilerMetrics::OverheadCycles& overheadCycles)
    : _callMetrics(callMetrics), _overheadCycles(overheadCycles) {}

std::vector<ProfilerMetrics::ComputedMetrics> ProfilerMetrics::ComputedMetricsBuilder::build() {
    _computedMetrics.resize(_callMetrics.nodeMetrics().size());
    if (!_callMetrics.callTree().nodes().empty()) {
        visit(0);
    }
    return std::move(_computedMetrics);
}

void ProfilerMetrics::ComputedMetricsBuilder::visit(NodeId nodeId) {
    _callMetrics.callTree().nodes()[nodeId].children.foreach (
        [this, nodeId](TagId, NodeId childId) -> void {
            visit(childId);
            _computedMetrics[nodeId].totalOverheadCycles +=
                _computedMetrics[childId].totalOverheadCycles;
        });

    double baseOverheadCycles =
        std::visit(OverloadedVisitor{[this](const CallTree::ChildrenInlinedMap&) {
                                         return _overheadCycles.narrowNodeOverheadCycles;
                                     },
                                     [this](const CallTree::ChildrenHashMap&) {
                                         return _overheadCycles.wideNodeOverheadCycles;
                                     }},
                   _callMetrics.callTree().nodes()[nodeId].children);
    _computedMetrics[nodeId].nodeOverheadCycles =
        (int64_t)(baseOverheadCycles * _callMetrics.nodeMetrics()[nodeId].count);
    _computedMetrics[nodeId].totalOverheadCycles += _computedMetrics[nodeId].nodeOverheadCycles;

    // Cycles measured for a node include overhead of all the children, and half of overhead
    // of the node itself. This is because each elapsed time measurement includes half ot
    // the overhead of the measurement itself. So if we subtract totalOverheadCycles then we
    // will subtract all the children, but we will also subtract all of node overhead, while
    // we should only subtract half of it.
    _computedMetrics[nodeId].netCycles = _callMetrics.nodeMetrics()[nodeId].cycles -
        _computedMetrics[nodeId].totalOverheadCycles +
        _computedMetrics[nodeId].nodeOverheadCycles / 2;

    _computedMetrics[nodeId].exclusiveCycles = _computedMetrics[nodeId].netCycles;
    _callMetrics.callTree().nodes()[nodeId].children.foreach (
        [this, nodeId](TagId, NodeId childId) -> void {
            _computedMetrics[nodeId].exclusiveCycles -= _computedMetrics[childId].netCycles;
        });
}

Profiler::ThreadLocalShard::ThreadLocalShard(Profiler* profiler)
    : _profiler(profiler), _currentNodeId(0) {
    _profiler->registerShard(this);
}
Profiler::ThreadLocalShard::~ThreadLocalShard() {
    _profiler->unregisterShard(this);
}

NodeId Profiler::ThreadLocalShard::getOrInsertChildNodeSafe(NodeId parentId, TagId tagId) {
    std::unique_lock lock(_sharedMutex);
    return _metrics.getOrInsertChildNode(parentId, tagId);
}

CallMetrics Profiler::ThreadLocalShard::getCallMetrics() {
    std::shared_lock lock(_sharedMutex);
    CallTree callTree = _metrics.callTree();
    std::vector<CallMetrics::NodeMetrics> nodeMetrics;
    nodeMetrics.reserve(_metrics.nodeMetrics().size());
    for (auto& m : _metrics.nodeMetrics()) {
        nodeMetrics.emplace_back(m.cycles.load(std::memory_order_relaxed),
                                 m.count.load(std::memory_order_relaxed));
    }

    return CallMetrics(std::move(callTree), std::move(nodeMetrics));
}

Profiler::Profiler(const ProfilerTags* profilerTags,
                   ProfilerMetrics::OverheadCycles overheadMetrics)
    : _profilerTags(profilerTags), _overheadCycles(overheadMetrics) {}

Profiler::~Profiler() {
    invariant(_shards.empty());
}

ProfilerMetrics Profiler::getMetricsImpl(double frequency) {
    std::shared_lock lock(_sharedMutex);

    CallMetrics callMetrics = _callMetrics;

    std::vector<CallMetrics> shardMetrics;
    for (ThreadLocalShard* shard : _shards) {
        callMetrics.append(shard->getCallMetrics());
    }

    std::vector<ProfilerMetrics::ComputedMetrics> computedMetrics =
        ProfilerMetrics::ComputedMetricsBuilder(callMetrics, _overheadCycles).build();

    return ProfilerMetrics(
        std::move(callMetrics), std::move(computedMetrics), _profilerTags, frequency);
}

Profiler::ShardUniquePtr Profiler::createShard() {
    void* p = boost::alignment::aligned_alloc(stdx::hardware_destructive_interference_size,
                                              sizeof(Profiler::ThreadLocalShard));

    return Profiler::ShardUniquePtr(new (p) Profiler::ThreadLocalShard(this));
}

void Profiler::registerShard(Profiler::ThreadLocalShard* shard) {
    std::unique_lock lock(_sharedMutex);
    _shards.insert(shard);
}

void Profiler::unregisterShard(Profiler::ThreadLocalShard* shard) {
    std::unique_lock lock(_sharedMutex);
    _callMetrics.append(shard->getCallMetrics());
    _shards.erase(shard);
}

namespace {
#if MONGO_CONFIG_USE_TRACING_PROFILER
/**
 * Measure the overhead of single measurement in cycles. The overhead depends on the number of
 * children the parent node has (node width), so it is measured for specific node width.
 */
double measureX5NarrowInternal() {
    Profiler profiler(nullptr, {});
    Profiler::ThreadLocalShard shard(&profiler);
    ProfilerBenchmark benchmark(&shard);

    int64_t totalCount = 1024;
    int64_t startCycles = SystemCycleClock::get().now();
    for (int64_t i = 0; i < totalCount; i++) {
        benchmark.doX5Narrow();
    }

    int64_t totalCycles = SystemCycleClock::get().now() - startCycles;
    return (double)(totalCycles) / (double)(totalCount);
}

double measureX25WideInternal() {
    Profiler profiler(nullptr, {});
    Profiler::ThreadLocalShard shard(&profiler);
    ProfilerBenchmark benchmark(&shard);

    int64_t totalCount = 1024;
    int64_t startCycles = SystemCycleClock::get().now();
    for (int64_t i = 0; i < totalCount; i++) {
        benchmark.doX25Wide();
    }

    int64_t totalCycles = SystemCycleClock::get().now() - startCycles;
    return (double)(totalCycles) / (double)(totalCount);
}

ProfilerMetrics::OverheadCycles measureOverhead() {
    constexpr size_t samples = 32;
    std::vector<double> narrow;
    std::vector<double> wide;

    // Collect overhead measurements for:
    // * narrow nodes (with 1 child), that fit into a small inlined array
    // * wide nodes with, with multiple children that don't fit into small inlined array
    // The overhead is mostly correlated with container type, so just those 2 variants
    // are sufficient for decent overhead estimate.
    for (size_t i = 0; i < samples; i++) {
        double n = measureX5NarrowInternal();
        double w = measureX25WideInternal();

        // There are 5 narrow nodes in X5Narrow benchmark.
        narrow.push_back(n / 5.0);

        // There is 25 wide nodes in X25Wide benchmark.
        wide.push_back(w / 25.0);
    }
    size_t median = samples / 2;
    std::nth_element(narrow.begin(), narrow.begin() + median, narrow.end());
    std::nth_element(wide.begin(), wide.begin() + median, wide.end());

    auto formatCyclesAsNanos = [](double x) -> std::string {
        // Workaround for LOGV2_INFO not logging doubles properly.
        std::stringstream ss;
        ss << (1e9 * x / SystemCycleClock::get().frequency());
        return ss.str();
    };

    ProfilerMetrics::OverheadCycles overhead{.narrowNodeOverheadCycles = narrow[median],
                                             .wideNodeOverheadCycles = wide[median]};
    LOGV2_INFO(8837400,
               "Estimated overhead of tracing profiler span measurement in nanos",
               "narrow"_attr = formatCyclesAsNanos(overhead.narrowNodeOverheadCycles),
               "wide"_attr = formatCyclesAsNanos(overhead.wideNodeOverheadCycles));
    return overhead;
}
#endif  // MONGO_CONFIG_USE_TRACING_PROFILER
}  // namespace

int ProfilerBenchmark::doX5Narrow() {
    auto s1 = enterSpan(1);

    auto s2 = enterSpan(2);
    auto s3 = enterSpan(3);
    leaveSpan(s3);
    leaveSpan(s2);

    auto s4 = enterSpan(4);
    auto s5 = enterSpan(5);
    leaveSpan(s5);
    leaveSpan(s4);

    leaveSpan(s1);
    return 0;
}

int ProfilerBenchmark::doX25Wide() {
    for (int i = 1; i <= 5; i++) {
        auto s1 = enterSpan(i);
        for (int j = 6; j <= 10; j++) {
            auto s2 = enterSpan(j);
            leaveSpan(s2);
        }
        leaveSpan(s1);
    }

    return 0;
}

#if MONGO_CONFIG_USE_TRACING_PROFILER

ProfilerMetrics::OverheadCycles getOverheadCycles() {
    static ProfilerMetrics::OverheadCycles overhead = measureOverhead();

    return overhead;
}

/**
 * Exports profilers metrics as a metrics section.
 */
Profiler GlobalProfilerService::_profiler(ProfilerTags::get(), getOverheadCycles());
thread_local Profiler::ShardUniquePtr GlobalProfilerService::_tlShard;

class ProfilerStatsServerStatusSection : public ServerStatusSection {
public:
    using ServerStatusSection::ServerStatusSection;

    ~ProfilerStatsServerStatusSection() override = default;

    bool includeByDefault() const override {
        return true;
    }

    BSONObj generateSection(OperationContext* opCtx,
                            const BSONElement& configElement) const override {
        const auto profiler = GlobalProfilerService::getProfiler();
        auto metrics = profiler->getMetrics();

        BSONObjBuilder builder;
        metrics.toBson(&builder);
        return builder.obj();
    }
};

auto& profilerServerStatusSection =
    *ServerStatusSectionBuilder<ProfilerStatsServerStatusSection>("tracing_profiler");
#endif

}  // namespace mongo::tracing_profiler::internal
