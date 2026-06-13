/**
 *    Copyright (C) 2025-present MongoDB, Inc.
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

#include "mongo/base/string_data.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/server_parameter.h"
#include "mongo/util/modules.h"

#include <concepts>
#include <utility>

MONGO_MOD_PUBLIC;

namespace mongo::unittest {
namespace server_parameter_guard_detail {
/**
 * Used to check if the provided value can be appended to a BSONObjBuilder.
 */
template <typename T>
concept IsBSONAppendable = requires(BSONObjBuilder builder, StringData name, T value) {
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
    ServerParameterGuard(StringData name, T value) {
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
            newParamBuilder.append("_id"_sd, name);
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
