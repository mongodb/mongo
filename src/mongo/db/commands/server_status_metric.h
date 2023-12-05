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

#pragma once

#include <boost/optional.hpp>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <utility>
#include <variant>

#include "mongo/base/counter.h"
#include "mongo/base/string_data.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/cluster_role.h"
#include "mongo/db/jsobj.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/synchronized_value.h"

namespace mongo {

class ServerStatusMetric {
public:
    virtual ~ServerStatusMetric() = default;
    virtual void appendTo(BSONObjBuilder& b, StringData leafName) const = 0;
};

/**
 * ServerStatusMetricField is the generic class for storing and reporting server
 * status metrics.
 * Its recommended usage is through the addMetricToTree helper function. Here is an
 * example of a ServerStatusMetricField holding a Counter64.
 * Note that the metric is a reference.
 *
 * auto& metric =
 *      addMetricToTree("path.to.counter", std::make_unique<ServerStatusMetricField<Counter64>>());
 *      ...
 *      metric.value().increment();
 *
 * Or with `makeServerStatusMetric`:
 *
 *     auto& counter = makeServerStatusMetric<Counter64>("path.to.counter");
 *     ...
 *     counter.increment();
 *
 * To read the metric from JavaScript:
 *      db.serverStatus().metrics.path.to.counter
 */
template <typename T>
class ServerStatusMetricField : public ServerStatusMetric {
public:
    T& value() {
        return _t;
    }

    /**
     * If the predicate has been set, is is consulted when appending the metric.
     * If it evaluates to false, the metric is not appended.
     */
    void setEnabledPredicate(std::function<bool()> enabled) {
        _enabled = std::move(enabled);
    }

    void appendTo(BSONObjBuilder& b, StringData leafName) const override {
        if (_enabled && !_enabled())
            return;
        b.append(leafName, _t);
    }

private:
    T _t;
    std::function<bool()> _enabled;
};

/**
 * Metrics are organized as a tree by dot-delimited paths.
 * If path starts with `.`, it will be stripped and the path will be treated
 * as being directly under the root.
 * Otherwise, it will appear under the "metrics" subtree.
 * Examples:
 *     tree.add(".foo.m1", m1);  // m1 appears as "foo.m1"
 *     tree.add("foo.m2", m2);   // m2 appears as "metrics.foo.m2"
 *
 * The `role` is used to disambiguate collisions.
 */
class MetricTree {
public:
    /** Can hold either a subtree or a metric. */
    struct TreeNode {
    public:
        explicit TreeNode(std::unique_ptr<MetricTree> v) : _v{std::move(v)} {}
        explicit TreeNode(std::unique_ptr<ServerStatusMetric> v) : _v{std::move(v)} {}

        bool isSubtree() const {
            return _v.index() == 0;
        }

        const std::unique_ptr<MetricTree>& getSubtree() const {
            return get<0>(_v);
        }

        const std::unique_ptr<ServerStatusMetric>& getMetric() const {
            return get<1>(_v);
        }

    private:
        std::variant<std::unique_ptr<MetricTree>, std::unique_ptr<ServerStatusMetric>> _v;
    };

    using ChildMap = std::map<std::string, TreeNode, std::less<>>;

    void add(StringData path,
             std::unique_ptr<ServerStatusMetric> metric,
             boost::optional<ClusterRole> role = {});

    void appendTo(BSONObjBuilder& b, const BSONObj& excludePaths = {}) const;

    const ChildMap& children() const {
        return _children;
    }

private:
    void _add(StringData path,
              std::unique_ptr<ServerStatusMetric> metric,
              boost::optional<ClusterRole> role);

    ChildMap _children;
};

MetricTree& getGlobalMetricTree();

template <typename T>
T& addMetricToTree(StringData name,
                   std::unique_ptr<T> metric,
                   MetricTree& tree = getGlobalMetricTree()) {
    invariant(metric);
    T& reference = *metric;
    tree.add(name, std::move(metric));
    return reference;
}

template <typename T>
T& makeServerStatusMetric(StringData path) {
    return addMetricToTree(path, std::make_unique<ServerStatusMetricField<T>>()).value();
}

/** Make a metric that only appends itself when `predicate` is true. */
template <typename T>
T& makeServerStatusMetric(StringData path, std::function<bool()> predicate) {
    auto ptr = std::make_unique<ServerStatusMetricField<T>>();
    ptr->setEnabledPredicate(std::move(predicate));
    return addMetricToTree(path, std::move(ptr)).value();
}

/**
 * Write a merger of the `trees` to `b`, under field `name`. `excludePaths` is a
 * BSON tree of Bool, specifying `false` for subtrees that are to be omitted
 * from the results.
 */
void appendMergedTrees(std::vector<const MetricTree*> trees,
                       BSONObjBuilder& b,
                       const BSONObj& excludePaths = {});

/** Replicates the public interface of Counter64. */
class CounterMetric {
public:
    explicit CounterMetric(StringData name) : _counter{makeServerStatusMetric<Counter64>(name)} {}
    CounterMetric(StringData name, std::function<bool()> predicate)
        : _counter{makeServerStatusMetric<Counter64>(name, std::move(predicate))} {}
    CounterMetric(CounterMetric&) = delete;
    CounterMetric& operator=(CounterMetric&) = delete;

    operator Counter64&() {
        return _counter;
    }

    void increment(uint64_t n = 1) const {
        _counter.increment(n);
    }

    void decrement(uint64_t n = 1) const {
        _counter.decrement(n);
    }

    long long get() const {
        return _counter.get();
    }

    operator long long() const {
        return get();
    }

private:
    Counter64& _counter;
};

/**
 * Leverage `synchronized_value<T>` to make a thread-safe `T` metric, for `T`
 * that are not intrinsically thread-safe (e.g. string, int).
 * T must be usable as an argument to `BSONObjBuilder::append`.
 */
template <typename T>
class SynchronizedMetric : public ServerStatusMetric {
public:
    void appendTo(BSONObjBuilder& b, StringData leafName) const override {
        b.append(leafName, **_v);
    }

    synchronized_value<T>& value() {
        return _v;
    }

private:
    synchronized_value<T> _v;
};

/**
 * Make a `synchronized_value<T>`-backed metric.
 * Example (note the auto& reference):
 *
 *     auto& currSize = makeSynchronizedMetric<long long>("some.path.size");
 *     currSize = message.size();
 *
 *     auto& currName = makeSynchronizedMetric<std::string>("some.path.name");
 *     currName = message.size();
 */
template <typename T>
synchronized_value<T>& makeSynchronizedMetric(std::string path) {
    return addMetricToTree(path, std::make_unique<SynchronizedMetric<T>>()).value();
}

}  // namespace mongo
