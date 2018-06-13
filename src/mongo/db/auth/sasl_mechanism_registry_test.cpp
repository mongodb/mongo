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

#include "mongo/db/auth/sasl_mechanism_registry.h"
#include "mongo/crypto/mechanism_scram.h"
#include "mongo/db/auth/authorization_manager.h"
#include "mongo/db/auth/authorization_manager_impl.h"
#include "mongo/db/auth/authz_manager_external_state_mock.h"
#include "mongo/db/operation_context_noop.h"
#include "mongo/db/service_context_noop.h"
#include "mongo/unittest/unittest.h"

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

// Policy for a hypothetical "FOO" SASL mechanism.
struct FooPolicy {
    static constexpr StringData getName() {
        return "FOO"_sd;
    }

    // This mech is kind of dangerous, it sends plaintext passwords across the wire.
    static SecurityPropertySet getProperties() {
        return SecurityPropertySet{SecurityProperty::kMutualAuth};
    }
};

class FooMechanism : public MakeServerMechanism<FooPolicy> {
public:
    explicit FooMechanism(std::string authenticationDatabase)
        : MakeServerMechanism<FooPolicy>(std::move(authenticationDatabase)) {}

protected:
    StatusWith<std::tuple<bool, std::string>> stepImpl(OperationContext* opCtx,
                                                       StringData input) final {
        return std::make_tuple(true, std::string());
    }
};

template <bool argIsInternal>
class FooMechanismFactory : public MakeServerFactory<FooMechanism> {
public:
    static constexpr bool isInternal = argIsInternal;
    bool canMakeMechanismForUser(const User* user) const final {
        return true;
    }
};

// Policy for a hypothetical "BAR" SASL mechanism.
struct BarPolicy {
    static constexpr StringData getName() {
        return "BAR"_sd;
    }

    static SecurityPropertySet getProperties() {
        return SecurityPropertySet{SecurityProperty::kMutualAuth, SecurityProperty::kNoPlainText};
    }
};

class BarMechanism : public MakeServerMechanism<BarPolicy> {
public:
    explicit BarMechanism(std::string authenticationDatabase)
        : MakeServerMechanism<BarPolicy>(std::move(authenticationDatabase)) {}

protected:
    StatusWith<std::tuple<bool, std::string>> stepImpl(OperationContext* opCtx,
                                                       StringData input) final {
        return std::make_tuple(true, std::string());
    }
};

template <bool argIsInternal>
class BarMechanismFactory : public MakeServerFactory<BarMechanism> {
public:
    static constexpr bool isInternal = argIsInternal;
    bool canMakeMechanismForUser(const User* user) const final {
        return true;
    }
};


class MechanismRegistryTest : public mongo::unittest::Test {
public:
    MechanismRegistryTest()
        : opClient(serviceContext.makeClient("mechanismRegistryTest")),
          opCtx(serviceContext.makeOperationContext(opClient.get())),
          authManagerExternalState(new AuthzManagerExternalStateMock()),
          authManager(new AuthorizationManagerImpl(
              std::unique_ptr<AuthzManagerExternalStateMock>(authManagerExternalState),
              AuthorizationManagerImpl::InstallMockForTestingOrAuthImpl{})) {
        AuthorizationManager::set(&serviceContext,
                                  std::unique_ptr<AuthorizationManager>(authManager));

        ASSERT_OK(authManagerExternalState->updateOne(
            opCtx.get(),
            AuthorizationManager::versionCollectionNamespace,
            AuthorizationManager::versionDocumentQuery,
            BSON("$set" << BSON(AuthorizationManager::schemaVersionFieldName
                                << AuthorizationManager::schemaVersion26Final)),
            true,
            BSONObj()));

        ASSERT_OK(authManagerExternalState->insert(
            opCtx.get(),
            NamespaceString("admin.system.users"),
            BSON("_id"
                 << "test.sajack"
                 << "user"
                 << "sajack"
                 << "db"
                 << "test"
                 << "credentials"
                 << BSON("SCRAM-SHA-256"
                         << scram::Secrets<SHA256Block>::generateCredentials("sajack‍", 15000))
                 << "roles"
                 << BSONArray()),
            BSONObj()));


        ASSERT_OK(authManagerExternalState->insert(opCtx.get(),
                                                   NamespaceString("admin.system.users"),
                                                   BSON("_id"
                                                        << "$external.sajack"
                                                        << "user"
                                                        << "sajack"
                                                        << "db"
                                                        << "$external"
                                                        << "credentials"
                                                        << BSON("external" << true)
                                                        << "roles"
                                                        << BSONArray()),
                                                   BSONObj()));
    }

    ServiceContextNoop serviceContext;
    ServiceContext::UniqueClient opClient;
    ServiceContext::UniqueOperationContext opCtx;
    AuthzManagerExternalStateMock* authManagerExternalState;
    AuthorizationManager* authManager;

    SASLServerMechanismRegistry registry;
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

    BSONObjBuilder builder;

    registry.advertiseMechanismNamesForUser(opCtx.get(),
                                            BSON("isMaster" << 1 << "saslSupportedMechs"
                                                            << "test.noSuchUser"),
                                            &builder);

    ASSERT_BSONOBJ_EQ(BSONObj(), builder.obj());
}

TEST_F(MechanismRegistryTest, strongMechCanAdvertise) {
    registry.registerFactory<BarMechanismFactory<true>>(
        SASLServerMechanismRegistry::kNoValidateGlobalMechanisms);
    registry.registerFactory<BarMechanismFactory<false>>(
        SASLServerMechanismRegistry::kNoValidateGlobalMechanisms);

    BSONObjBuilder builder;
    registry.advertiseMechanismNamesForUser(opCtx.get(),
                                            BSON("isMaster" << 1 << "saslSupportedMechs"
                                                            << "test.sajack"),
                                            &builder);

    BSONObj obj = builder.done();
    ASSERT_BSONOBJ_EQ(BSON("saslSupportedMechs" << BSON_ARRAY("BAR")), obj);

    BSONObjBuilder builderExternal;
    registry.advertiseMechanismNamesForUser(opCtx.get(),
                                            BSON("isMaster" << 1 << "saslSupportedMechs"
                                                            << "$external.sajack"),
                                            &builderExternal);

    BSONObj objExternal = builderExternal.done();
    ASSERT_BSONOBJ_EQ(BSON("saslSupportedMechs" << BSON_ARRAY("BAR")), objExternal);
}

TEST_F(MechanismRegistryTest, weakMechCannotAdvertiseOnInternal) {
    registry.registerFactory<FooMechanismFactory<true>>(
        SASLServerMechanismRegistry::kNoValidateGlobalMechanisms);

    BSONObjBuilder builder;
    registry.advertiseMechanismNamesForUser(opCtx.get(),
                                            BSON("isMaster" << 1 << "saslSupportedMechs"
                                                            << "test.sajack"),
                                            &builder);


    BSONObj obj = builder.done();

    ASSERT_BSONOBJ_EQ(BSON("saslSupportedMechs" << BSONArray()), obj);
}

TEST_F(MechanismRegistryTest, weakMechCanAdvertiseOnExternal) {
    registry.registerFactory<FooMechanismFactory<false>>(
        SASLServerMechanismRegistry::kNoValidateGlobalMechanisms);

    BSONObjBuilder builder;
    registry.advertiseMechanismNamesForUser(opCtx.get(),
                                            BSON("isMaster" << 1 << "saslSupportedMechs"
                                                            << "$external.sajack"),
                                            &builder);

    BSONObj obj = builder.done();

    ASSERT_BSONOBJ_EQ(BSON("saslSupportedMechs" << BSON_ARRAY("FOO")), obj);
}


}  // namespace
}  // namespace mongo
