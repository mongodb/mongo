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

#pragma once

#include <memory>
#include <unordered_map>

#include "mongo/base/string_data.h"
#include "mongo/db/auth/authorization_manager.h"
#include "mongo/db/commands.h"
#include "mongo/db/commands/test_commands_enabled.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/service_context.h"

namespace mongo {

class User;

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
     * Standard method in mongodb for determining if "authenticatedUser" may act as "requestedUser."
     *
     * The standard rule in MongoDB is simple.  The authenticated user name must be the same as the
     * requested user name.
     */
    virtual bool isAuthorizedToActAs(StringData requestedUser, StringData authenticatedUser) {
        return requestedUser == authenticatedUser;
    }

    /**
     * Performs a single step of a SASL exchange. Takes an input provided by a client,
     * and either returns an error, or a response to be sent back.
     */
    StatusWith<std::string> step(OperationContext* opCtx, StringData input) {
        auto result = stepImpl(opCtx, input);
        if (result.isOK()) {
            bool isDone;
            std::string responseMessage;
            std::tie(isDone, responseMessage) = result.getValue();

            _done = isDone;
            return responseMessage;
        }
        return result.getStatus();
    }

    /**
     * Returns true if the conversation has completed.
     * Note that this does not mean authentication succeeded!
     * An error may have occurred.
     */
    bool isDone() const {
        return _done;
    }

    /** Returns which database contains the user which authentication is being performed against. */
    StringData getAuthenticationDatabase() const {
        if (getTestCommandsEnabled() && _authenticationDatabase == "admin" &&
            getPrincipalName() == internalSecurity.user->getName().getUser()) {
            // Allows authenticating as the internal user against the admin database.  This is to
            // support the auth passthrough test framework on mongos (since you can't use the local
            // database on a mongos, so you can't auth as the internal user without this).
            return internalSecurity.user->getName().getDB();
        } else {
            return _authenticationDatabase;
        }
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

    bool _done = false;
    std::string _principalName;
    std::string _authenticationDatabase;
};

/** Base class for server mechanism factories. */
class ServerFactoryBase : public SaslServerCommonBase {
public:
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

    virtual ServerMechanism* createImpl(std::string authenticationDatabase) override {
        return new ServerMechanism(std::move(authenticationDatabase));
    }

    StringData mechanismName() const final {
        return policy_type::getName();
    }

    SecurityPropertySet properties() const final {
        return policy_type::getProperties();
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
     * Produces a list of SASL mechanisms which can be used to authenticate as a user.
     * If isMasterCmd contains a field with a username called 'saslSupportedMechs',
     * will populate 'builder' with an Array called saslSupportedMechs containing each mechanism the
     * user supports.
     */
    void advertiseMechanismNamesForUser(OperationContext* opCtx,
                                        const BSONObj& isMasterCmd,
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

        // Always allow SCRAM-SHA-1 to pass to the first sasl step since we need to
        // handle internal user authentication, SERVER-16534
        if (validateGlobalConfig &&
            (mechName != "SCRAM-SHA-1" && !_mechanismSupportedByConfig(mechName))) {
            return false;
        }

        invariant(
            _getMapRef(T::isInternal).emplace(mechName.toString(), std::make_unique<T>()).second);
        return true;
    }

private:
    stdx::unordered_map<std::string, std::unique_ptr<ServerFactoryBase>>& _getMapRef(
        StringData dbName) {
        return _getMapRef(dbName != "$external"_sd);
    }

    stdx::unordered_map<std::string, std::unique_ptr<ServerFactoryBase>>& _getMapRef(
        bool internal) {
        if (internal) {
            return _internalMap;
        }
        return _externalMap;
    }

    bool _mechanismSupportedByConfig(StringData mechName);

    // Stores factories which make mechanisms for all databases other than $external
    stdx::unordered_map<std::string, std::unique_ptr<ServerFactoryBase>> _internalMap;
    // Stores factories which make mechanisms exclusively for $external
    stdx::unordered_map<std::string, std::unique_ptr<ServerFactoryBase>> _externalMap;
};

}  // namespace mongo
