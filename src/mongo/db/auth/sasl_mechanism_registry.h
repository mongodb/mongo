/**
 *    Copyright (C) 2018-present MongoDB, Inc.
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

#pragma once

#include <boost/optional.hpp>
#include <memory>
#include <unordered_map>

#include "mongo/base/string_data.h"
#include "mongo/db/auth/authentication_metrics.h"
#include "mongo/db/auth/authorization_manager.h"
#include "mongo/db/commands.h"
#include "mongo/db/commands/test_commands_enabled.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/service_context.h"

namespace mongo {

class User;
class BSONObjBuilder;

/**
 * The set of attributes SASL mechanisms may possess.
 * Different SASL mechanisms provide different types of assurances to clients and servers.
 * These SecurityProperties are attached to mechanism types to allow reasoning about them.
 *
 * Descriptions of individual properties assumed while using a mechanism with the property.
 */
enum class SecurityProperty : size_t {
    kMutualAuth,            // Both clients and servers are assured of the other's identity.
    kNoPlainText,           // Messages sent across the wire don't include the plaintext password.
    kLastSecurityProperty,  // Do not use. An enum value for internal bookkeeping.
};

/** Allows a set of SecurityProperties to be defined in an initializer list. */
using SecurityPropertyList = std::initializer_list<SecurityProperty>;

/** A set of SecurityProperties which may be exhibited by a SASL mechanism. */
class SecurityPropertySet {
public:
    explicit SecurityPropertySet(SecurityPropertyList props) {
        for (auto prop : props) {
            _set.set(static_cast<size_t>(prop));
        }
    }

    /** Returns true if the set contains all of the requested properties. */
    bool hasAllProperties(SecurityPropertySet propertySet) const {
        for (size_t i = 0; i < propertySet._set.size(); ++i) {
            if (propertySet._set[i] && !_set[i]) {
                return false;
            }
        }
        return true;
    }


private:
    std::bitset<static_cast<size_t>(SecurityProperty::kLastSecurityProperty)> _set;
};

/**
 * SASL server mechanisms are made by a corresponding factory.
 * Mechanisms have properties. We wish to be able to manipulate these properties both at runtime
 * and compile-time. These properties should apply to, and be accessible from, mechanisms and
 * factories. Client mechanisms/factories should be able to use the same property definitions.
 * This allows safe mechanism negotiation.
 *
 * The properties are set by creating a Policy class, and making mechanisms inherit from a helper
 * derived from the class. Factories are derived from a helper which uses the mechanism.
 */

/** Exposes properties of the SASL mechanism. */
class SaslServerCommonBase {
public:
    virtual ~SaslServerCommonBase() = default;
    virtual StringData mechanismName() const = 0;
    virtual SecurityPropertySet properties() const = 0;

    /**
     * This returns a number that represents the "amount" of security provided by this mechanism
     * to determine the order in which it is offered to clients in the isMaster
     * saslSupportedMechs response.
     *
     * The value of securityLevel is arbitrary so long as the more secure mechanisms return a
     * higher value than the less secure mechanisms.
     *
     * For example, SCRAM-SHA-256 > SCRAM-SHA-1 > PLAIN
     */
    virtual int securityLevel() const = 0;

    /**
     * Returns true if the mechanism can be used for internal cluster authentication.
     * Currently only SCRAM-SHA-1/SCRAM-SHA-256 return true here.
     */
    virtual bool isInternalAuthMech() const = 0;
};

/**
 * Base class shared by all server-side SASL mechanisms.
 */
class ServerMechanismBase : public SaslServerCommonBase {
public:
    explicit ServerMechanismBase(std::string authenticationDatabase)
        : _authenticationDatabase(std::move(authenticationDatabase)) {}

    virtual ~ServerMechanismBase() = default;

    /**
     * Returns the principal name which this mechanism is performing authentication for.
     * This name is provided by the client, in the SASL messages they send the server.
     * This value may not be available until after some number of conversation steps have
     * occurred.
     *
     * This method is virtual so more complex implementations can obtain this value from a
     * non-member.
     */
    virtual StringData getPrincipalName() const {
        return _principalName;
    }

    /**
     * Returns the expiration time, if applicable, of the user's authentication for the given
     * mechanism. The default of boost::none indicates that the user will be authenticated
     * indefinitely on the session.
     */
    virtual boost::optional<Date_t> getExpirationTime() const {
        return boost::none;
    }

    /**
     * Appends mechanism specific info in BSON form. The schema of this BSON will vary by mechanism
     * implementation, thus this info is entirely diagnostic/for records.
     */
    virtual void appendExtraInfo(BSONObjBuilder*) const {}

    /**
     * Standard method in mongodb for determining if "authenticatedUser" may act as "requestedUser."
     *
     * The standard rule in MongoDB is simple.  The authenticated user name must be the same as the
     * requested user name.
     */
    virtual bool isAuthorizedToActAs(StringData requestedUser, StringData authenticatedUser) {
        return requestedUser == authenticatedUser;
    }

    /**
     * Provides logic for determining if a user is a cluster member or an actual client for SASL
     * authentication mechanisms
     */
    bool isClusterMember() const {
        auto systemUser = internalSecurity.getUser();
        return _principalName == (*systemUser)->getName().getUser() &&
            getAuthenticationDatabase() == (*systemUser)->getName().getDB();
    };

    /**
     * Performs a single step of a SASL exchange. Takes an input provided by a client,
     * and either returns an error, or a response to be sent back.
     */
    StatusWith<std::string> step(OperationContext* opCtx, StringData input) {

        auto result = stepImpl(opCtx, input);
        if (result.isOK()) {
            bool isSuccess;
            std::string responseMessage;
            std::tie(isSuccess, responseMessage) = result.getValue();

            _success = isSuccess;
            return responseMessage;
        }
        return result.getStatus();
    }

    /**
     * Returns true if the conversation has completed successfully.
     */
    bool isSuccess() const {
        return _success;
    }

    /** Returns which database contains the user which authentication is being performed against. */
    StringData getAuthenticationDatabase() const;

    /**
     * Flexible bag of options for a saslStart command.
     */
    virtual Status setOptions(BSONObj options) {
        // Be default, ignore any options provided.
        return Status::OK();
    }

    virtual boost::optional<unsigned int> currentStep() const = 0;
    virtual boost::optional<unsigned int> totalSteps() const = 0;

    /**
     * Create a UserRequest to send to AuthorizationSession.
     */
    virtual UserRequest getUserRequest() const {
        return UserRequest(UserName(getPrincipalName(), getAuthenticationDatabase()), boost::none);
    }

protected:
    /**
     * Mechanism provided step implementation.
     * On failure, returns a non-OK status. On success, returns a tuple consisting of
     * a boolean indicating whether the mechanism has completed, and the string
     * containing the server's response to the client.
     */
    virtual StatusWith<std::tuple<bool, std::string>> stepImpl(OperationContext* opCtx,
                                                               StringData input) = 0;

    bool _success = false;
    std::string _principalName;
    std::string _authenticationDatabase;
};

/** Base class for server mechanism factories. */
class ServerFactoryBase : public SaslServerCommonBase {
public:
    explicit ServerFactoryBase(ServiceContext*) {}
    ServerFactoryBase() = default;

    /**
     * Returns if the factory is capable of producing a server mechanism object which could
     * authenticate the provided user.
     */
    virtual bool canMakeMechanismForUser(const User* user) const = 0;

    /** Produces a unique_ptr containing a server SASL mechanism.*/
    std::unique_ptr<ServerMechanismBase> create(std::string authenticationDatabase) {
        std::unique_ptr<ServerMechanismBase> rv(
            this->createImpl(std::move(authenticationDatabase)));
        invariant(rv->mechanismName() == this->mechanismName());
        return rv;
    }

private:
    virtual ServerMechanismBase* createImpl(std::string authenticationDatabase) = 0;
};

/** Instantiates a class which provides runtime access to Policy properties. */
template <typename Policy>
class MakeServerMechanism : public ServerMechanismBase {
public:
    explicit MakeServerMechanism(std::string authenticationDatabase)
        : ServerMechanismBase(std::move(authenticationDatabase)) {}
    virtual ~MakeServerMechanism() = default;

    using policy_type = Policy;

    StringData mechanismName() const final {
        return policy_type::getName();
    }

    SecurityPropertySet properties() const final {
        return policy_type::getProperties();
    }

    int securityLevel() const final {
        return policy_type::securityLevel();
    }

    bool isInternalAuthMech() const final {
        return policy_type::isInternalAuthMech();
    }
};

/** Instantiates a class which provides runtime access to Policy properties. */
template <typename ServerMechanism>
class MakeServerFactory : public ServerFactoryBase {
public:
    static_assert(std::is_base_of<MakeServerMechanism<typename ServerMechanism::policy_type>,
                                  ServerMechanism>::value,
                  "MakeServerFactory must be instantiated with a ServerMechanism derived from "
                  "MakeServerMechanism");

    using mechanism_type = ServerMechanism;
    using policy_type = typename ServerMechanism::policy_type;

    explicit MakeServerFactory(ServiceContext*) {}
    MakeServerFactory() = default;

    virtual ServerMechanism* createImpl(std::string authenticationDatabase) override {
        return new ServerMechanism(std::move(authenticationDatabase));
    }

    StringData mechanismName() const final {
        return policy_type::getName();
    }

    SecurityPropertySet properties() const final {
        return policy_type::getProperties();
    }

    int securityLevel() const final {
        return policy_type::securityLevel();
    }

    bool isInternalAuthMech() const final {
        return policy_type::isInternalAuthMech();
    }
};

/**
 * Tracks server-side SASL mechanisms. Mechanisms' factories are registered with this class during
 * server initialization. During authentication, this class finds a factory, to obtains a
 * mechanism from. Also capable of producing a list of mechanisms which would be valid for a
 * particular user.
 */
class SASLServerMechanismRegistry {
public:
    static SASLServerMechanismRegistry& get(ServiceContext* serviceContext);
    static void set(ServiceContext* service, std::unique_ptr<SASLServerMechanismRegistry> registry);

    /**
     * Intialize the registry with a list of enabled mechanisms.
     */
    explicit SASLServerMechanismRegistry(ServiceContext* svcCtx,
                                         std::vector<std::string> enabledMechanisms);

    /**
     * Sets a new list of enabled mechanisms - used in testing.
     */
    void setEnabledMechanisms(std::vector<std::string> enabledMechanisms);

    /**
     * Produces a list of SASL mechanisms which can be used to authenticate as a user.
     * This will populate 'builder' with an Array called saslSupportedMechs containing each
     * mechanism the user supports.
     */
    void advertiseMechanismNamesForUser(OperationContext* opCtx,
                                        UserName userName,
                                        BSONObjBuilder* builder);

    /**
     * Gets a mechanism object which corresponds to the provided name.
     * The mechanism will be able to authenticate users which exist on the
     * "authenticationDatabase".
     */
    StatusWith<std::unique_ptr<ServerMechanismBase>> getServerMechanism(
        StringData mechanismName, std::string authenticationDatabase);

    /**
     * Registers a factory T to produce a type of SASL mechanism.
     * If 'internal' is false, the factory will be used to create mechanisms for authentication
     * attempts on $external. Otherwise, the mechanism may be used for any database but $external.
     * This allows distinct mechanisms with the same name for the servers' different authentication
     * domains.
     */
    enum ValidateGlobalMechanisms : bool {
        kValidateGlobalMechanisms = true,
        kNoValidateGlobalMechanisms = false
    };
    template <typename T>
    bool registerFactory(
        ValidateGlobalMechanisms validateGlobalConfig = kValidateGlobalMechanisms) {
        using policy_type = typename T::policy_type;
        auto mechName = policy_type::getName();

        if (validateGlobalConfig &&
            (!policy_type::isInternalAuthMech() && !_mechanismSupportedByConfig(mechName))) {
            return false;
        }

        auto& list = _getMapRef(T::isInternal);
        list.emplace_back(std::make_unique<T>(_svcCtx));
        std::stable_sort(list.begin(), list.end(), [](const auto& a, const auto& b) {
            return (a->securityLevel() > b->securityLevel());
        });

        return true;
    }

    std::vector<std::string> getMechanismNames() const;

private:
    using MechList = std::vector<std::unique_ptr<ServerFactoryBase>>;

    MechList& _getMapRef(StringData dbName) {
        return _getMapRef(dbName != DatabaseName::kExternal.db());
    }

    MechList& _getMapRef(bool internal) {
        if (internal) {
            return _internalMechs;
        }
        return _externalMechs;
    }

    bool _mechanismSupportedByConfig(StringData mechName) const;

    ServiceContext* _svcCtx = nullptr;

    // Stores factories which make mechanisms for all databases other than $external
    MechList _internalMechs;
    // Stores factories which make mechanisms exclusively for $external
    MechList _externalMechs;

    std::vector<std::string> _enabledMechanisms;
};

template <typename Factory>
class GlobalSASLMechanismRegisterer {
private:
    boost::optional<ServiceContext::ConstructorActionRegisterer> registerer;

public:
    GlobalSASLMechanismRegisterer() {
        registerer.emplace(std::string(typeid(Factory).name()),
                           std::vector<std::string>{"CreateSASLServerMechanismRegistry"},
                           std::vector<std::string>{"ValidateSASLServerMechanismRegistry"},
                           [](ServiceContext* service) {
                               SASLServerMechanismRegistry::get(service).registerFactory<Factory>();
                           });
    }
};
}  // namespace mongo
