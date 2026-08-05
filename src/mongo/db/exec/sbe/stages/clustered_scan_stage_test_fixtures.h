// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/bson/bsonobj.h"
#include "mongo/db/exec/sbe/expressions/compile_ctx.h"
#include "mongo/db/exec/sbe/sbe_plan_stage_test.h"
#include "mongo/db/query/multiple_collection_accessor.h"
#include "mongo/db/query/record_id_range.h"

#include <utility>
#include <vector>

#include <boost/none.hpp>
#include <boost/optional/optional.hpp>

namespace mongo::sbe {

class ClusteredScanStageTestFixture : public PlanStageTestFixture {
public:
    ClusteredScanStageTestFixture(NamespaceString nss) : _nss(std::move(nss)) {}

    MultipleCollectionAccessor createClusteredCollection(const std::vector<BSONObj>& docs);

    // Build a RecordIdRange from optional integer _id bounds.
    static RecordIdRange makeIntRange(boost::optional<int> minId,
                                      bool minInclusive,
                                      boost::optional<int> maxId,
                                      bool maxInclusive);

protected:
    const NamespaceString _nss;
};

}  // namespace mongo::sbe
