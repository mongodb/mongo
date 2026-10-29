// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/auth/sasl_mechanism_registry.h"

#include "mongo/base/error_codes.h"
#include "mongo/base/init.h"  // IWYU pragma: keep
#include "mongo/client/authenticate.h"
#include "mongo/db/auth/auth_name.h"
#include "mongo/db/auth/sasl_options.h"
#include "mongo/db/auth/user.h"
#include "mongo/db/commands/test_commands_enabled.h"
#include "mongo/logv2/log.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/decorable.h"
#include "mongo/util/exit_code.h"
#include "mongo/util/quick_exit.h"
#include "mongo/util/str.h"

#include <algorithm>
#include <iterator>
#include <string_view>

#include <boost/move/utility_core.hpp>
#include <boost/none.hpp>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kAccessControl


namespace mongo {

namespace {
const auto getSASLServerMechanismRegistry =
    Service::declareDecoration<std::unique_ptr<SASLServerMechanismRegistry>>();
}  // namespace

SASLServerMechanismRegistry& SASLServerMechanismRegistry::get(Service* service) {
    auto& uptr = getSASLServerMechanismRegistry(service);
    invariant(uptr);
    return *uptr;
}

void SASLServerMechanismRegistry::set(Service* service,
                                      std::unique_ptr<SASLServerMechanismRegistry> registry) {
    getSASLServerMechanismRegistry(service) = std::move(registry);
}

SASLServerMechanismRegistry::SASLServerMechanismRegistry(Service* service,
                                                         std::vector<std::string> enabledMechanisms)
    : _service(service), _enabledMechanisms(std::move(enabledMechanisms)) {}

void SASLServerMechanismRegistry::setEnabledMechanisms(std::vector<std::string> enabledMechanisms) {
    _enabledMechanisms = std::move(enabledMechanisms);
}

StatusWith<std::unique_ptr<ServerMechanismBase>> SASLServerMechanismRegistry::getServerMechanism(
    std::string_view mechanismName, std::string authenticationDatabase) {
    auto& mechList = _getMapRef(authenticationDatabase);

    auto it = std::find_if(mechList.begin(), mechList.end(), [&](const auto& mech) {
        return (mech->mechanismName() == mechanismName);
    });
    if (it != mechList.end()) {
        return (*it)->create(std::move(authenticationDatabase));
    }

    return Status(ErrorCodes::MechanismUnavailable,
                  str::stream() << "Unsupported mechanism '" << mechanismName
                                << "' on authentication database '" << authenticationDatabase
                                << "'");
}

void SASLServerMechanismRegistry::advertiseMechanismNamesForUser(OperationContext* opCtx,
                                                                 UserName userName,
                                                                 BSONObjBuilder* builder) {
    // Authenticating the __system@local user to the admin database on mongos is required
    // by the auth passthrough test suite.
    auto systemUser = internalSecurity.getUser();
    if (getTestCommandsEnabled() && userName.getUser() == (*systemUser)->getName().getUser() &&
        userName.getDB() == "admin") {
        userName = (*systemUser)->getName();
    }

    AuthorizationManager* authManager = AuthorizationManager::get(opCtx->getService());
    const auto swUser = authManager->acquireUser(
        opCtx, std::make_unique<UserRequestGeneral>(userName, boost::none));

    if (!swUser.isOK()) {
        auto& status = swUser.getStatus();
        if (status.code() == ErrorCodes::UserNotFound) {
            LOGV2(20251,
                  "Supported SASL mechanisms requested for unknown user",
                  "user"_attr = userName);
            // Return the server's enabled mechanisms so the client can select one that the
            // server will accept. Without this, the client falls back to a wire-version
            // heuristic and may select a mechanism that has been explicitly disabled.
            BSONArrayBuilder mechanismsBuilder;
            const auto& mechList = _getMapRef(userName.getDB());
            for (const auto& factoryIt : mechList) {
                SecurityPropertySet properties = factoryIt->properties();
                if (!properties.hasAllProperties(SecurityPropertySet{
                        SecurityProperty::kNoPlainText, SecurityProperty::kMutualAuth}) &&
                    userName.getDB() != "$external") {
                    continue;
                }
                if (_mechanismSupportedByConfig(factoryIt->mechanismName())) {
                    mechanismsBuilder << factoryIt->mechanismName();
                }
            }
            builder->appendArray("saslSupportedMechs", mechanismsBuilder.arr());
            return;
        }
        uassertStatusOK(status);
    }

    UserHandle user = std::move(swUser.getValue());
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
        if (!mechanismEnabled && userName == (*systemUser)->getName()) {
            mechanismEnabled = factoryIt->isInternalAuthMech();
        }

        if (mechanismEnabled && factoryIt->canMakeMechanismForUser(user.get())) {
            mechanismsBuilder << factoryIt->mechanismName();
        }
    }

    builder->appendArray("saslSupportedMechs", mechanismsBuilder.arr());
}

bool SASLServerMechanismRegistry::_mechanismSupportedByConfig(std::string_view mechName) const {
    const auto& v = _enabledMechanisms;
    return std::find(v.begin(), v.end(), mechName) != v.end();
}

void appendMechs(const std::vector<std::unique_ptr<ServerFactoryBase>>& mechs,
                 std::vector<std::string>* pNames) {
    std::transform(mechs.cbegin(),
                   mechs.cend(),
                   std::back_inserter(*pNames),
                   [](const std::unique_ptr<mongo::ServerFactoryBase>& factory) {
                       return std::string{factory->mechanismName()};
                   });
}

std::vector<std::string> SASLServerMechanismRegistry::getMechanismNames() const {
    std::vector<std::string> names;
    names.reserve(_externalMechs.size() + _internalMechs.size());

    appendMechs(_externalMechs, &names);
    appendMechs(_internalMechs, &names);

    return names;
}

std::string_view ServerMechanismBase::getAuthenticationDatabase() const {
    auto systemUser = internalSecurity.getUser();
    if (getTestCommandsEnabled() && _authenticationDatabase == "admin" &&
        getPrincipalName() == (*systemUser)->getName().getUser()) {
        // Allows authenticating as the internal user against the admin database.  This is to
        // support the auth passthrough test framework on mongos (since you can't use the local
        // database on a mongos, so you can't auth as the internal user without this).
        return (*systemUser)->getName().getDB();
    } else {
        return _authenticationDatabase;
    }
}

namespace {
Service::ConstructorActionRegisterer SASLServerMechanismRegistryInitializer{
    "CreateSASLServerMechanismRegistry", {"EndStartupOptionStorage"}, [](Service* service) {
        SASLServerMechanismRegistry::set(service,
                                         std::make_unique<SASLServerMechanismRegistry>(
                                             service, saslGlobalParams.authenticationMechanisms));
    }};

Service::ConstructorActionRegisterer SASLServerMechanismRegistryValidationInitializer{
    "ValidateSASLServerMechanismRegistry",
    {"CreateSASLServerMechanismRegistry"},
    [](Service* service) {
        auto supportedMechanisms = SASLServerMechanismRegistry::get(service).getMechanismNames();

        // Manually include MONGODB-X509 since there is no factory for it since it not a SASL
        // mechanism
        supportedMechanisms.push_back(std::string{auth::kMechanismMongoX509});

        // Error if the user tries to use a SASL mechanism that does not exist
        for (const auto& mech : saslGlobalParams.authenticationMechanisms) {
            auto it = std::find(supportedMechanisms.cbegin(), supportedMechanisms.cend(), mech);
            if (it == supportedMechanisms.end()) {
                LOGV2_ERROR(4742901, "Unsupported SASL mechanism", "mechanism"_attr = mech);

                // Quick Exit since we are in the middle of setting up ServiceContext
                quickExit(ExitCode::badOptions);
            }
        }
    }};

}  // namespace

}  // namespace mongo
