/**
 *    Copyright (C) 2018-present MerizoDB, Inc.
 *
 *    This program is free software: you can redistribute it and/or modify
 *    it under the terms of the Server Side Public License, version 1,
 *    as published by MerizoDB, Inc.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    Server Side Public License for more details.
 *
 *    You should have received a copy of the Server Side Public License
 *    along with this program. If not, see
 *    <http://www.merizodb.com/licensing/server-side-public-license>.
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
#define MONGO_LOG_DEFAULT_COMPONENT ::merizo::logger::LogComponent::kAccessControl

#include "merizo/platform/basic.h"

#include "merizo/db/auth/sasl_mechanism_registry.h"

#include "merizo/base/init.h"
#include "merizo/db/auth/sasl_options.h"
#include "merizo/db/auth/user.h"
#include "merizo/util/icu.h"
#include "merizo/util/log.h"
#include "merizo/util/net/socket_utils.h"
#include "merizo/util/scopeguard.h"
#include "merizo/util/sequence_util.h"

namespace merizo {

namespace {
const auto getSASLServerMechanismRegistry =
    ServiceContext::declareDecoration<std::unique_ptr<SASLServerMechanismRegistry>>();
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

SASLServerMechanismRegistry::SASLServerMechanismRegistry(std::vector<std::string> enabledMechanisms)
    : _enabledMechanisms(std::move(enabledMechanisms)) {}

void SASLServerMechanismRegistry::setEnabledMechanisms(std::vector<std::string> enabledMechanisms) {
    _enabledMechanisms = std::move(enabledMechanisms);
}

StatusWith<std::unique_ptr<ServerMechanismBase>> SASLServerMechanismRegistry::getServerMechanism(
    StringData mechanismName, std::string authenticationDatabase) {
    auto& mechList = _getMapRef(authenticationDatabase);

    auto it = std::find_if(mechList.begin(), mechList.end(), [&](const auto& mech) {
        return (mech->mechanismName() == mechanismName);
    });
    if (it != mechList.end()) {
        return (*it)->create(std::move(authenticationDatabase));
    }

    return Status(ErrorCodes::BadValue,
                  merizoutils::str::stream() << "Unsupported mechanism '" << mechanismName
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

        AuthorizationManager* authManager = AuthorizationManager::get(opCtx->getServiceContext());

        UserHandle user;
        const auto swUser = authManager->acquireUser(opCtx, userName);
        if (!swUser.isOK()) {
            auto& status = swUser.getStatus();
            if (status.code() == ErrorCodes::UserNotFound) {
                log() << "Supported SASL mechanisms requested for unknown user '" << userName
                      << "'";
                return;
            }
            uassertStatusOK(status);
        }

        user = std::move(swUser.getValue());
        BSONArrayBuilder mechanismsBuilder;
        const auto& mechList = _getMapRef(userName.getDB());

        for (const auto& factoryIt : mechList) {
            SecurityPropertySet properties = factoryIt->properties();
            if (!properties.hasAllProperties(SecurityPropertySet{SecurityProperty::kNoPlainText,
                                                                 SecurityProperty::kMutualAuth}) &&
                userName.getDB() != "$external") {
                continue;
            }

            auto mechanismEnabled = _mechanismSupportedByConfig(factoryIt->mechanismName());
            if (!mechanismEnabled && userName == internalSecurity.user->getName()) {
                mechanismEnabled = factoryIt->isInternalAuthMech();
            }

            if (mechanismEnabled && factoryIt->canMakeMechanismForUser(user.get())) {
                mechanismsBuilder << factoryIt->mechanismName();
            }
        }

        builder->appendArray("saslSupportedMechs", mechanismsBuilder.arr());
    }
}

bool SASLServerMechanismRegistry::_mechanismSupportedByConfig(StringData mechName) const {
    return sequenceContains(_enabledMechanisms, mechName);
}

namespace {
ServiceContext::ConstructorActionRegisterer SASLServerMechanismRegistryInitializer{
    "CreateSASLServerMechanismRegistry",
    {"EndStartupOptionStorage"},
    [](ServiceContext* service) {
        SASLServerMechanismRegistry::set(service,
                                         std::make_unique<SASLServerMechanismRegistry>(
                                             saslGlobalParams.authenticationMechanisms));
    }};
}  // namespace

}  // namespace merizo
