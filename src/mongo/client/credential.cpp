// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/client/credential.h"

#include "mongo/base/error_codes.h"
#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/util/bson_extract.h"
#include "mongo/stdx/unordered_set.h"

#include <string>
#include <string_view>

#include <boost/optional/optional.hpp>

namespace mongo {
namespace auth {

namespace {
using namespace std::literals::string_view_literals;
// BSON field names used in auth parameter documents.
constexpr std::string_view kMechField = "mechanism"sv;
constexpr std::string_view kUserDBField = "db"sv;
constexpr std::string_view kUserSourceField = "userSource"sv;
constexpr std::string_view kUserField = "user"sv;
constexpr std::string_view kPasswordField = "pwd"sv;
}  // namespace

StatusWith<Credential> Credential::fromBSON(const BSONObj& params) {
    if (params.hasField(kUserDBField) && params.hasField(kUserSourceField)) {
        return Status(ErrorCodes::AuthenticationFailed,
                      "You cannot specify both 'db' and 'userSource'. Please use only 'db'.");
    }

    std::string mechStr;
    if (auto s = bsonExtractStringField(params, kMechField, &mechStr); !s.isOK())
        return s;
    auto swMech = authMechanismFromString(mechStr);
    if (!swMech.isOK())
        return swMech.getStatus();

    boost::optional<std::string> db;
    {
        std::string dbStr;
        if (params.hasField(kUserSourceField)) {
            if (auto s = bsonExtractStringField(params, kUserSourceField, &dbStr); !s.isOK())
                return s;
            if (!dbStr.empty())
                db = std::move(dbStr);
        } else if (params.hasField(kUserDBField)) {
            if (auto s = bsonExtractStringField(params, kUserDBField, &dbStr); !s.isOK())
                return s;
            if (!dbStr.empty())
                db = std::move(dbStr);
        }
    }

    boost::optional<std::string> username;
    {
        std::string usernameStr;
        if (params.hasField(kUserField)) {
            if (auto s = bsonExtractStringField(params, kUserField, &usernameStr); !s.isOK())
                return s;
            if (!usernameStr.empty())
                username = std::move(usernameStr);
        }
    }

    boost::optional<std::string> password;
    {
        std::string passwordStr;
        if (params.hasField(kPasswordField)) {
            if (auto s = bsonExtractStringField(params, kPasswordField, &passwordStr); !s.isOK())
                return s;
            if (!passwordStr.empty())
                password = std::move(passwordStr);
        }
    }

    // Collect mechanism-specific properties (everything except the 5 core fields).
    static const stdx::unordered_set<std::string_view> kCoreFields = {
        kMechField, kUserDBField, kUserSourceField, kUserField, kPasswordField};
    BSONObjBuilder props;
    for (const auto& elem : params) {
        if (!kCoreFields.count(elem.fieldNameStringData()))
            props.append(elem);
    }

    return Credential{
        swMech.getValue(), std::move(db), std::move(username), std::move(password), props.obj()};
}

}  // namespace auth
}  // namespace mongo
