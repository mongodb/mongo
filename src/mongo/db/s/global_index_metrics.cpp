/**
 *    Copyright (C) 2022-present MongoDB, Inc.
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
#include "mongo/db/s/global_index_metrics.h"
#include "mongo/db/exec/document_value/document.h"


namespace mongo {
namespace {
// Returns the originalCommand with the createIndexes, key and unique fields added.
BSONObj createOriginalCommand(const NamespaceString& nss, BSONObj keyPattern, bool unique) {

    using Doc = Document;
    using Arr = std::vector<Value>;
    using V = Value;

    return Doc{{"originatingCommand",
                V{Doc{{"createIndexes", V{StringData{nss.toString()}}},
                      {"key", std::move(keyPattern)},
                      {"unique", V{unique}}}}}}
        .toBson();
}
}  // namespace

GlobalIndexMetrics::GlobalIndexMetrics(UUID instanceId,
                                       BSONObj originatingCommand,
                                       NamespaceString nss,
                                       Role role,
                                       Date_t startTime,
                                       ClockSource* clockSource,
                                       ShardingDataTransformCumulativeMetrics* cumulativeMetrics)
    : ShardingDataTransformInstanceMetrics{std::move(instanceId),
                                           std::move(originatingCommand),
                                           std::move(nss),
                                           role,
                                           startTime,
                                           clockSource,
                                           cumulativeMetrics} {}

std::string GlobalIndexMetrics::createOperationDescription() const noexcept {
    return fmt::format("GlobalIndexMetrics{}Service {}",
                       ShardingDataTransformMetrics::getRoleName(_role),
                       _instanceId.toString());
}

Milliseconds GlobalIndexMetrics::getRecipientHighEstimateRemainingTimeMillis() const {
    return Milliseconds{0};
}

}  // namespace mongo
