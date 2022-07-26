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

#include "mongo/platform/basic.h"

#include <memory>

#include "mongo/db/auth/address_restriction.h"
#include "mongo/db/auth/address_restriction_gen.h"
#include "mongo/db/server_options.h"

constexpr mongo::StringData mongo::address_restriction_detail::ClientSource::label;
constexpr mongo::StringData mongo::address_restriction_detail::ClientSource::field;

constexpr mongo::StringData mongo::address_restriction_detail::ServerAddress::label;
constexpr mongo::StringData mongo::address_restriction_detail::ServerAddress::field;

mongo::StatusWith<mongo::RestrictionSet<>> mongo::parseAddressRestrictionSet(
    const BSONObj& obj) try {
    IDLParserContext ctx("address restriction");
    const auto ar = Address_restriction::parse(ctx, obj);
    std::vector<std::unique_ptr<NamedRestriction>> vec;

    const boost::optional<std::vector<StringData>>& client = ar.getClientSource();
    if (client) {
        vec.push_back(std::make_unique<ClientSourceRestriction>(client.get()));
    }

    const boost::optional<std::vector<StringData>>& server = ar.getServerAddress();
    if (server) {
        vec.push_back(std::make_unique<ServerAddressRestriction>(server.get()));
    }

    if (vec.empty()) {
        return Status(ErrorCodes::CollectionIsEmpty,
                      "At least one of 'clientSource' or 'serverAddress' must be set");
    }
    return RestrictionSet<>(std::move(vec));
} catch (const DBException& e) {
    return Status(ErrorCodes::BadValue, e.what());
}

mongo::StatusWith<mongo::SharedRestrictionDocument> mongo::parseAuthenticationRestriction(
    const BSONArray& arr) {
    static_assert(
        std::is_same<std::shared_ptr<RestrictionDocument<>>, SharedRestrictionDocument>::value,
        "SharedRestrictionDocument expected to be a shared_ptr to a RestrictionDocument<>");
    using document_type = SharedRestrictionDocument::element_type;
    static_assert(std::is_same<document_type::pointer_type,
                               std::unique_ptr<document_type::element_type>>::value,
                  "SharedRestrictionDocument expected to contain a sequence of unique_ptrs");

    document_type::sequence_type doc;
    for (const auto& elem : arr) {
        if (elem.type() != Object) {
            return Status(ErrorCodes::UnsupportedFormat,
                          "restriction array sub-documents must be address restriction objects");
        }

        auto restriction = parseAddressRestrictionSet(elem.Obj());
        if (!restriction.isOK()) {
            return restriction.getStatus();
        }

        doc.emplace_back(
            std::make_unique<document_type::element_type>(std::move(restriction.getValue())));
    }

    return std::make_shared<document_type>(std::move(doc));
}

mongo::StatusWith<mongo::BSONArray> mongo::getRawAuthenticationRestrictions(
    const BSONArray& arr) noexcept try {
    BSONArrayBuilder builder;

    for (auto const& elem : arr) {
        if (elem.type() != Object) {
            return Status(ErrorCodes::UnsupportedFormat,
                          "'authenticationRestrictions' array sub-documents must be address "
                          "restriction objects");
        }
        IDLParserContext ctx("address restriction");
        auto const ar = Address_restriction::parse(ctx, elem.Obj());
        if (auto const&& client = ar.getClientSource()) {
            // Validate
            ClientSourceRestriction(client.get());
        }
        if (auto const&& server = ar.getServerAddress()) {
            // Validate
            ServerAddressRestriction(server.get());
        }
        if (!ar.getClientSource() && !ar.getServerAddress()) {
            return Status(ErrorCodes::CollectionIsEmpty,
                          "At least one of 'clientSource' and/or 'serverAddress' must be set");
        }
        builder.append(ar.toBSON());
    }
    return builder.arr();
} catch (const DBException& e) {
    return Status(ErrorCodes::BadValue, e.what());
} catch (const std::exception& e) {
    return Status(ErrorCodes::InternalError, e.what());
}
