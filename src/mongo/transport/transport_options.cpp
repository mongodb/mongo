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

#include <string>
#include <variant>
#include <vector>

#include "mongo/base/status.h"
#include "mongo/base/string_data.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/json.h"
#include "mongo/db/server_options.h"
#include "mongo/idl/idl_parser.h"
#include "mongo/transport/transport_options_gen.h"
#include "mongo/util/net/cidr.h"
#include "mongo/util/overloaded_visitor.h"

namespace mongo::transport {
namespace {
using ConnectionList = std::vector<std::variant<CIDR, std::string>>;

auto parseConnectionListParameters(const BSONObj& obj) {
    IDLParserContext ctx("maxConnectionsOverride");
    const auto params = ConnectionListParameters::parse(ctx, obj);
    ConnectionList output;
    for (const auto& range : params.getRanges()) {
        auto swr = CIDR::parse(range);
        if (!swr.isOK()) {
            output.push_back(range.toString());
        } else {
            output.push_back(std::move(swr.getValue()));
        }
    }
    return output;
}

void appendParameter(VersionedValue<ConnectionList>* value, BSONObjBuilder* bob, StringData name) {
    BSONObjBuilder subBob(bob->subobjStart(name));
    BSONArrayBuilder subArray(subBob.subarrayStart("ranges"_sd));

    invariant(value);
    auto snapshot = value->makeSnapshot();
    if (!snapshot)
        return;

    for (const auto& range : *snapshot) {
        subArray.append(std::visit(OverloadedVisitor{
                                       [](const CIDR& arg) { return arg.toString(); },
                                       [](const std::string& arg) { return arg; },
                                   },
                                   range));
    }
}

Status setParameter(VersionedValue<ConnectionList>* value, BSONObj obj) try {
    invariant(value);
    auto list = parseConnectionListParameters(obj);
    value->update(std::make_shared<ConnectionList>(std::move(list)));
    return Status::OK();
} catch (const AssertionException& e) {
    return e.toStatus();
}

}  // namespace

void MaxIncomingConnectionsOverrideServerParameter::append(OperationContext*,
                                                           BSONObjBuilder* bob,
                                                           StringData name,
                                                           const boost::optional<TenantId>&) {
    appendParameter(&serverGlobalParams.maxIncomingConnsOverride, bob, name);
}

Status MaxIncomingConnectionsOverrideServerParameter::set(const BSONElement& value,
                                                          const boost::optional<TenantId>&) {
    return setParameter(&serverGlobalParams.maxIncomingConnsOverride, value.Obj());
}

Status MaxIncomingConnectionsOverrideServerParameter::setFromString(
    StringData str, const boost::optional<TenantId>&) {
    return setParameter(&serverGlobalParams.maxIncomingConnsOverride, fromjson(str));
}

void MaxEstablishingConnectionsOverrideServerParameter::append(OperationContext*,
                                                               BSONObjBuilder* bob,
                                                               StringData name,
                                                               const boost::optional<TenantId>&) {
    appendParameter(&serverGlobalParams.maxEstablishingConnsOverride, bob, name);
}

Status MaxEstablishingConnectionsOverrideServerParameter::set(const BSONElement& value,
                                                              const boost::optional<TenantId>&) {
    return setParameter(&serverGlobalParams.maxEstablishingConnsOverride, value.Obj());
}

Status MaxEstablishingConnectionsOverrideServerParameter::setFromString(
    StringData str, const boost::optional<TenantId>&) {
    return setParameter(&serverGlobalParams.maxEstablishingConnsOverride, fromjson(str));
}

void IngressConnectionEstablishmentRatePerSecServerParameter::append(
    OperationContext*, BSONObjBuilder* bob, StringData name, const boost::optional<TenantId>&) {
    bob->append(name, _data);
}

Status IngressConnectionEstablishmentRatePerSecServerParameter::set(
    const BSONElement& value, const boost::optional<TenantId>&) {
    double newValue;
    Status coercionStatus = value.tryCoerce(&newValue);
    if (!coercionStatus.isOK() || newValue < 0) {
        return Status(ErrorCodes::BadValue,
                      str::stream()
                          << "Invalid value for ingressConnectionEstablishmentRatePerSec: "
                          << value);
    }

    _data = newValue;

    // TODO SERVER-104415 Set value on RateLimiter

    return Status::OK();
}

Status IngressConnectionEstablishmentRatePerSecServerParameter::setFromString(
    StringData str, const boost::optional<TenantId>&) {
    double newValue;
    Status status = NumberParser{}(str, &newValue);
    if (!status.isOK()) {
        return status;
    }
    if (newValue < 0) {
        return Status(ErrorCodes::BadValue,
                      str::stream()
                          << "Invalid value for ingressConnectionEstablishmentRatePerSec: "
                          << newValue);
    }

    _data = newValue;

    // TODO SERVER-104415 Set value on RateLimiter

    return Status::OK();
}

void IngressConnectionEstablishmentBurstSizeServerParameter::append(
    OperationContext*, BSONObjBuilder* bob, StringData name, const boost::optional<TenantId>&) {
    bob->append(name, _data);
}

Status IngressConnectionEstablishmentBurstSizeServerParameter::set(
    const BSONElement& value, const boost::optional<TenantId>&) {
    double newValue;
    Status coercionStatus = value.tryCoerce(&newValue);
    if (!coercionStatus.isOK() || newValue < 0) {
        return Status(ErrorCodes::BadValue,
                      str::stream() << "Invalid value for ingressConnectionEstablishmentBurstSize: "
                                    << value);
    }

    _data = newValue;

    // TODO SERVER-104415 Set value on RateLimiter

    return Status::OK();
}

Status IngressConnectionEstablishmentBurstSizeServerParameter::setFromString(
    StringData str, const boost::optional<TenantId>&) {
    double newValue;
    Status status = NumberParser{}(str, &newValue);
    if (!status.isOK()) {
        return status;
    }
    if (newValue < 0) {
        return Status(ErrorCodes::BadValue,
                      str::stream() << "Invalid value for ingressConnectionEstablishmentBurstSize: "
                                    << newValue);
    }

    _data = newValue;

    // TODO SERVER-104415 Set value on RateLimiter

    return Status::OK();
}

void IngressConnectionEstablishmentMaxQueueDepthServerParameter::append(
    OperationContext*, BSONObjBuilder* bob, StringData name, const boost::optional<TenantId>&) {
    bob->append(name, _data);
}

Status IngressConnectionEstablishmentMaxQueueDepthServerParameter::set(
    const BSONElement& value, const boost::optional<TenantId>&) {
    if (!value.isNumber()) {
        return Status(ErrorCodes::BadValue,
                      str::stream()
                          << "Invalid type for ingressConnectionEstablishmentMaxQueueDepth: "
                          << value);
    }

    auto valueLong = value.safeNumberLong();
    if (valueLong < 0) {
        return Status(ErrorCodes::BadValue,
                      str::stream()
                          << "Invalid value for ingressConnectionEstablishmentMaxQueueDepth: "
                          << value);
    }

    size_t newValue = valueLong;
    _data = newValue;

    // TODO SERVER-104415 Set value on RateLimiter

    return Status::OK();
}

Status IngressConnectionEstablishmentMaxQueueDepthServerParameter::setFromString(
    StringData str, const boost::optional<TenantId>&) {
    long long valueLong;
    Status status = NumberParser{}(str, &valueLong);
    if (!status.isOK()) {
        return status;
    }
    if (valueLong < 0) {
        return Status(ErrorCodes::BadValue,
                      str::stream()
                          << "Invalid value for ingressConnectionEstablishmentMaxQueueDepth: "
                          << valueLong);
    }

    size_t newValue = valueLong;
    _data = newValue;

    // TODO SERVER-104415 Set value on RateLimiter

    return Status::OK();
}

}  // namespace mongo::transport
