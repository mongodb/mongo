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

#include "mongo/base/counter.h"
#include "mongo/base/string_data.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/topology/cluster_role.h"
#include "mongo/platform/atomic_word.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/synchronized_value.h"

#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <tuple>
#include <utility>
#include <variant>

#include <boost/optional.hpp>

namespace mongo {
class Service;

namespace status_metric_detail {

template <typename M>
using HasValueFnOp = decltype(std::declval<M>().value());

template <typename M>
constexpr inline bool hasValueFn = stdx::is_detected_v<HasValueFnOp, M>;

template <typename M>
auto& voidlessValue(M& m) {
    if constexpr (hasValueFn<M>) {
        return m.value();
    } else {
        struct Dummy {};
        static constexpr Dummy dummy;
        return dummy;
    }
}
}  // namespace status_metric_detail

/**
 * Polymorphic interface for a metric to be published in the payload of serverStatus.
 */
class ServerStatusMetric {
public:
    virtual ~ServerStatusMetric() = default;

    /**
     * Appends this metric to the current `b` as name `leafName`.
     * Metrics do not know the name to appear under: `leafName` tells them.
     */
    virtual void appendTo(BSONObjBuilder& b, StringData leafName) const = 0;

    /**
     * If the predicate has been set, is is consulted when appending the metric.
     * If it evaluates to false, the metric is not appended.
     */
    void setEnabledPredicate(std::function<bool()> enabled) {
        _enabled = std::move(enabled);
    }

    bool isEnabled() const {
        return !_enabled || _enabled();
    }

private:
    std::function<bool()> _enabled;
};

/**
 * A generic `ServerStatusMetric` controlled by a policy to handle a common use
 * case. `ValuePolicy` configures the `BasicServerStatusMetric` template.
 *
 * Has a `ValuePolicy` data member. Calls the policy's `doAppend` member
 * function to perform serialization.
 *
 * For `ValuePolicy p`, require:
 *
 *     p.appendTo(bob, leafName)
 *         A customization point, whereby this metric specifies how it will
 *         append itself as a field `StringData leafName` to the
 *         `BSONObjBuilder& bob`.
 *
 *     T& p.value()
 *         [Optional]
 *         Returns a reference to some object through which user code can
 *         manipulate the metric. For example, a `Counter64&`.  This reference
 *         is returned to the user by a metric builder's call operator.
 *
 *         If `ValuePolicy` has no `value()` member function, the builder will
 *         return an empty placeholder value. This is the case for metrics that
 *         don't need any input calls.
 */
template <typename ValuePolicy>
class BasicServerStatusMetric : public ServerStatusMetric {
public:
    /** All ctor args are forwarded to the policy. */
    template <typename... As,
              std::enable_if_t<std::is_constructible_v<ValuePolicy, As...>, int> = 0>
    explicit BasicServerStatusMetric(As&&... args) : _policy{std::forward<As>(args)...} {}

    /** Returns the reference returned by the builder, for the user to retain. */
    auto& value() {
        return status_metric_detail::voidlessValue(_policy);
    }

    void appendTo(BSONObjBuilder& b, StringData leafName) const override {
        if (!isEnabled())
            return;
        _policy.appendTo(b, leafName);
    }

private:
    MONGO_COMPILER_NO_UNIQUE_ADDRESS ValuePolicy _policy;
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

    void add(StringData path, std::unique_ptr<ServerStatusMetric> metric);

    void appendTo(BSONObjBuilder& b, const BSONObj& excludePaths = {}) const;

    const ChildMap& children() const {
        return _children;
    }

private:
    void _add(StringData path, std::unique_ptr<ServerStatusMetric> metric);

    ChildMap _children;
};

class MetricTreeSet {
public:
    /**
     * Returns the metric tree for the specified ClusterRole.
     * The `role` must be exactly one of None, ShardServer, or RouterServer.
     */
    MetricTree& operator[](ClusterRole role);

private:
    MetricTree _none;
    MetricTree _shard;
    MetricTree _router;
};

MetricTreeSet& globalMetricTreeSet();

/**
 * Write a merger of the `trees` to `b`, under field `name`. `excludePaths` is a
 * BSON tree of Bool, specifying `false` for subtrees that are to be omitted
 * from the results.
 */
void appendMergedTrees(std::vector<const MetricTree*> trees,
                       BSONObjBuilder& b,
                       const BSONObj& excludePaths = {});

template <typename Policy>
class CustomMetricBuilder {
public:
    using Metric = BasicServerStatusMetric<Policy>;

    explicit CustomMetricBuilder(std::string name) : _name{std::move(name)} {}

    /** Execute the builder, creating a reference to the registered metric */
    auto& operator*() && {
        std::unique_ptr<Metric> ptr;
        if (_construct)
            ptr = _construct();
        else if constexpr (std::is_constructible_v<Metric>)
            ptr = std::make_unique<Metric>();
        else
            invariant(_construct, "No suitable constructor");
        if (_pred)
            ptr->setEnabledPredicate(std::move(_pred));
        auto& reference = *ptr;
        MetricTreeSet* trees = _trees ? _trees : &globalMetricTreeSet();
        MetricTree* tree = &(*trees)[_role];
        if (!tree)
            tree = &globalMetricTreeSet()[ClusterRole::None];
        tree->add(_name, std::move(ptr));
        return status_metric_detail::voidlessValue(reference);
    }

    CustomMetricBuilder setTreeSet(MetricTreeSet* trees) && {
        _trees = trees;
        return std::move(*this);
    }

    CustomMetricBuilder setRole(ClusterRole role) && {
        _role = role;
        return std::move(*this);
    }

    CustomMetricBuilder setPredicate(std::function<bool()> pred) && {
        _pred = std::move(pred);
        return std::move(*this);
    }

    /* Sets constructor arguments for the built product. */
    template <typename... Args>
    CustomMetricBuilder bind(Args... args) && {
        _construct = [args...] {
            return std::make_unique<Metric>(args...);
        };
        return std::move(*this);
    }

private:
    std::string _name;
    std::function<bool()> _pred;
    std::function<std::unique_ptr<Metric>()> _construct;
    MetricTreeSet* _trees = nullptr;
    ClusterRole _role{};
};

/** The ValuePolicy to be used for the simple case of BSON-appendable types. */
template <typename T>
class DefaultStatusMetricValuePolicy {
public:
    template <typename... As, std::enable_if_t<std::is_constructible_v<T, As...>, int> = 0>
    explicit DefaultStatusMetricValuePolicy(As&&... args) : _v{std::forward<As>(args)...} {}

    auto& value() {
        return _v;
    }

    void appendTo(BSONObjBuilder& b, StringData leafName) const {
        b.append(leafName, _v);
    }

private:
    T _v;
};

/** Trait for choosing a policy for a metric. */
template <typename T>
struct ServerStatusMetricPolicySelection {
    using type = DefaultStatusMetricValuePolicy<T>;
};
template <typename T>
using ServerStatusMetricPolicySelectionT = typename ServerStatusMetricPolicySelection<T>::type;

/**
 * A CustomMetricBuilder, using a policy chosen by the ServerStatusMetricValuePolicy trait,
 *
 * Example (note the auto& reference):
 *   auto& myMetric = *MetricBuilder<Counter64>("network.myMetric")
 *                         .setPredicate(someNullaryPredicate);
 */
template <typename T>
using MetricBuilder = CustomMetricBuilder<ServerStatusMetricPolicySelectionT<T>>;

/**
 * Leverage `synchronized_value<T>` to make a thread-safe `T` metric, for `T`
 * that are not intrinsically thread-safe (e.g. string, int).
 * T must be usable as an argument to `BSONObjBuilder::append`.
 */
template <typename T>
struct ServerStatusMetricPolicySelection<synchronized_value<T>> {
    class Policy {
    public:
        template <typename... As, std::enable_if_t<std::is_constructible_v<T, As...>, int> = 0>
        explicit Policy(As&&... args) : _v{std::forward<As>(args)...} {}

        auto& value() {
            return _v;
        }

        void appendTo(BSONObjBuilder& b, StringData leafName) const {
            b.append(leafName, **_v);
        }

    private:
        synchronized_value<T> _v;
    };
    using type = Policy;
};

template <typename T>
struct CounterMetricPolicy {
public:
    T& value() {
        return _v;
    }

    void appendTo(BSONObjBuilder& b, StringData leafName) const {
        b.append(leafName, static_cast<long long>(_v.get()));
    }

private:
    T _v;
};

template <>
struct ServerStatusMetricPolicySelection<Counter64> {
    using type = CounterMetricPolicy<Counter64>;
};

template <>
struct ServerStatusMetricPolicySelection<Atomic64Metric> {
    using type = CounterMetricPolicy<Atomic64Metric>;
};

}  // namespace mongo
