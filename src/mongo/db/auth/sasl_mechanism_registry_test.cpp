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

#include "mongo/db/auth/sasl_mechanism_registry.h"

#include "mongo/bson/bsonmisc.h"
#include "mongo/crypto/mechanism_scram.h"
#include "mongo/crypto/sha256_block.h"
#include "mongo/db/auth/auth_name.h"
#include "mongo/db/auth/authorization_backend_interface.h"
#include "mongo/db/auth/authorization_backend_local.h"
#include "mongo/db/auth/authorization_backend_mock.h"
#include "mongo/db/auth/authorization_client_handle_shard.h"
#include "mongo/db/auth/authorization_manager.h"
#include "mongo/db/auth/authorization_manager_factory_mock.h"
#include "mongo/db/auth/authorization_manager_impl.h"
#include "mongo/db/auth/authorization_router_impl.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/repl/replication_coordinator_mock.h"
#include "mongo/db/service_context_test_fixture.h"
#include "mongo/db/service_entry_point_shard_role.h"
#include "mongo/unittest/unittest.h"

#include <absl/container/node_hash_map.h>
#include <boost/move/utility_core.hpp>
#include <boost/none.hpp>
#include <boost/optional/optional.hpp>

namespace mongo {
namespace {

TEST(SecurityProperty, emptyHasEmptyProperties) {
    SecurityPropertySet set(SecurityPropertySet{});
    ASSERT_TRUE(set.hasAllProperties(set));
    ASSERT_FALSE(set.hasAllProperties(SecurityPropertySet{SecurityProperty::kMutualAuth}));
    ASSERT_FALSE(set.hasAllProperties(SecurityPropertySet{SecurityProperty::kNoPlainText}));
}

TEST(SecurityProperty, mutualHasMutalAndEmptyProperties) {
    SecurityPropertySet set(SecurityPropertySet{SecurityProperty::kMutualAuth});
    ASSERT_TRUE(set.hasAllProperties(SecurityPropertySet{}));
    ASSERT_TRUE(set.hasAllProperties(SecurityPropertySet{SecurityProperty::kMutualAuth}));
    ASSERT_FALSE(set.hasAllProperties(SecurityPropertySet{SecurityProperty::kNoPlainText}));
}

TEST(SecurityProperty, mutualAndPlainHasAllSubsets) {
    SecurityPropertySet set{SecurityProperty::kMutualAuth, SecurityProperty::kNoPlainText};
    ASSERT_TRUE(set.hasAllProperties(SecurityPropertySet{}));
    ASSERT_TRUE(set.hasAllProperties(SecurityPropertySet{SecurityProperty::kMutualAuth}));
    ASSERT_TRUE(set.hasAllProperties(SecurityPropertySet{SecurityProperty::kNoPlainText}));
    ASSERT_TRUE(set.hasAllProperties(
        SecurityPropertySet{SecurityProperty::kMutualAuth, SecurityProperty::kNoPlainText}));
}

template <typename Policy>
class BaseMockMechanism : public MakeServerMechanism<Policy> {
public:
    explicit BaseMockMechanism(std::string authenticationDatabase)
        : MakeServerMechanism<Policy>(std::move(authenticationDatabase)) {}

    boost::optional<unsigned int> currentStep() const override {
        return boost::none;
    }

    boost::optional<unsigned int> totalSteps() const override {
        return boost::none;
    }

protected:
    StatusWith<std::tuple<bool, std::string>> stepImpl(OperationContext* opCtx,
                                                       StringData input) final {
        return std::make_tuple(true, std::string());
    }
};

template <typename Policy, bool argIsInternal>
class BaseMockMechanismFactory : public MakeServerFactory<Policy> {
public:
    using MakeServerFactory<Policy>::MakeServerFactory;
    static constexpr bool isInternal = argIsInternal;
    bool canMakeMechanismForUser(const User* user) const final {
        return true;
    }
};

// Policy for a hypothetical "FOO" SASL mechanism.
struct FooPolicy {
    static constexpr StringData getName() {
        return "FOO"_sd;
    }

    static constexpr int securityLevel() {
        return 0;
    }

    static constexpr bool isInternalAuthMech() {
        return false;
    }

    // This mech is kind of dangerous, it sends plaintext passwords across the wire.
    static SecurityPropertySet getProperties() {
        return SecurityPropertySet{SecurityProperty::kMutualAuth};
    }
};

class FooMechanism : public BaseMockMechanism<FooPolicy> {
public:
    using BaseMockMechanism<FooPolicy>::BaseMockMechanism;
};

template <bool argIsInternal>
class FooMechanismFactory : public BaseMockMechanismFactory<FooMechanism, argIsInternal> {
public:
    using BaseMockMechanismFactory<FooMechanism, argIsInternal>::BaseMockMechanismFactory;
};

// Policy for a hypothetical "BAR" SASL mechanism.
struct BarPolicy {
    static constexpr StringData getName() {
        return "BAR"_sd;
    }

    static constexpr int securityLevel() {
        return 1;
    }

    static constexpr bool isInternalAuthMech() {
        return false;
    }

    static SecurityPropertySet getProperties() {
        return SecurityPropertySet{SecurityProperty::kMutualAuth, SecurityProperty::kNoPlainText};
    }
};

class BarMechanism : public BaseMockMechanism<BarPolicy> {
public:
    using BaseMockMechanism<BarPolicy>::BaseMockMechanism;
};

template <bool argIsInternal>
class BarMechanismFactory : public BaseMockMechanismFactory<BarMechanism, argIsInternal> {
public:
    using BaseMockMechanismFactory<BarMechanism, argIsInternal>::BaseMockMechanismFactory;
};

// Policy for a hypothetical "InternalAuth" SASL mechanism.
struct InternalAuthPolicy {
    static constexpr StringData getName() {
        return "InternalAuth"_sd;
    }

    static constexpr int securityLevel() {
        return 2;
    }

    static constexpr bool isInternalAuthMech() {
        return true;
    }

    static SecurityPropertySet getProperties() {
        return SecurityPropertySet{SecurityProperty::kMutualAuth, SecurityProperty::kNoPlainText};
    }
};

class InternalAuthMechanism : public BaseMockMechanism<InternalAuthPolicy> {
public:
    using BaseMockMechanism<InternalAuthPolicy>::BaseMockMechanism;
};

class InternalAuthMechanismFactory : public BaseMockMechanismFactory<InternalAuthMechanism, true> {
public:
    using BaseMockMechanismFactory<InternalAuthMechanism, true>::BaseMockMechanismFactory;
};

class MechanismRegistryTest : public ServiceContextTest {
public:
    MechanismRegistryTest()
        : opCtx(makeOperationContext()),
          // By default the registry is initialized with all mechanisms enabled.
          registry(opCtx->getService(), {"FOO", "BAR", "InternalAuth"}) {

        auto globalAuthzManagerFactory = std::make_unique<AuthorizationManagerFactoryMock>();
        AuthorizationManager::set(getService(),
                                  globalAuthzManagerFactory->createShard(getService()));

        auth::AuthorizationBackendInterface::set(
            getService(), globalAuthzManagerFactory->createBackendInterface(getService()));
        authzBackend = reinterpret_cast<auth::AuthorizationBackendMock*>(
            auth::AuthorizationBackendInterface::get(getService()));

        // Initialize the serviceEntryPoint so that DBDirectClient can function.
        getService()->setServiceEntryPoint(std::make_unique<ServiceEntryPointShardRole>());

        // Setup the repl coordinator in standalone mode so we don't need an oplog etc.
        repl::ReplicationCoordinator::set(getServiceContext(),
                                          std::make_unique<repl::ReplicationCoordinatorMock>(
                                              getServiceContext(), repl::ReplSettings()));

        ASSERT_OK(authzBackend->insert(
            opCtx.get(),
            NamespaceString::createNamespaceString_forTest("admin.system.users"),
            BSON("_id" << "test.sajack"
                       << "user"
                       << "sajack"
                       << "db"
                       << "test"
                       << "credentials"
                       << BSON("SCRAM-SHA-256" << scram::Secrets<SHA256Block>::generateCredentials(
                                   "sajack‍", 15000))
                       << "roles" << BSONArray()),
            BSONObj()));


        ASSERT_OK(authzBackend->insert(
            opCtx.get(),
            NamespaceString::createNamespaceString_forTest("admin.system.users"),
            BSON("_id" << "$external.sajack"
                       << "user"
                       << "sajack"
                       << "db"
                       << "$external"
                       << "credentials" << BSON("external" << true) << "roles" << BSONArray()),
            BSONObj()));

        std::unique_ptr<UserRequest> systemLocal =
            std::make_unique<UserRequestGeneral>(UserName("__system"_sd, "local"_sd), boost::none);
        internalSecurity.setUser(std::make_shared<UserHandle>(User(std::move(systemLocal))));
    }

    BSONObj getMechsFor(const UserName user) {
        BSONObjBuilder builder;
        registry.advertiseMechanismNamesForUser(opCtx.get(), user, &builder);
        return builder.obj();
    }

    ServiceContext::UniqueOperationContext opCtx;
    auth::AuthorizationBackendMock* authzBackend;

    SASLServerMechanismRegistry registry;

    const UserName internalSajack = {"sajack"_sd, "test"_sd};
    const UserName externalSajack = {"sajack"_sd, DatabaseName::kExternal.db(omitTenant)};
};

TEST_F(MechanismRegistryTest, acquireInternalMechanism) {
    registry.registerFactory<FooMechanismFactory<true>>(
        SASLServerMechanismRegistry::kNoValidateGlobalMechanisms);

    auto swMechanism = registry.getServerMechanism(FooPolicy::getName(), "test");
    ASSERT_OK(swMechanism.getStatus());
}

TEST_F(MechanismRegistryTest, cantAcquireInternalMechanismOnExternal) {
    registry.registerFactory<FooMechanismFactory<true>>(
        SASLServerMechanismRegistry::kNoValidateGlobalMechanisms);

    auto swMechanism = registry.getServerMechanism(FooPolicy::getName(), "$external");
    ASSERT_NOT_OK(swMechanism.getStatus());
}

TEST_F(MechanismRegistryTest, acquireExternalMechanism) {
    registry.registerFactory<FooMechanismFactory<false>>(
        SASLServerMechanismRegistry::kNoValidateGlobalMechanisms);

    auto swMechanism = registry.getServerMechanism(FooPolicy::getName(), "$external");
    ASSERT_OK(swMechanism.getStatus());
}

TEST_F(MechanismRegistryTest, cantAcquireExternalMechanismOnInternal) {
    registry.registerFactory<FooMechanismFactory<false>>(
        SASLServerMechanismRegistry::kNoValidateGlobalMechanisms);

    auto swMechanism = registry.getServerMechanism(FooPolicy::getName(), "test");
    ASSERT_NOT_OK(swMechanism.getStatus());
}


TEST_F(MechanismRegistryTest, invalidUserCantAdvertiseMechs) {
    registry.registerFactory<FooMechanismFactory<true>>(
        SASLServerMechanismRegistry::kNoValidateGlobalMechanisms);

    ASSERT_BSONOBJ_EQ(BSONObj(), getMechsFor(UserName("noSuchUser"_sd, "test"_sd)));
}

TEST_F(MechanismRegistryTest, strongMechCanAdvertise) {
    registry.registerFactory<BarMechanismFactory<true>>(
        SASLServerMechanismRegistry::kNoValidateGlobalMechanisms);
    registry.registerFactory<BarMechanismFactory<false>>(
        SASLServerMechanismRegistry::kNoValidateGlobalMechanisms);

    ASSERT_BSONOBJ_EQ(BSON("saslSupportedMechs" << BSON_ARRAY("BAR")), getMechsFor(internalSajack));
    ASSERT_BSONOBJ_EQ(BSON("saslSupportedMechs" << BSON_ARRAY("BAR")), getMechsFor(externalSajack));
}

TEST_F(MechanismRegistryTest, weakMechCannotAdvertiseOnInternal) {
    registry.registerFactory<FooMechanismFactory<true>>(
        SASLServerMechanismRegistry::kNoValidateGlobalMechanisms);

    ASSERT_BSONOBJ_EQ(BSON("saslSupportedMechs" << BSONArray()), getMechsFor(internalSajack));
}

TEST_F(MechanismRegistryTest, weakMechCanAdvertiseOnExternal) {
    registry.registerFactory<FooMechanismFactory<false>>(
        SASLServerMechanismRegistry::kNoValidateGlobalMechanisms);

    ASSERT_BSONOBJ_EQ(BSON("saslSupportedMechs" << BSON_ARRAY("FOO")), getMechsFor(externalSajack));
}

TEST_F(MechanismRegistryTest, internalAuth) {
    registry.setEnabledMechanisms({"BAR"});

    registry.registerFactory<BarMechanismFactory<true>>(
        SASLServerMechanismRegistry::kValidateGlobalMechanisms);
    registry.registerFactory<InternalAuthMechanismFactory>(
        SASLServerMechanismRegistry::kValidateGlobalMechanisms);

    ASSERT_BSONOBJ_EQ(BSON("saslSupportedMechs" << BSON_ARRAY("BAR")), getMechsFor(internalSajack));
    ASSERT_BSONOBJ_EQ(BSON("saslSupportedMechs" << BSON_ARRAY("InternalAuth" << "BAR")),
                      getMechsFor((*internalSecurity.getUser())->getName()));

    registry.setEnabledMechanisms({"BAR", "InternalAuth"});
    ASSERT_BSONOBJ_EQ(BSON("saslSupportedMechs" << BSON_ARRAY("InternalAuth" << "BAR")),
                      getMechsFor(internalSajack));
}

}  // namespace
}  // namespace mongo
