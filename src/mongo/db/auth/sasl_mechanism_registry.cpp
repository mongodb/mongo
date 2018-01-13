/**
 *    Copyright (C) 2018 MongoDB Inc.
 *
 *    This program is free software: you can redistribute it and/or  modify
 *    it under the terms of the GNU Affero General Public License, version 3,
 *    as published by the Free Software Foundation.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    GNU Affero General Public License for more details.
 *
 *    You should have received a copy of the GNU Affero General Public License
 *    along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the GNU Affero General Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */

#include "mongo/platform/basic.h"

#include "mongo/db/auth/sasl_mechanism_registry.h"

#include "mongo/base/init.h"
#include "mongo/db/auth/sasl_options.h"
#include "mongo/db/auth/user.h"
#include "mongo/util/icu.h"
#include "mongo/util/net/sock.h"
#include "mongo/util/scopeguard.h"
#include "mongo/util/sequence_util.h"

namespace mongo {

namespace {
const auto getSASLServerMechanismRegistry =
    ServiceContext::declareDecoration<std::unique_ptr<SASLServerMechanismRegistry>>();

/**
 * Fetch credentials for the named user either using the unnormalized form provided,
 * or the form returned from saslPrep().
 * If both forms exist as different user records, produce an error.
 */
User* getUserPtr(OperationContext* opCtx, const UserName& userName) {
    AuthorizationManager* authManager = AuthorizationManager::get(opCtx->getServiceContext());

    User* rawObj = nullptr;
    const auto rawStatus = authManager->acquireUser(opCtx, userName, &rawObj);
    auto rawGuard = MakeGuard([authManager, &rawObj] {
        if (rawObj) {
            authManager->releaseUser(rawObj);
        }
    });

    // Attempt to normalize the provided username (excluding DB portion).
    // If saslPrep() fails, then there can't possibly be another user with
    // compatibility equivalence, so fall-through.
    const auto swPrepUser = saslPrep(userName.getUser());
    if (swPrepUser.isOK()) {
        UserName prepUserName(swPrepUser.getValue(), userName.getDB());
        if (prepUserName != userName) {
            // User has a SASLPREPable name which differs from the raw presentation.
            // Double check that we don't have a different user by that new name.
            User* prepObj = nullptr;
            const auto prepStatus = authManager->acquireUser(opCtx, prepUserName, &prepObj);

            auto prepGuard = MakeGuard([authManager, &prepObj] {
                if (prepObj) {
                    authManager->releaseUser(prepObj);
                }
            });

            if (prepStatus.isOK()) {
                // If both statuses are OK, then we have two distinct users with "different" names.
                uassert(ErrorCodes::BadValue,
                        "Two users exist with names exhibiting compatibility equivalence",
                        !rawStatus.isOK());
                // Otherwise, only the normalized version exists.
                User* returnObj = nullptr;
                std::swap(returnObj, prepObj);
                return returnObj;
            }
        }
    }

    uassertStatusOK(rawStatus);
    User* returnObj = nullptr;
    std::swap(returnObj, rawObj);
    return returnObj;
}

}  // namespace

SASLServerMechanismRegistry& SASLServerMechanismRegistry::get(ServiceContext* serviceContext) {
    auto& uptr = getSASLServerMechanismRegistry(serviceContext);
    invariant(uptr);
    return *uptr;
}

void SASLServerMechanismRegistry::set(ServiceContext* service,
                                      std::unique_ptr<SASLServerMechanismRegistry> registry) {
    getSASLServerMechanismRegistry(service) = std::move(registry);
}

StatusWith<std::unique_ptr<ServerMechanismBase>> SASLServerMechanismRegistry::getServerMechanism(
    StringData mechanismName, std::string authenticationDatabase) {
    auto& map = _getMapRef(authenticationDatabase);

    auto it = map.find(mechanismName.toString());
    if (it != map.end()) {
        return it->second->create(std::move(authenticationDatabase));
    }

    return Status(ErrorCodes::BadValue,
                  mongoutils::str::stream() << "Unsupported mechanism '" << mechanismName
                                            << "' on authentication database '"
                                            << authenticationDatabase
                                            << "'");
}

void SASLServerMechanismRegistry::advertiseMechanismNamesForUser(OperationContext* opCtx,
                                                                 const BSONObj& isMasterCmd,
                                                                 BSONObjBuilder* builder) {
    BSONElement saslSupportedMechs = isMasterCmd["saslSupportedMechs"];
    if (saslSupportedMechs.type() == BSONType::String) {

        const auto userName = uassertStatusOK(UserName::parse(saslSupportedMechs.String()));
        const auto userPtr = getUserPtr(opCtx, userName);
        auto guard = MakeGuard([&opCtx, &userPtr] {
            AuthorizationManager* authManager =
                AuthorizationManager::get(opCtx->getServiceContext());
            authManager->releaseUser(userPtr);
        });

        BSONArrayBuilder mechanismsBuilder;
        auto& map = _getMapRef(userName.getDB());

        for (const auto& factoryIt : map) {
            SecurityPropertySet properties = factoryIt.second->properties();
            if (!properties.hasAllProperties(SecurityPropertySet{SecurityProperty::kNoPlainText,
                                                                 SecurityProperty::kMutualAuth}) &&
                userName.getDB() != "$external") {
                continue;
            }

            if (factoryIt.second->canMakeMechanismForUser(userPtr)) {
                mechanismsBuilder << factoryIt.first;
            }
        }

        builder->appendArray("saslSupportedMechs", mechanismsBuilder.arr());
    }
}

bool SASLServerMechanismRegistry::_mechanismSupportedByConfig(StringData mechName) {
    return sequenceContains(saslGlobalParams.authenticationMechanisms, mechName);
}

MONGO_INITIALIZER_WITH_PREREQUISITES(CreateSASLServerMechanismRegistry, ("SetGlobalEnvironment"))
(::mongo::InitializerContext* context) {
    if (saslGlobalParams.hostName.empty())
        saslGlobalParams.hostName = getHostNameCached();
    if (saslGlobalParams.serviceName.empty())
        saslGlobalParams.serviceName = "mongodb";

    auto registry = stdx::make_unique<SASLServerMechanismRegistry>();
    SASLServerMechanismRegistry::set(getGlobalServiceContext(), std::move(registry));
    return Status::OK();
}

}  // namespace mongo
