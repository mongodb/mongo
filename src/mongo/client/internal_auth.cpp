/**
 *    Copyright (C) 2020-present MongoDB, Inc.
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

#include "mongo/client/internal_auth.h"

#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/config.h"  // IWYU pragma: keep
#include "mongo/db/auth/authorization_manager.h"
#include "mongo/db/auth/sasl_command_constants.h"
#include "mongo/db/auth/user.h"
#include "mongo/db/auth/user_name.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/password_digest.h"
#include "mongo/util/read_through_cache.h"

#include <mutex>

#include <boost/optional/optional.hpp>

namespace mongo {
namespace auth {

static std::mutex internalAuthKeysMutex;
static bool internalAuthSet = false;
static std::vector<std::string> internalAuthKeys;
static boost::optional<Credential> internalAuthCredential;

void setInternalAuthKeys(const std::vector<std::string>& keys) {
    std::lock_guard<std::mutex> lk(internalAuthKeysMutex);
    internalAuthKeys = keys;
    fassert(50996, internalAuthKeys.size() > 0);
    internalAuthSet = true;
}

void setInternalUserAuthParams(Credential credential) {
    std::lock_guard<std::mutex> lk(internalAuthKeysMutex);
    internalAuthCredential = std::move(credential);
    internalAuthKeys.clear();
    internalAuthSet = true;
}

bool hasMultipleInternalAuthKeys() {
    std::lock_guard<std::mutex> lk(internalAuthKeysMutex);
    return internalAuthSet && internalAuthKeys.size() > 1;
}

bool isInternalAuthSet() {
    std::lock_guard<std::mutex> lk(internalAuthKeysMutex);
    return internalAuthSet;
}

Credential createInternalX509AuthCredential(boost::optional<StringData> userName) {
    return Credential{
        .mechanism = AuthMechanism::kMongoX509,
        .db = std::string{"$external"},
        .username = userName ? boost::make_optional(std::string{*userName}) : boost::none,
    };
}

boost::optional<Credential> getInternalAuthParams(size_t idx, StringData mechanism) {
    std::lock_guard<std::mutex> lk(internalAuthKeysMutex);
    if (!internalAuthSet) {
        return boost::none;
    }

    // Explicit credential (e.g. X.509): only one entry, no alternates.
    if (internalAuthCredential) {
        return idx == 0 ? internalAuthCredential : boost::none;
    }

    if (idx + 1 > internalAuthKeys.size()) {
        return boost::none;
    }

    auto swMech = authMechanismFromString(mechanism);
    if (!swMech.isOK())
        return boost::none;

    auto password = internalAuthKeys.at(idx);
    auto systemUser = internalSecurity.getUser();
    if (swMech.getValue() == AuthMechanism::kScramSha1) {
        password = mongo::createPasswordDigest((*systemUser)->getName().getUser(), password);
    }

    // digestPassword: false because the password is already in its final form above.
    return Credential{swMech.getValue(),
                      std::string{(*systemUser)->getName().getDB()},
                      std::string{(*systemUser)->getName().getUser()},
                      std::move(password),
                      BSON(saslCommandDigestPasswordFieldName << false)};
}

std::string getInternalAuthDB() {
    std::lock_guard<std::mutex> lk(internalAuthKeysMutex);

    if (internalAuthCredential && internalAuthCredential->db) {
        return *internalAuthCredential->db;
    }

    auto systemUser = internalSecurity.getUser();
    if (systemUser && *systemUser) {
        return std::string{(*systemUser)->getName().getDB()};
    }

    return "admin";
}

}  // namespace auth
}  // namespace mongo
