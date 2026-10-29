// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0
#pragma once

#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/server_parameter.h"
#include "mongo/util/modules.h"

#include <concepts>
#include <string_view>
#include <utility>

[[MONGO_MOD_PUBLIC]];

namespace mongo::unittest {
namespace server_parameter_guard_detail {
/**
 * Used to check if the provided value can be appended to a BSONObjBuilder.
 */
template <typename T>
concept IsBSONAppendable = requires(BSONObjBuilder builder, std::string_view name, T value) {
    { builder.append(name, value) };
};

/**
 * Used to check if the provided value has the toBSON() method, which is called
 * if the value is not directly appendable to a BSONObjBuilder.
 */
template <typename T>
concept HasToBSON = requires(T t) {
    { t.toBSON() } -> std::same_as<BSONObj>;
};

template <typename T>
class UniqueObserver {
public:
    UniqueObserver() = default;
    explicit UniqueObserver(T* p) : _p{p} {}

    T* get() const {
        return _p;
    }

    UniqueObserver(UniqueObserver&& o) noexcept : _p{std::exchange(o._p, {})} {}

    UniqueObserver& operator=(UniqueObserver&& o) noexcept {
        _p = std::exchange(o._p, {});
        return *this;
    }

private:
    T* _p{};
};
}  // namespace server_parameter_guard_detail

/**
 * Test-only RAII guard that sets a server parameter to the specified value on construction. Upon
 * destruction, the server parameter is set back to its original value.
 */
class ServerParameterGuard {
public:
    template <typename T>
    requires server_parameter_guard_detail::IsBSONAppendable<T> ||
        server_parameter_guard_detail::HasToBSON<T>
    ServerParameterGuard(std::string_view name, T value) {
        // T must satisfy one of the following criteria:
        //    1. T can be coerced into a type accepted by one of the overloads of
        //    BSONObjBuilder::append(), or
        //    2. T must have a method called toBSON(), returning a BSONObj.

        auto* nodeParameter = ServerParameterSet::getNodeParameterSet()->getIfExists(name);
        auto* clusterParameter = ServerParameterSet::getClusterParameterSet()->getIfExists(name);
        tassert(10431803,
                "Invalid server parameter requested for test",
                nodeParameter || clusterParameter);
        tassert(10431804,
                "Server parameter registered as both node and cluster parameter",
                !(nodeParameter && clusterParameter));
        auto* serverParam = nodeParameter ? nodeParameter : clusterParameter;
        _serverParam = server_parameter_guard_detail::UniqueObserver<ServerParameter>{serverParam};

        BSONObjBuilder oldParamBuilder;
        BSONObjBuilder newParamBuilder;
        if constexpr (server_parameter_guard_detail::IsBSONAppendable<T>) {
            // If the parameter is serialized via BSONObjBuilder::append(), then we can just save
            // the BSONObj as-is in the format {name: <parameter>}.
            serverParam->appendSupportingRoundtrip(nullptr, &oldParamBuilder, name, boost::none);
            _oldValue = oldParamBuilder.obj();

            // Correspondingly, the new parameter value can be set just by synthesizing a BSONObj in
            // the format {name: <value>} and then retrieving the first BSONElement.
            newParamBuilder.append(name, value);
            uassertStatusOK(serverParam->set(newParamBuilder.obj().firstElement(), boost::none));
        } else if constexpr (server_parameter_guard_detail::HasToBSON<T>) {
            // If the parameter is serialized via T::toBSON(), then we need to manually add the
            // synthesized BSONObj as a value keyed by "name".
            serverParam->appendSupportingRoundtrip(nullptr, &oldParamBuilder, name, boost::none);
            _oldValue = BSON(name << oldParamBuilder.obj());

            // Correspondingly, the new parameter value needs to be set by synthesizing a BSONObj
            // that independently includes the name under the _id field.
            newParamBuilder.append("_id", name);
            newParamBuilder.appendElementsUnique(value.toBSON());
            uassertStatusOK(serverParam->set(newParamBuilder.obj(), boost::none));
        }
    }

    ServerParameterGuard(ServerParameterGuard&&) noexcept = default;
    ServerParameterGuard& operator=(ServerParameterGuard&&) noexcept = default;

    ~ServerParameterGuard() {
        auto* serverParam = _serverParam.get();
        if (!serverParam) {
            return;
        }

        // Reset to the old value.
        auto elem = _oldValue.firstElement();
        uassertStatusOK(serverParam->set(elem, boost::none));
    }

private:
    server_parameter_guard_detail::UniqueObserver<ServerParameter> _serverParam;
    BSONObj _oldValue;
};

}  // namespace mongo::unittest
