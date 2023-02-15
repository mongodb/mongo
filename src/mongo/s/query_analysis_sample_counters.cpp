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

#include "mongo/s/query_analysis_sample_counters.h"

#include "mongo/bson/bsonobj.h"
#include "mongo/s/is_mongos.h"

namespace mongo {
namespace analyze_shard_key {

const std::string SampleCounters::kDescriptionFieldName("desc");
const std::string SampleCounters::kDescriptionFieldValue("query analyzer");
const std::string SampleCounters::kNamespaceStringFieldName("ns");
const std::string SampleCounters::kCollUuidFieldName("collUuid");
const std::string SampleCounters::kSampledReadsCountFieldName("sampledReadsCount");
const std::string SampleCounters::kSampledWritesCountFieldName("sampledWritesCount");
const std::string SampleCounters::kSampledReadsBytesFieldName("sampledReadsBytes");
const std::string SampleCounters::kSampledWritesBytesFieldName("sampledWritesBytes");
const std::string SampleCounters::kSampleRateFieldName("sampleRate");

BSONObj SampleCounters::reportCurrentOp() const {
    BSONObjBuilder bob;
    bob.append(kDescriptionFieldName, kDescriptionFieldValue);
    bob.append(kNamespaceStringFieldName, _nss.toString());
    _collUuid.appendToBuilder(&bob, kCollUuidFieldName);
    bob.append(kSampledReadsCountFieldName, _sampledReadsCount);
    bob.append(kSampledWritesCountFieldName, _sampledWritesCount);
    if (!isMongos()) {
        bob.append(kSampledReadsBytesFieldName, _sampledReadsBytes);
        bob.append(kSampledWritesBytesFieldName, _sampledWritesBytes);
    }
    return bob.obj();
}

}  // namespace analyze_shard_key
}  // namespace mongo
