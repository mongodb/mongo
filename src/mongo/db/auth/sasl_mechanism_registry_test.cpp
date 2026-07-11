// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

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

#include <string_view>

#include <absl/container/node_hash_map.h>
#include <boost/move/utility_core.hpp>
#include <boost/none.hpp>
#include <boost/optional/optional.hpp>

namespace mongo {
namespace {
using namespace std::literals::string_view_literals;

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

    boost::optional<std::uint32_t> currentStep() const override {
        return boost::none;
    }

    boost::optional<std::uint32_t> totalSteps() const override {
        return boost::none;
    }

protected:
    StatusWith<std::tuple<bool, std::string>> stepImpl(OperationContext* opCtx,
                                                       std::string_view input) final {
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
    static constexpr std::string_view getName() {
        return "FOO"sv;
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
    static constexpr std::string_view getName() {
        return "BAR"sv;
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
    static constexpr std::string_view getName() {
        return "InternalAuth"sv;
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
            std::make_unique<UserRequestGeneral>(UserName("__system"sv, "local"sv), boost::none);
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

    const UserName internalSajack = {"sajack"sv, "test"sv};
    const UserName externalSajack = {"sajack"sv, DatabaseName::kExternal.db(omitTenant)};
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

    // Unknown users now return the server's enabled mechanisms so the client can select one
    // that the server will accept. FOO lacks kNoPlainText so it is filtered out for internal
    // DBs, producing an empty saslSupportedMechs array (field present, contents empty).
    ASSERT_BSONOBJ_EQ(BSON("saslSupportedMechs" << BSONArray()),
                      getMechsFor(UserName("noSuchUser"sv, "test"sv)));
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
