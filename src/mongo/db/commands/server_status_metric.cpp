/**
 *    Copyright (C) 2018-present MongoDB, Inc.
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

#include "mongo/db/commands/server_status_metric.h"

#include "mongo/base/error_codes.h"
#include "mongo/base/string_data.h"
#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsontypes.h"
#include "mongo/db/service_context.h"
#include "mongo/db/topology/cluster_role.h"
#include "mongo/logv2/log.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/static_immortal.h"

#include <cstddef>
#include <deque>
#include <memory>
#include <new>
#include <vector>

#include <absl/container/node_hash_set.h>
#include <fmt/format.h>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kCommand


namespace mongo {
namespace {

/** Algorithm object implementing the `appendMergedTrees` function.  */
class AppendMergedTreesInvocation {
    /** Represents progress in the iteration through the children of one node. */
    class TreeNodeCursor {
    public:
        explicit TreeNodeCursor(const MetricTree* node) : _node{node} {
            invariant(_node);
            start();
        }

        /** The name of the child element to which this refers. Requires `!done()`. */
        StringData key() const {
            return _iter->first;
        }

        /** Requires `isSubtree()`. */
        const MetricTree& getSubtree() const {
            return *_iter->second.getSubtree();
        }

        /** Requires `!isSubtree()`. */
        const ServerStatusMetric& getMetric() const {
            return *_iter->second.getMetric();
        }

        /** Is the child node referred to by the cursor a subtree? Requires `!done()`. */
        bool isSubtree() const {
            return _iter->second.isSubtree();
        }

        bool done() const {
            return _iter == _node->children().end();
        }

        /** Reset the cursor to beginning of children. */
        void start() {
            _iter = _node->children().begin();
        }

        /** Requires `!done()`. */
        void advance() {
            ++_iter;
        }

    private:
        const MetricTree* _node;
        MetricTree::ChildMap::const_iterator _iter;
    };

    /**
     * Manages a stack frame in a recursive descent through the
     * virtual aggregate merged tree.
     *
     * As we visit each node, we are walking through a list of input `cursors`,
     * appending metrics to the `bob` subobject, excluding keys in the
     * `excludePaths` subobject.
     *
     * Each node's sequence of children is traversed twice. This is tracked by
     * `inSubtreePhase`. First pass is to emit all metrics. Second pass is to
     * visit all subtrees.
     */
    struct Frame {
        /** Set all input trees' node cursors to their first child. */
        void startCursors() {
            for (auto&& c : cursors)
                c.start();
        }

        /** Advance all cursors past phase-irrelevant nodes. */
        void skipIgnoredNodes() {
            for (auto&& cp : cursors)
                while (!cp.done() && cp.isSubtree() != inSubtreePhase)
                    cp.advance();
        }

        std::vector<TreeNodeCursor> cursors;
        BSONObjBuilder* bob;
        std::unique_ptr<BSONObjBuilder> bobStorage;
        bool inSubtreePhase;
        StringData key;
        BSONObj excludePaths;
    };

public:
    void operator()(std::vector<const MetricTree*> trees,
                    BSONObjBuilder& b,
                    const BSONObj& excludePaths) {
        // The `_stack` tracks recursive traversal of the virtual merged tree.
        // Traversing a node is done in two phases, first the leaf nodes, then
        // all of the subtree nodes. When the top frame is finished, it is popped.
        // So we initialize by pushing the root Frame onto the `_stack` and looping
        // until the `_stack` is empty. Each iteration of this loop corresponds
        // to the visitation of one node of the virtual merged metric tree view.
        _init(std::move(trees), &b, excludePaths);
        while (!_stack.empty()) {
            Frame& frame = _stack.back();
            // So that all cursors are either at a relevant node or exhausted.
            frame.skipIgnoredNodes();

            // Pick which cursors are going to be visited and advanced.
            // It's all cursors that tie for min key value.
            const auto relevant = _selectCursors(frame);
            if (relevant.empty()) {
                _atEndOfSubtree();
                continue;
            }
            const auto& cursor = *relevant.front();
            auto key = cursor.key();

            auto [excluded, excludeSub] = _applyExclusion(key);
            if (!excluded) {
                if (!frame.inSubtreePhase) {
                    // This node has no subtrees. It should therefore have one member.
                    uassert(
                        ErrorCodes::BadValue,
                        fmt::format("Collision between trees at node {}.{}", _pathDiagJoin(), key),
                        relevant.size() == 1);
                    cursor.getMetric().appendTo(*frame.bob, key);
                } else {
                    _stack.push_back(_descentFrame(relevant, std::move(excludeSub)));
                }
            }

            for (auto&& cp : relevant)
                if (!cp->done())
                    cp->advance();
        }
    }

private:
    /**
     * Returns `true` if `key` is a path excluded from the active Frame. If
     * active Frame is in the subtree phase, and the entire `key` subtree is not
     * excluded, then this returns false and the embedded subtree under that key
     * for use in recursive descent, hence the `std::pair` return type.
     */
    std::pair<bool, BSONObj> _applyExclusion(StringData key) {
        Frame& frame = _stack.back();
        auto el = frame.excludePaths.getField(key);
        if (!el)
            return {false, {}};
        if (el.type() == BSONType::boolean)
            return {!el.boolean(), {}};
        if (el.type() == BSONType::object && frame.inSubtreePhase)
            return {false, el.embeddedObject()};
        uasserted(ErrorCodes::InvalidBSONType,
                  "Exclusion value must be a boolean for leaf nodes. "
                  "Nonleaf nodes may also accept a nested object.");
    }

    /** Returns a new `Frame` suitable for descending into the subtree at the current cursor set. */
    Frame _descentFrame(const std::vector<TreeNodeCursor*>& relevant, BSONObj excludePaths) {
        auto&& frame = _stack.back();
        auto key = relevant.front()->key();
        auto sub = std::make_unique<BSONObjBuilder>(frame.bob->subobjStart(key));
        auto subPtr = &*sub;
        std::vector<TreeNodeCursor> cursors;
        for (auto&& cp : relevant)
            cursors.push_back(TreeNodeCursor{&cp->getSubtree()});
        return Frame{
            std::move(cursors), subPtr, std::move(sub), false, key, std::move(excludePaths)};
    }

    /**
     * Called when the end of a node is reached. The active Frame could either
     * restart in a new phase, or be truly finished and popped from the `_stack`.
     */
    void _atEndOfSubtree() {
        Frame& frame = _stack.back();
        if (!frame.inSubtreePhase) {
            frame.startCursors();
            frame.inSubtreePhase = true;
        } else {
            _stack.pop_back();
        }
    }

    /** Initialize with a frame pointing at the start of the roots of all trees. */
    void _init(std::vector<const MetricTree*> trees, BSONObjBuilder* b, BSONObj excludePaths) {
        std::vector<TreeNodeCursor> cursors;
        for (const MetricTree* node : trees)
            cursors.push_back(TreeNodeCursor{node});
        _stack.push_back(Frame{std::move(cursors), b, nullptr, false, "", std::move(excludePaths)});
    }

    /**
     * Searches all cursors in `frame` to determine the min-valued key among
     * them. Returns pointers to all cursors in `frame` that share this
     * min-valued key. Cursors that are not applicable to the frame's current
     * phase (visiting leaves vs visiting subtrees) are ignored.
     */
    std::vector<TreeNodeCursor*> _selectCursors(Frame& frame) {
        std::vector<TreeNodeCursor*> cursors;
        for (auto&& c : frame.cursors) {
            if (c.done() || c.isSubtree() != frame.inSubtreePhase)
                continue;
            if (!cursors.empty()) {
                int rel = c.key().compare(cursors.front()->key());
                if (rel > 0)
                    continue;
                if (rel < 0)
                    cursors.clear();
            }
            cursors.push_back(&c);
        }
        return cursors;
    }

    /** Returns current path built from the `_stack` keys. */
    std::vector<StringData> _pathDiag() const {
        std::vector<StringData> parts;
        for (auto&& fr : _stack)
            parts.push_back(fr.key);
        return parts;
    }

    /** The `_pathDiag` vector, joined by dots. */
    std::string _pathDiagJoin() const {
        std::string r;
        StringData sep;
        for (auto s : _pathDiag()) {
            (r += sep) += s;
            sep = ".";
        }
        return r;
    }

    std::vector<const MetricTree*> _trees;
    std::deque<Frame> _stack;
};

}  // namespace

void MetricTree::add(StringData path, std::unique_ptr<ServerStatusMetric> metric) {
    // Never add metrics with empty names.
    // If there's a leading ".", strip it.
    // Otherwise, we're really adding with an implied "metrics." prefix.
    if (path.empty())
        return;
    if (path.starts_with('.')) {
        path.remove_prefix(1);
        if (!path.empty())
            _add(path, std::move(metric));
    } else {
        _add(fmt::format("metrics.{}", path), std::move(metric));
    }
}

void MetricTree::_add(StringData path, std::unique_ptr<ServerStatusMetric> metric) {
    StringData tail = path;
    MetricTree* sub = this;
    while (true) {
        // Walk the tree popping heads and creating interior nodes until there's no more tail.
        auto dot = tail.find('.');
        if (dot == std::string::npos) {
            auto [insIter, insOk] =
                sub->_children.try_emplace(std::string{tail}, std::move(metric));
            if (!insOk)
                LOGV2_FATAL(6483100, "metric conflict", "path"_attr = path);
            return;
        }
        // Found a dot, so hop to an interior node, creating it if necessary.
        StringData part = tail.substr(0, dot);
        tail = tail.substr(dot + 1);
        auto iter = sub->_children.find(part);
        if (iter != sub->_children.end()) {
            if (!iter->second.isSubtree())
                LOGV2_FATAL(16461, "metric conflict", "path"_attr = path);
        } else {
            auto [insIter, ok] =
                sub->_children.try_emplace(std::string{part}, std::make_unique<MetricTree>());
            iter = insIter;
        }
        sub = iter->second.getSubtree().get();
    }
}

void appendMergedTrees(std::vector<const MetricTree*> trees,
                       BSONObjBuilder& b,
                       const BSONObj& excludePaths) {
    AppendMergedTreesInvocation{}(std::move(trees), b, excludePaths);
}

void MetricTree::appendTo(BSONObjBuilder& b, const BSONObj& excludePaths) const {
    appendMergedTrees({this}, b, excludePaths);
}

MetricTree& MetricTreeSet::operator[](ClusterRole role) {
    if (role.hasExclusively(ClusterRole::None))
        return _none;
    if (role.hasExclusively(ClusterRole::ShardServer))
        return _shard;
    if (role.hasExclusively(ClusterRole::RouterServer))
        return _router;
    MONGO_UNREACHABLE;
}

MetricTreeSet& globalMetricTreeSet() {
    static StaticImmortal<MetricTreeSet> obj;
    return *obj;
}
}  // namespace mongo
