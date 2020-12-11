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

#include "mongo/platform/basic.h"

#include "mongo/client/internal_auth.h"

#include "mongo/bson/json.h"
#include "mongo/client/sasl_client_authenticate.h"
#include "mongo/config.h"
#include "mongo/db/auth/authorization_manager.h"
#include "mongo/db/auth/sasl_command_constants.h"
#include "mongo/platform/mutex.h"
#include "mongo/util/password_digest.h"

namespace mongo {
namespace auth {

static auto internalAuthKeysMutex = MONGO_MAKE_LATCH();
static bool internalAuthSet = false;
static std::vector<std::string> internalAuthKeys;
static BSONObj internalAuthParams;

void setInternalAuthKeys(const std::vector<std::string>& keys) {
    stdx::lock_guard<Latch> lk(internalAuthKeysMutex);

    internalAuthKeys = keys;
    fassert(50996, internalAuthKeys.size() > 0);
    internalAuthSet = true;
}

void setInternalUserAuthParams(BSONObj obj) {
    stdx::lock_guard<Latch> lk(internalAuthKeysMutex);
    internalAuthParams = obj.getOwned();
    internalAuthKeys.clear();
    internalAuthSet = true;
}

bool hasMultipleInternalAuthKeys() {
    stdx::lock_guard<Latch> lk(internalAuthKeysMutex);
    return internalAuthSet && internalAuthKeys.size() > 1;
}

bool isInternalAuthSet() {
    stdx::lock_guard<Latch> lk(internalAuthKeysMutex);
    return internalAuthSet;
}

BSONObj createInternalX509AuthDocument(boost::optional<StringData> userName) {
    BSONObjBuilder builder;
    builder.append(saslCommandMechanismFieldName, "MONGODB-X509");
    builder.append(saslCommandUserDBFieldName, "$external");

    if (userName) {
        builder.append(saslCommandUserFieldName, userName.get());
    }

    return builder.obj();
}

BSONObj getInternalAuthParams(size_t idx, StringData mechanism) {
    stdx::lock_guard<Latch> lk(internalAuthKeysMutex);
    if (!internalAuthSet) {
        return BSONObj();
    }

    // If we've set a specific BSONObj as the internal auth pararms, return it if the index
    // is zero (there are no alternate credentials if we've set a BSONObj explicitly).
    if (!internalAuthParams.isEmpty()) {
        return idx == 0 ? internalAuthParams : BSONObj();
    }

    // If the index is larger than the number of keys we know about then return an empty
    // BSONObj.
    if (idx + 1 > internalAuthKeys.size()) {
        return BSONObj();
    }

    auto password = internalAuthKeys.at(idx);
    if (mechanism == kMechanismScramSha1) {
        password = mongo::createPasswordDigest(
            internalSecurity.user->getName().getUser().toString(), password);
    }

    return BSON(saslCommandMechanismFieldName
                << mechanism << saslCommandUserDBFieldName
                << internalSecurity.user->getName().getDB() << saslCommandUserFieldName
                << internalSecurity.user->getName().getUser() << saslCommandPasswordFieldName
                << password << saslCommandDigestPasswordFieldName << false);
}

std::string getBSONString(BSONObj container, StringData field) {
    auto elem = container[field];
    uassert(ErrorCodes::BadValue,
            str::stream() << "Field '" << field << "' must be of type string",
            elem.type() == String);
    return elem.String();
}


std::string getInternalAuthDB() {
    stdx::lock_guard<Latch> lk(internalAuthKeysMutex);

    if (!internalAuthParams.isEmpty()) {
        return getBSONString(internalAuthParams, saslCommandUserDBFieldName);
    }

    auto isu = internalSecurity.user;
    return isu ? isu->getName().getDB().toString() : "admin";
}

}  // namespace auth
}  // namespace mongo
