// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/auth/address_restriction.h"

#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsontypes.h"
#include "mongo/db/auth/address_restriction_gen.h"
#include "mongo/idl/idl_parser.h"
#include "mongo/util/assert_util.h"

#include <exception>
#include <memory>
#include <string_view>
#include <type_traits>
#include <utility>

#include <boost/move/utility_core.hpp>
#include <boost/optional/optional.hpp>

constexpr std::string_view mongo::address_restriction_detail::ClientSource::label;
constexpr std::string_view mongo::address_restriction_detail::ClientSource::field;

constexpr std::string_view mongo::address_restriction_detail::ServerAddress::label;
constexpr std::string_view mongo::address_restriction_detail::ServerAddress::field;

mongo::StatusWith<mongo::RestrictionSet<>> mongo::parseAddressRestrictionSet(
    const BSONObj& obj) try {
    IDLParserContext ctx("address restriction");
    const auto ar = Address_restriction::parse(obj, ctx);
    std::vector<std::unique_ptr<NamedRestriction>> vec;

    const boost::optional<std::vector<std::string_view>>& client = ar.getClientSource();
    if (client) {
        vec.push_back(std::make_unique<ClientSourceRestriction>(client.value()));
    }

    const boost::optional<std::vector<std::string_view>>& server = ar.getServerAddress();
    if (server) {
        vec.push_back(std::make_unique<ServerAddressRestriction>(server.value()));
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
        if (elem.type() != BSONType::object) {
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
    const BSONArray& arr) try {
    BSONArrayBuilder builder;

    for (auto const& elem : arr) {
        if (elem.type() != BSONType::object) {
            return Status(ErrorCodes::UnsupportedFormat,
                          "'authenticationRestrictions' array sub-documents must be address "
                          "restriction objects");
        }
        IDLParserContext ctx("address restriction");
        auto const ar = Address_restriction::parse(elem.Obj(), ctx);
        if (auto const&& client = ar.getClientSource()) {
            // Validate
            ClientSourceRestriction(client.value());
        }
        if (auto const&& server = ar.getServerAddress()) {
            // Validate
            ServerAddressRestriction(server.value());
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
