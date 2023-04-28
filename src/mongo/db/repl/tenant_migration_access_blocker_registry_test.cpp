/**
 *    Copyright (C) 2023-present MongoDB, Inc.
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

#include "mongo/db/repl/tenant_migration_access_blocker_registry.h"
#include "mongo/db/service_context_test_fixture.h"
#include "mongo/idl/server_parameter_test_util.h"

namespace mongo {

class TenantMigrationAccessBlockerRegistryTest : public ServiceContextTest {
public:
    void setUp() {
        _opCtx = makeOperationContext();
        TenantMigrationAccessBlockerRegistry::get(getServiceContext()).startup();
    }

    void tearDown() {
        TenantMigrationAccessBlockerRegistry::get(getServiceContext()).shutDown();
    }

    OperationContext* opCtx() const {
        return _opCtx.get();
    }

private:
    ServiceContext::UniqueOperationContext _opCtx;
};

TEST_F(TenantMigrationAccessBlockerRegistryTest, AddAccessBlocker) {
    auto& registry = TenantMigrationAccessBlockerRegistry::get(getServiceContext());
    const auto uuid = UUID::gen();
    const auto tenant = TenantId{OID::gen()};
    const auto otherTenant = TenantId{OID::gen()};

    registry.add(
        tenant, std::make_shared<TenantMigrationRecipientAccessBlocker>(getServiceContext(), uuid));
    registry.add(tenant,
                 std::make_shared<TenantMigrationDonorAccessBlocker>(getServiceContext(), uuid));
    registry.add(otherTenant,
                 std::make_shared<TenantMigrationDonorAccessBlocker>(getServiceContext(), uuid));


    registry.addGlobalDonorAccessBlocker(
        std::make_shared<TenantMigrationDonorAccessBlocker>(getServiceContext(), uuid));
    ASSERT_THROWS_CODE(
        registry.addGlobalDonorAccessBlocker(
            std::make_shared<TenantMigrationDonorAccessBlocker>(getServiceContext(), uuid)),
        DBException,
        ErrorCodes::ConflictingOperationInProgress);

    ASSERT_THROWS_CODE(
        registry.add(
            tenant, std::make_shared<TenantMigrationDonorAccessBlocker>(getServiceContext(), uuid)),
        DBException,
        ErrorCodes::ConflictingServerlessOperation);
}

TEST_F(TenantMigrationAccessBlockerRegistryTest, RemoveAccessBlockersForMigration) {
    auto& registry = TenantMigrationAccessBlockerRegistry::get(getServiceContext());
    const auto uuid = UUID::gen();
    const auto tenant = TenantId{OID::gen()};

    registry.add(
        tenant, std::make_shared<TenantMigrationRecipientAccessBlocker>(getServiceContext(), uuid));
    registry.addGlobalDonorAccessBlocker(
        std::make_shared<TenantMigrationDonorAccessBlocker>(getServiceContext(), uuid));
    registry.add(tenant,
                 std::make_shared<TenantMigrationDonorAccessBlocker>(getServiceContext(), uuid));

    registry.removeAccessBlockersForMigration(
        uuid, TenantMigrationAccessBlocker::BlockerType::kRecipient);
    ASSERT(registry.getTenantMigrationAccessBlockerForTenantId(
        tenant, TenantMigrationAccessBlocker::BlockerType::kDonor));

    registry.removeAccessBlockersForMigration(UUID::gen(),
                                              TenantMigrationAccessBlocker::BlockerType::kDonor);
    ASSERT(registry.getTenantMigrationAccessBlockerForTenantId(
        tenant, TenantMigrationAccessBlocker::BlockerType::kDonor));

    registry.removeAccessBlockersForMigration(uuid,
                                              TenantMigrationAccessBlocker::BlockerType::kDonor);
    registry.removeAccessBlockersForMigration(
        uuid, TenantMigrationAccessBlocker::BlockerType::kRecipient);

    ASSERT_FALSE(registry.getTenantMigrationAccessBlockerForTenantId(
        tenant, TenantMigrationAccessBlocker::BlockerType::kDonor));
    ASSERT_FALSE(registry.getTenantMigrationAccessBlockerForTenantId(
        tenant, TenantMigrationAccessBlocker::BlockerType::kRecipient));
}

TEST_F(TenantMigrationAccessBlockerRegistryTest, GetAccessBlockerForDbName) {
    auto& registry = TenantMigrationAccessBlockerRegistry::get(getServiceContext());
    const auto tenant = TenantId{OID::gen()};
    const auto uuid = UUID::gen();

    ASSERT_FALSE(registry.getAccessBlockersForDbName(
        DatabaseName::createDatabaseName_forTest(boost::none, tenant.toString() + "_foo")));

    // If the MTAB registry is empty (such as in non-serverless deployments) using an invalid
    // tenantId simply returns boost::none. This is required as the underscore can be present in db
    // names for non-serverless deployments.
    ASSERT_FALSE(registry.getAccessBlockersForDbName(
        DatabaseName::createDatabaseName_forTest(boost::none, "tenant_foo")));

    auto globalAccessBlocker =
        std::make_shared<TenantMigrationDonorAccessBlocker>(getServiceContext(), UUID::gen());
    registry.addGlobalDonorAccessBlocker(globalAccessBlocker);

    // If the MTAB registry is not empty, it implies we have a serverless deployment. In that case
    // anything before the underscore should be a valid TenantId.
    ASSERT_THROWS_CODE(registry.getAccessBlockersForDbName(
                           DatabaseName::createDatabaseName_forTest(boost::none, "tenant_foo")),
                       DBException,
                       ErrorCodes::BadValue);

    ASSERT_EQ(registry
                  .getAccessBlockersForDbName(DatabaseName::createDatabaseName_forTest(
                      boost::none, tenant.toString() + "_foo"))
                  ->getDonorAccessBlocker(),
              globalAccessBlocker);
    ASSERT_FALSE(registry.getAccessBlockersForDbName(DatabaseName::kAdmin));

    auto recipientBlocker =
        std::make_shared<TenantMigrationRecipientAccessBlocker>(getServiceContext(), uuid);
    registry.add(tenant, recipientBlocker);
    ASSERT_EQ(registry
                  .getAccessBlockersForDbName(DatabaseName::createDatabaseName_forTest(
                      boost::none, tenant.toString() + "_foo"))
                  ->getDonorAccessBlocker(),
              globalAccessBlocker);
    ASSERT_EQ(registry
                  .getAccessBlockersForDbName(DatabaseName::createDatabaseName_forTest(
                      boost::none, tenant.toString() + "_foo"))
                  ->getRecipientAccessBlocker(),
              recipientBlocker);

    auto donorBlocker =
        std::make_shared<TenantMigrationDonorAccessBlocker>(getServiceContext(), uuid);
    registry.add(tenant, donorBlocker);
    ASSERT_EQ(registry
                  .getAccessBlockersForDbName(DatabaseName::createDatabaseName_forTest(
                      boost::none, tenant.toString() + "_foo"))
                  ->getDonorAccessBlocker(),
              donorBlocker);
    ASSERT_EQ(registry
                  .getAccessBlockersForDbName(DatabaseName::createDatabaseName_forTest(
                      boost::none, tenant.toString() + "_foo"))
                  ->getRecipientAccessBlocker(),
              recipientBlocker);

    {
        RAIIServerParameterControllerForTest multitenancyController("multitenancySupport", true);
        // since we enabled multitenancySupport, having underscore in the dbName won't throw because
        // we have constructed a DatabaseName with a TenantId. Therefore `my` won't be identified as
        // the tenantId.
        const DatabaseName validUnderscoreDbName =
            DatabaseName::createDatabaseName_forTest(tenant, "my_Db");
        ASSERT(registry.getAccessBlockersForDbName(validUnderscoreDbName) != boost::none);
    }
}

TEST_F(TenantMigrationAccessBlockerRegistryTest, GetTenantMigrationAccessBlockerForTenantId) {
    auto& registry = TenantMigrationAccessBlockerRegistry::get(getServiceContext());
    const auto tenant = TenantId{OID::gen()};
    const auto uuid = UUID::gen();

    auto recipientBlocker =
        std::make_shared<TenantMigrationRecipientAccessBlocker>(getServiceContext(), uuid);
    registry.add(tenant, recipientBlocker);
    registry.add(TenantId{OID::gen()},
                 std::make_shared<TenantMigrationDonorAccessBlocker>(getServiceContext(), uuid));

    ASSERT_FALSE(registry.getTenantMigrationAccessBlockerForTenantId(
        tenant, TenantMigrationAccessBlocker::BlockerType::kDonor));
    ASSERT_EQ(registry.getTenantMigrationAccessBlockerForTenantId(
                  tenant, TenantMigrationAccessBlocker::BlockerType::kRecipient),
              recipientBlocker);

    auto globalBlocker =
        std::make_shared<TenantMigrationDonorAccessBlocker>(getServiceContext(), uuid);
    registry.addGlobalDonorAccessBlocker(globalBlocker);
    ASSERT_EQ(registry.getTenantMigrationAccessBlockerForTenantId(
                  tenant, TenantMigrationAccessBlocker::BlockerType::kDonor),
              globalBlocker);

    auto donorBlocker =
        std::make_shared<TenantMigrationDonorAccessBlocker>(getServiceContext(), uuid);
    registry.add(tenant, donorBlocker);
    ASSERT_EQ(registry.getTenantMigrationAccessBlockerForTenantId(
                  tenant, TenantMigrationAccessBlocker::BlockerType::kDonor),
              donorBlocker);
}

TEST_F(TenantMigrationAccessBlockerRegistryTest, GetDonorAccessBlockersForMigration) {
    auto& registry = TenantMigrationAccessBlockerRegistry::get(getServiceContext());
    const auto uuid = UUID::gen();

    auto assertVector =
        [](const std::vector<std::shared_ptr<TenantMigrationDonorAccessBlocker>>& result,
           const std::vector<std::shared_ptr<TenantMigrationDonorAccessBlocker>>& expected) {
            // Order might change. Check that vector size are equals and all expected entries are
            // found.
            ASSERT_EQ(result.size(), expected.size());
            for (const auto& ptr : expected) {
                ASSERT_NE(std::find_if(result.begin(),
                                       result.end(),
                                       [ptr](const auto& entry) { return entry == ptr; }),
                          result.end());
            }
        };

    registry.add(
        TenantId{OID::gen()},
        std::make_shared<TenantMigrationDonorAccessBlocker>(getServiceContext(), UUID::gen()));
    ASSERT(registry.getDonorAccessBlockersForMigration(uuid).empty());
    auto donorBlocker =
        std::make_shared<TenantMigrationDonorAccessBlocker>(getServiceContext(), uuid);
    registry.add(TenantId{OID::gen()}, donorBlocker);
    assertVector(registry.getDonorAccessBlockersForMigration(uuid), {donorBlocker});

    auto globalBlocker =
        std::make_shared<TenantMigrationDonorAccessBlocker>(getServiceContext(), uuid);
    registry.addGlobalDonorAccessBlocker(globalBlocker);
    assertVector(registry.getDonorAccessBlockersForMigration(uuid), {globalBlocker, donorBlocker});

    ASSERT(registry.getDonorAccessBlockersForMigration(UUID::gen()).empty());

    registry.add(TenantId{OID::gen()},
                 std::make_shared<TenantMigrationDonorAccessBlocker>(getServiceContext(), uuid));
    ASSERT_EQ(registry.getDonorAccessBlockersForMigration(uuid).size(), 3);
}

TEST_F(TenantMigrationAccessBlockerRegistryTest, GetRecipientAccessBlockersForMigration) {
    auto& registry = TenantMigrationAccessBlockerRegistry::get(getServiceContext());
    const auto uuid = UUID::gen();

    auto assertVector =
        [](const std::vector<std::shared_ptr<TenantMigrationRecipientAccessBlocker>>& result,
           const std::vector<std::shared_ptr<TenantMigrationRecipientAccessBlocker>>& expected) {
            // Order might change. Check that vector size is equal and all expected entries are
            // found.
            ASSERT_EQ(result.size(), expected.size());
            for (const auto& ptr : expected) {
                ASSERT_NE(std::find_if(result.begin(),
                                       result.end(),
                                       [ptr](const auto& entry) { return entry == ptr; }),
                          result.end());
            }
        };

    registry.add(
        TenantId{OID::gen()},
        std::make_shared<TenantMigrationRecipientAccessBlocker>(getServiceContext(), UUID::gen()));
    ASSERT(registry.getRecipientAccessBlockersForMigration(uuid).empty());

    auto recipientBlocker =
        std::make_shared<TenantMigrationRecipientAccessBlocker>(getServiceContext(), uuid);
    registry.add(TenantId{OID::gen()}, recipientBlocker);
    assertVector(registry.getRecipientAccessBlockersForMigration(uuid), {recipientBlocker});

    auto secondBlocker =
        std::make_shared<TenantMigrationRecipientAccessBlocker>(getServiceContext(), uuid);
    registry.add(TenantId{OID::gen()}, secondBlocker);
    assertVector(registry.getRecipientAccessBlockersForMigration(uuid),
                 {recipientBlocker, secondBlocker});
}

}  // namespace mongo
