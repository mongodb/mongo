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

#include "audit_client_attrs.h"

namespace mongo::rpc {
namespace {

const auto getAuditClientAttrs =
    Client::declareDecoration<synchronized_value<boost::optional<AuditClientAttrs>>>();

// AuditClientAttrs BSON field names.
constexpr auto kLocalFieldName = "local"_sd;
constexpr auto kRemoteFieldName = "remote"_sd;
constexpr auto kProxiesFieldName = "proxies"_sd;

}  // namespace

boost::optional<AuditClientAttrs> AuditClientAttrs::get(Client* client) {
    return getAuditClientAttrs(client).get();
}

void AuditClientAttrs::set(Client* client, AuditClientAttrs clientAttrs) {
    *getAuditClientAttrs(client) = std::move(clientAttrs);
}

AuditClientAttrs AuditClientAttrs::parseFromBSON(BSONObj obj) {
    std::bitset<3> usedFields;
    constexpr size_t kLocalFieldBit = 0;
    constexpr size_t kRemoteFieldBit = 1;
    constexpr size_t kProxiesFieldBit = 2;

    HostAndPort local, remote;
    std::vector<HostAndPort> proxies{};

    const auto validateTopLevelField =
        [&](const BSONElement& elem, const size_t bit, BSONType expectedType) {
            auto fieldName = elem.fieldNameStringData();
            uassert(ErrorCodes::BadValue,
                    str::stream() << "Encountered field named " << fieldName << " with type "
                                  << typeName(elem.type())
                                  << ". AuditClientAttrs expects this field to have type "
                                  << typeName(expectedType),
                    elem.type() == expectedType);
            uassert(ErrorCodes::BadValue,
                    str::stream() << "Encountered more than one field named: " << fieldName,
                    !usedFields[bit]);
        };

    for (const auto& element : obj) {
        auto fieldName = element.fieldNameStringData();
        if (fieldName == kLocalFieldName) {
            // Check that the field is a string, then parse to HostAndPort.
            validateTopLevelField(element, kLocalFieldBit, String);
            local = HostAndPort::parseThrowing(element.valueStringDataSafe());
            usedFields.set(kLocalFieldBit);
        } else if (fieldName == kRemoteFieldName) {
            // Check that the field is a string, then parse to HostAndPort.
            validateTopLevelField(element, kRemoteFieldBit, String);
            remote = HostAndPort::parseThrowing(element.valueStringDataSafe());
            usedFields.set(kRemoteFieldBit);
        } else if (fieldName == kProxiesFieldName) {
            // Check that the field is an Array, then individually parse each element to a
            // HostAndPort.
            validateTopLevelField(element, kProxiesFieldBit, Array);
            for (const auto& proxyElem : element.Array()) {
                uassert(ErrorCodes::BadValue,
                        str::stream()
                            << "Encountered entry in 'proxies' array of type "
                            << typeName(proxyElem.type()) << ", but was expecting 'String'",
                        proxyElem.type() == String);
                proxies.push_back(HostAndPort::parseThrowing(proxyElem.valueStringDataSafe()));
            }
            usedFields.set(kProxiesFieldBit);
        } else {
            // For now, parse AuditClientAttrs strictly and throw on unrecognized fields.
            uasserted(ErrorCodes::BadValue,
                      str::stream() << "Encountered unknown field named: '" << fieldName
                                    << " when parsing BSON to AuditClientAttrs");
        }
    }

    uassert(ErrorCodes::BadValue,
            "BSON being parsed into AuditClientAttrs must contain a field named 'local'.",
            usedFields[kLocalFieldBit]);
    uassert(ErrorCodes::BadValue,
            str::stream()
                << "BSON being parsed into AuditClientAttrs must contain a field named 'remote'",
            usedFields[kRemoteFieldBit]);
    uassert(ErrorCodes::BadValue,
            str::stream()
                << "BSON being parsed into AuditClientAttrs must contain a field named 'proxies'",
            usedFields[kProxiesFieldBit]);

    return AuditClientAttrs(local, remote, proxies);
}

BSONObj AuditClientAttrs::serialize() const {
    BSONObjBuilder builder;
    builder << kLocalFieldName << _local.toString() << kRemoteFieldName << _remote.toString();
    if (!_proxies.empty()) {
        BSONArrayBuilder arrBuilder(builder.subarrayStart(kProxiesFieldName));
        for (const auto& proxyHost : _proxies) {
            arrBuilder << proxyHost.toString();
        }
        arrBuilder.done();
    } else {
        builder << kProxiesFieldName << BSONArray();
    }

    return builder.obj();
}

}  // namespace mongo::rpc
