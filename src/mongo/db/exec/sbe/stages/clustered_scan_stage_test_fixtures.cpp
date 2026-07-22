/**
 *    Copyright (C) 2026-present MongoDB, Inc.
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
#include "mongo/db/exec/sbe/stages/clustered_scan_stage_test_fixtures.h"

#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/dbhelpers.h"
#include "mongo/db/exec/sbe/expressions/compile_ctx.h"
#include "mongo/db/exec/sbe/expressions/runtime_environment.h"
#include "mongo/db/exec/sbe/sbe_plan_stage_test.h"
#include "mongo/db/exec/sbe/stages/stages.h"
#include "mongo/db/exec/sbe/values/slot.h"
#include "mongo/db/exec/sbe/values/value.h"
#include "mongo/db/query/multiple_collection_accessor.h"
#include "mongo/db/query/record_id_bound.h"
#include "mongo/db/query/record_id_range.h"
#include "mongo/db/query/record_id_range_list.h"
#include "mongo/db/record_id_helpers.h"
#include "mongo/db/shard_role/shard_catalog/clustered_collection_util.h"
#include "mongo/db/shard_role/shard_catalog/collection.h"
#include "mongo/unittest/unittest.h"

#include <memory>
#include <utility>
#include <vector>

#include <boost/none.hpp>
#include <boost/optional/optional.hpp>

namespace mongo::sbe {

MultipleCollectionAccessor ClusteredScanStageTestFixture::createClusteredCollection(
    const std::vector<BSONObj>& docs) {
    CollectionOptions options;
    options.clusteredIndex = clustered_util::makeDefaultClusteredIdIndex();
    ASSERT_OK(storageInterface()->createCollection(
        operationContext(), _nss, options, false /* createIdIndex */));

    {
        AutoGetCollection agc(operationContext(), _nss, LockMode::MODE_IX);
        WriteUnitOfWork wuow{operationContext()};
        ASSERT_OK(Helpers::insert(operationContext(), *agc, docs));
        wuow.commit();
    }

    auto localColl =
        acquireCollection(operationContext(),
                          CollectionAcquisitionRequest::fromOpCtx(
                              operationContext(), _nss, AcquisitionPrerequisites::kRead),
                          MODE_IS);
    return MultipleCollectionAccessor(localColl);
}

RecordIdRange ClusteredScanStageTestFixture::makeIntRange(boost::optional<int> minId,
                                                          bool minInclusive,
                                                          boost::optional<int> maxId,
                                                          bool maxInclusive) {
    boost::optional<RecordIdBound> minBound, maxBound;
    if (minId)
        minBound =
            RecordIdBound(record_id_helpers::keyForElem(BSON("_id" << *minId).firstElement()));
    if (maxId)
        maxBound =
            RecordIdBound(record_id_helpers::keyForElem(BSON("_id" << *maxId).firstElement()));
    RecordIdRange range;
    range.intersectRange(minBound, maxBound, minInclusive, maxInclusive);
    return range;
}

}  // namespace mongo::sbe
