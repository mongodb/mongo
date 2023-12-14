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

#include "mongo/db/query/query_stats/aggregated_metric.h"

#include "mongo/base/string_data.h"
#include "mongo/bson/bsonobjbuilder.h"

namespace mongo::query_stats {

template <typename T>
requires std::is_arithmetic_v<T>
void AggregatedMetric<T>::appendTo(BSONObjBuilder& builder, StringData fieldName) const {
    BSONObjBuilder metricsBuilder = builder.subobjStart(fieldName);
    metricsBuilder.append("sum", static_cast<long long>(sum));
    metricsBuilder.append("max", static_cast<long long>(max));
    metricsBuilder.append("min", static_cast<long long>(min));
    metricsBuilder.append("sumOfSquares", static_cast<long long>(sumOfSquares));
    metricsBuilder.done();
}

template void AggregatedMetric<uint64_t>::appendTo(BSONObjBuilder& builder,
                                                   StringData fieldName) const;

void AggregatedBool::appendTo(BSONObjBuilder& builder, StringData fieldName) const {
    BSONObjBuilder metricsBuilder = builder.subobjStart(fieldName);
    metricsBuilder.append("true"_sd, (long long)trueCount);
    metricsBuilder.append("false"_sd, (long long)falseCount);
    metricsBuilder.done();
}

}  // namespace mongo::query_stats
