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

#include <functional>
#include <map>
#include <memory>
#include <string>

#include "mongo/db/jsobj.h"
#include "mongo/util/synchronized_value.h"

namespace mongo {

class ServerStatusMetric {
public:
    virtual ~ServerStatusMetric() = default;

    const std::string& getMetricName() const {
        return _name;
    }

    virtual void appendAtLeaf(BSONObjBuilder& b) const = 0;

protected:
    static std::string _parseLeafName(const std::string& name);

    /**
     * The parameter name is a string where periods have a special meaning.
     * It represents the path where the metric can be found into the tree.
     * If name starts with ".", it will be treated as a path from
     * the serverStatus root otherwise it will live under the "counters"
     * namespace so foo.bar would be serverStatus().counters.foo.bar
     */
    explicit ServerStatusMetric(std::string name);

    const std::string _name;
    const std::string _leafName;
};

/**
 * ServerStatusMetricField is the generic class for storing and reporting server
 * status metrics.
 * Its recommended usage is through the addMetricToTree helper function. Here is an
 * example of a ServerStatusMetricField holding a Counter64.
 * Note that the metric is a reference.
 *
 * auto& metric =
 *      addMetricToTree(std::make_unique<ServerStatusMetricField<Counter64>>("path.to.counter"));
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
    explicit ServerStatusMetricField(std::string name) : ServerStatusMetric(std::move(name)) {}

    void appendAtLeaf(BSONObjBuilder& b) const override {
        b.append(_leafName, _t);
    }

    T& value() {
        return _t;
    }

private:
    T _t;
};

/**
 * A ServerStatusMetricField that is only reported when a predicate is satisfied.
 *
 * It's recommended to create these same way as ServerStatusMetricField, with the
 * makeServerStatusMetric helper, but with the predicate as an additional
 * function argument. Example:
 *
 *     auto predicate = [] { return gMyCounterFeatureFlag.isEnabledAndIgnoreFCVUnsafeAtStartup(); };
 *     auto& counter = makeServerStatusMetric<Counter64>("path.to.counter", predicate);
 *     ...
 *     counter.increment();
 */
template <typename T>
class ConditionalServerStatusMetricField : public ServerStatusMetricField<T> {
public:
    ConditionalServerStatusMetricField(std::string name, std::function<bool()> predicate)
        : ServerStatusMetricField<T>{std::move(name)}, _predicate{std::move(predicate)} {}

    void appendAtLeaf(BSONObjBuilder& b) const override {
        if (_predicate && !_predicate())
            return;
        ServerStatusMetricField<T>::appendAtLeaf(b);
    }

private:
    std::function<bool()> _predicate;
};

class MetricTree {
public:
    ~MetricTree() = default;

    void add(std::unique_ptr<ServerStatusMetric> metric);

    /**
     * Append the metrics tree to the given BSON builder.
     */
    void appendTo(BSONObjBuilder& b) const;

    /**
     * Overload of appendTo which allows tree of exclude paths. The alternative overload is
     * preferred to avoid overhead when no excludes are present.
     */
    void appendTo(const BSONObj& excludePaths, BSONObjBuilder& b) const;

private:
    void _add(const std::string& path, std::unique_ptr<ServerStatusMetric> metric);

    std::map<std::string, std::unique_ptr<MetricTree>> _subtrees;
    std::map<std::string, std::unique_ptr<ServerStatusMetric>> _metrics;
};

/**
 * globalMetricTree is responsible for creating and returning a MetricTree instance
 * statically stored inside. The create parameter bypasses its creation returning a
 * null pointer for when it has not been created yet.
 */
MetricTree* globalMetricTree(bool create = true);

template <typename T>
T& addMetricToTree(std::unique_ptr<T> metric, MetricTree* metricTree = globalMetricTree()) {
    invariant(metric);
    invariant(metricTree);
    T& reference = *metric;
    metricTree->add(std::move(metric));
    return reference;
}

template <typename T>
T& makeServerStatusMetric(std::string path) {
    return addMetricToTree(std::make_unique<ServerStatusMetricField<T>>(std::move(path))).value();
}

/** Make a metric that only appends itself when `predicate` is true. */
template <typename T>
T& makeServerStatusMetric(std::string path, std::function<bool()> predicate) {
    return addMetricToTree(std::make_unique<ConditionalServerStatusMetricField<T>>(
                               std::move(path), std::move(predicate)))
        .value();
}

class CounterMetric {
public:
    CounterMetric(const std::string& name) : _counter{makeServerStatusMetric<Counter64>(name)} {}
    CounterMetric(const std::string& name, std::function<bool()>&& predicate)
        : _counter{makeServerStatusMetric<Counter64>(name, std::move(predicate))} {}
    CounterMetric(CounterMetric&) = delete;
    CounterMetric& operator=(CounterMetric&) = delete;

    operator Counter64&() {
        return _counter;
    }

    /**
     * replicates the same public interface found in Counter64.
     */

    /**
     * Atomically increment.
     */
    void increment(uint64_t n = 1) const {
        _counter.increment(n);
    }

    /**
     * Atomically decrement.
     */
    void decrement(uint64_t n = 1) const {
        _counter.decrement(n);
    }

    /**
     * Return the current value.
     */
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
    explicit SynchronizedMetric(std::string name) : ServerStatusMetric{std::move(name)} {}

    void appendAtLeaf(BSONObjBuilder& b) const override {
        b.append(_leafName, **_v);
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
    return addMetricToTree(std::make_unique<SynchronizedMetric<T>>(std::move(path))).value();
}

}  // namespace mongo
