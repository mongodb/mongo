/**
 *    Copyright (C) 2020-present MongoDB, Inc.
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

#pragma once

#include "mongo/db/operation_context.h"
#include "mongo/db/service_context.h"

namespace mongo {

/**
 * Container for server-wide hedging metrics.
 */
class HedgingMetrics {
    HedgingMetrics(const HedgingMetrics&) = delete;
    HedgingMetrics& operator=(const HedgingMetrics&) = delete;

public:
    HedgingMetrics() = default;

    static HedgingMetrics* get(ServiceContext* service);
    static HedgingMetrics* get(OperationContext* opCtx);

    long long getNumTotalOperations() const;
    void incrementNumTotalOperations();

    long long getNumTotalHedgedOperations() const;
    void incrementNumTotalHedgedOperations();

    long long getNumAdvantageouslyHedgedOperations() const;
    void incrementNumAdvantageouslyHedgedOperations();

    BSONObj toBSON() const;

private:
    // The number of all operations with readPreference options such that they could be hedged.
    AtomicWord<long long> _numTotalOperations{0};

    // The number of all operations that actually dispatched an additional rpc.
    AtomicWord<long long> _numTotalHedgedOperations{0};

    // The number of all operations where a rpc other than the first one fulfilled the client
    // request.
    AtomicWord<long long> _numAdvantageouslyHedgedOperations{0};
};

}  // namespace mongo
