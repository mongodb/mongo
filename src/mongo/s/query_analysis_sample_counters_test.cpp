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

#include "mongo/db/namespace_string.h"
#include "mongo/logv2/log.h"
#include "mongo/s/query_analysis_sample_counters.h"
#include "mongo/unittest/bson_test_util.h"
#include "mongo/util/uuid.h"

#include <boost/none.hpp>
#include <cstddef>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kTest

namespace mongo {
namespace analyze_shard_key {
namespace {

const NamespaceString nss0 = NamespaceString::createNamespaceString_forTest("testDb", "testColl0");

TEST(QueryAnalysisSampleCounters, CountAndReportCurrentOp) {
    auto collUuid = UUID::gen();
    auto counters = SampleCounters(nss0, collUuid);
    ASSERT_EQ(nss0, counters.getNss());
    ASSERT_EQ(collUuid, counters.getCollUUID());
    ASSERT_EQ(0, counters.getSampledReadsCount());
    ASSERT_EQ(0, counters.getSampledReadsBytes());
    ASSERT_EQ(0, counters.getSampledWritesCount());
    ASSERT_EQ(0, counters.getSampledWritesBytes());

    long long size = 100LL;

    counters.incrementReads(size);
    counters.incrementReads(boost::none);
    ASSERT_EQ(2, counters.getSampledReadsCount());
    ASSERT_EQ(size, counters.getSampledReadsBytes());

    counters.incrementWrites(size);
    counters.incrementWrites(boost::none);
    ASSERT_EQ(2, counters.getSampledWritesCount());
    ASSERT_EQ(size, counters.getSampledWritesBytes());

    BSONObj report = counters.reportCurrentOp();

    ASSERT_EQ(report.getField(SampleCounters::kDescriptionFieldName).String(),
              SampleCounters::kDescriptionFieldValue);
    ASSERT_EQ(report.getField(SampleCounters::kNamespaceStringFieldName).String(), nss0.toString());
    ASSERT_EQ(UUID::parse(report.getField(SampleCounters::kCollUuidFieldName)), collUuid);
    ASSERT_EQ(report.getField(SampleCounters::kSampledReadsCountFieldName).Long(), 2);
    ASSERT_EQ(report.getField(SampleCounters::kSampledWritesCountFieldName).Long(), 2);
    ASSERT_EQ(report.getField(SampleCounters::kSampledReadsBytesFieldName).Long(), size);
    ASSERT_EQ(report.getField(SampleCounters::kSampledWritesBytesFieldName).Long(), size);
}

}  // namespace
}  // namespace analyze_shard_key
}  // namespace mongo
