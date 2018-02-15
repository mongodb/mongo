/**
 * Copyright (C) 2018 MongoDB Inc.
 *
 * This program is free software: you can redistribute it and/or  modify
 * it under the terms of the GNU Affero General Public License, version 3,
 * as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Affero General Public License for more details.
 *
 * You should have received a copy of the GNU Affero General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 * As a special exception, the copyright holders give permission to link the
 * code of portions of this program with the OpenSSL library under certain
 * conditions as described in each individual source file and distribute
 * linked combinations including the program with the OpenSSL library. You
 * must comply with the GNU Affero General Public License in all respects for
 * all of the code used other than as permitted herein. If you modify file(s)
 * with this exception, you may extend this exception to your version of the
 * file(s), but you are not obligated to do so. If you do not wish to do so,
 * delete this exception statement from your version. If you delete this
 * exception statement from all source files in the program, then also delete
 * it in the license file.
 */

#include "mongo/platform/basic.h"

#include "mongo/db/auth/sasl_mechanism_advertiser.h"

#include "mongo/crypto/sha1_block.h"
#include "mongo/db/auth/authorization_manager.h"
#include "mongo/db/auth/sasl_options.h"
#include "mongo/db/auth/user.h"
#include "mongo/util/icu.h"
#include "mongo/util/scopeguard.h"

namespace mongo {

namespace {
void appendMechanismIfSupported(StringData mechanism, BSONArrayBuilder* builder) {
    const auto& globalMechanisms = saslGlobalParams.authenticationMechanisms;
    if (std::find(globalMechanisms.begin(), globalMechanisms.end(), mechanism) !=
        globalMechanisms.end()) {
        (*builder) << mechanism;
    }
}

/**
 * Fetch credentials for the named user eithing using the unnormalized form provided,
 * or the form returned from saslPrep().
 * If both forms exist as different user records, produce an error.
 */
User::CredentialData getUserCredentials(OperationContext* opCtx, const std::string& username) {
    AuthorizationManager* authManager = AuthorizationManager::get(opCtx->getServiceContext());
    User* rawObj = nullptr;
    User* prepObj = nullptr;
    auto guard = MakeGuard([authManager, &rawObj, &prepObj] {
        if (prepObj) {
            authManager->releaseUser(prepObj);
        }
        if (rawObj) {
            authManager->releaseUser(rawObj);
        }
    });

    const auto rawUserName = uassertStatusOK(UserName::parse(username));
    const auto rawStatus = authManager->acquireUser(opCtx, rawUserName, &rawObj);

    // Attempt to normalize the provided username (excluding DB portion).
    // If saslPrep() fails, then there can't possibly be another user with
    // compatibility equivalence, so fall-through.
    const auto swPrepUser = saslPrep(rawUserName.getUser());
    if (swPrepUser.isOK()) {
        UserName prepUserName(swPrepUser.getValue(), rawUserName.getDB());
        if (prepUserName != rawUserName) {
            // User has a SASLPREPable name which differs from the raw presentation.
            // Double check that we don't have a different user by that new name.
            const auto prepStatus = authManager->acquireUser(opCtx, prepUserName, &prepObj);
            if (prepStatus.isOK()) {
                // If both statuses are OK, then we have two distinct users with "different" names.
                uassert(ErrorCodes::BadValue,
                        "Two users exist with names exhibiting compatibility equivalence",
                        !rawStatus.isOK());
                // Otherwise, only the normalized version exists.
                return prepObj->getCredentials();
            }
        }
    }

    uassertStatusOK(rawStatus);
    return rawObj->getCredentials();
}

}  // namespace


void SASLMechanismAdvertiser::advertise(OperationContext* opCtx,
                                        const BSONObj& cmdObj,
                                        BSONObjBuilder* result) {
    BSONElement saslSupportedMechs = cmdObj["saslSupportedMechs"];
    if (saslSupportedMechs.type() == BSONType::String) {
        const auto credentials = getUserCredentials(opCtx, saslSupportedMechs.String());

        BSONArrayBuilder mechanismsBuilder;
        if (credentials.isExternal) {
            for (const StringData& userMechanism : {"GSSAPI", "PLAIN"}) {
                appendMechanismIfSupported(userMechanism, &mechanismsBuilder);
            }
        }
        if (credentials.scram<SHA1Block>().isValid()) {
            appendMechanismIfSupported("SCRAM-SHA-1", &mechanismsBuilder);
        }
        if (credentials.scram<SHA256Block>().isValid()) {
            appendMechanismIfSupported("SCRAM-SHA-256", &mechanismsBuilder);
        }

        result->appendArray("saslSupportedMechs", mechanismsBuilder.arr());
    }
}


}  // namespace mongo
