// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

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
#include <string_view>

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

Credential createInternalX509AuthCredential(boost::optional<std::string_view> userName) {
    return Credential{
        .mechanism = AuthMechanism::kMongoX509,
        .db = std::string{"$external"},
        .username = userName ? boost::make_optional(std::string{*userName}) : boost::none,
    };
}

boost::optional<Credential> getInternalAuthParams(size_t idx, std::string_view mechanism) {
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
