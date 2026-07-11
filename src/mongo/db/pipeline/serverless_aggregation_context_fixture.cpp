// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/pipeline/serverless_aggregation_context_fixture.h"

#include "mongo/db/topology/sharding_state.h"

namespace mongo {

ServerlessAggregationContextFixture::ServerlessAggregationContextFixture()
    : AggregationContextFixture(NamespaceString::createNamespaceString_forTest(
          TenantId(OID::gen()), "test", "pipeline_test")) {
    ShardingState::create(getServiceContext());
}

}  // namespace mongo
