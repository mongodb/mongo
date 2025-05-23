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

#include <boost/move/utility_core.hpp>
#include <boost/none.hpp>
#include <boost/optional/optional.hpp>
#include <boost/smart_ptr/intrusive_ptr.hpp>
#include <vector>


#include "mongo/base/string_data.h"
#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/oid.h"
#include "mongo/bson/simple_bsonobj_comparator.h"
#include "mongo/db/client.h"
#include "mongo/db/logical_time.h"
#include "mongo/db/query/query_knobs_gen.h"
#include "mongo/db/query/query_settings/query_settings_gen.h"
#include "mongo/db/query/query_settings/query_settings_manager.h"
#include "mongo/db/query/query_settings/query_settings_service.h"
#include "mongo/db/server_parameter.h"
#include "mongo/db/service_context_test_fixture.h"
#include "mongo/db/tenant_id.h"
#include "mongo/idl/server_parameter_test_util.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/namespace_string_util.h"
#include "mongo/util/serialization_context.h"

namespace mongo::query_settings {
namespace {

bool operator==(const QuerySettings& lhs, const QuerySettings& rhs) {
    return SimpleBSONObjComparator::kInstance.compare(lhs.toBSON(), rhs.toBSON()) == 0;
}

QuerySettings makeQuerySettings(const IndexHintSpecs& indexHints) {
    QuerySettings settings;
    if (!indexHints.empty()) {
        settings.setIndexHints(indexHints);
    }
    settings.setQueryFramework(mongo::QueryFrameworkControlEnum::kTrySbeEngine);
    return settings;
}
}  // namespace

static auto const kSerializationContext =
    SerializationContext{SerializationContext::Source::Command,
                         SerializationContext::CallerType::Request,
                         SerializationContext::Prefix::ExcludePrefix};

class QuerySettingsManagerTest : public ServiceContextTest {
public:
    static constexpr StringData kCollName = "exampleCol"_sd;
    static constexpr StringData kDbName = "foo"_sd;

    std::vector<QueryShapeConfiguration> getExampleQueryShapeConfigurations(TenantId tenantId) {
        NamespaceSpec ns;
        ns.setDb(DatabaseNameUtil::deserialize(tenantId, kDbName, kSerializationContext));
        ns.setColl(kCollName);

        const QuerySettings settings = makeQuerySettings({IndexHintSpec(ns, {IndexHint("a_1")})});
        QueryInstance queryA =
            BSON("find" << kCollName << "$db" << kDbName << "filter" << BSON("a" << 2));
        QueryInstance queryB =
            BSON("find" << kCollName << "$db" << kDbName << "filter" << BSON("a" << BSONNULL));
        return {makeQueryShapeConfiguration(settings, queryA, tenantId),
                makeQueryShapeConfiguration(settings, queryB, tenantId)};
    }

    QueryShapeConfiguration makeQueryShapeConfiguration(const QuerySettings& settings,
                                                        QueryInstance query,
                                                        boost::optional<TenantId> tenantId) {
        auto queryShapeHash = createRepresentativeInfo(opCtx(), query, tenantId).queryShapeHash;
        QueryShapeConfiguration result(queryShapeHash, settings);
        result.setRepresentativeQuery(query);
        return result;
    }

    void setUp() final {
        _opCtx = cc().makeOperationContext();
    }

    OperationContext* opCtx() {
        return _opCtx.get();
    }

    QuerySettingsManager& manager() {
        return _manager;
    }

    static NamespaceString nss(boost::optional<TenantId> tenantId) {
        static auto const kSerializationContext =
            SerializationContext{SerializationContext::Source::Command,
                                 SerializationContext::CallerType::Request,
                                 SerializationContext::Prefix::ExcludePrefix};

        return NamespaceStringUtil::deserialize(
            tenantId, kDbName, kCollName, kSerializationContext);
    }

private:
    ServiceContext::UniqueOperationContext _opCtx;
    QuerySettingsManager _manager;
};

TEST_F(QuerySettingsManagerTest, QuerySettingsLookup) {
    RAIIServerParameterControllerForTest multitenanyController("multitenancySupport", true);
    TenantId tenantId1(OID::fromTerm(1));
    TenantId tenantId2(OID::fromTerm(2));

    auto configs = getExampleQueryShapeConfigurations(tenantId1);
    manager().setAllQueryShapeConfigurations({{configs}, LogicalTime()}, tenantId1);

    // Ensure QuerySettingsManager returns boost::none when QuerySettings are not found.
    ASSERT_EQ(manager().getQuerySettingsForQueryShapeHash(query_shape::QueryShapeHash(), tenantId1),
              boost::none);

    // Ensure QuerySettingsManager returns a valid QuerySettings on lookup.
    ASSERT_EQ(manager()
                  .getQuerySettingsForQueryShapeHash(configs[1].getQueryShapeHash(), tenantId1)
                  ->querySettings,
              configs[1].getSettings());

    // Ensure QuerySettingsManager does not return a valid QuerySettings of 'tenantId1', when
    // performing lookup as 'tenantId2'.
    ASSERT_EQ(
        manager().getQuerySettingsForQueryShapeHash(configs[1].getQueryShapeHash(), tenantId2),
        boost::none);
}
}  // namespace mongo::query_settings
