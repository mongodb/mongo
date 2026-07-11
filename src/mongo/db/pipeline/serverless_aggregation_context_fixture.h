// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/db/pipeline/aggregation_context_fixture.h"
#include "mongo/util/modules.h"

namespace mongo {

class ServerlessAggregationContextFixture : public AggregationContextFixture {
protected:
    ServerlessAggregationContextFixture();

    const std::string _targetDb = "test";
    const std::string _targetColl = "target_collection";
};

}  // namespace mongo
