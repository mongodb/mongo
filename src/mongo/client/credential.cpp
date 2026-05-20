/**
 *    Copyright (C) 2026-present MongoDB, Inc.
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

#include "mongo/client/credential.h"

#include "mongo/base/error_codes.h"
#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/util/bson_extract.h"
#include "mongo/stdx/unordered_set.h"

#include <string>

#include <boost/optional/optional.hpp>

namespace mongo {
namespace auth {

namespace {
// BSON field names used in auth parameter documents.
constexpr StringData kMechField = "mechanism"_sd;
constexpr StringData kUserDBField = "db"_sd;
constexpr StringData kUserSourceField = "userSource"_sd;
constexpr StringData kUserField = "user"_sd;
constexpr StringData kPasswordField = "pwd"_sd;
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
    static const stdx::unordered_set<StringData> kCoreFields = {
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
