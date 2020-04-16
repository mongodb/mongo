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

#include "mongo/db/vector_clock.h"

namespace mongo {

/**
 * A vector clock service that additionally permits being advanced authoritatively ("ticking").
 *
 * Only linked in contexts where ticking is allowed, ie. mongod, embedded, mongod-based unittests.
 */
class VectorClockMutable : public VectorClock {
public:
    // Decorate ServiceContext with VectorClockMutable*, that will resolve to the mutable vector
    // clock implementation.
    static VectorClockMutable* get(ServiceContext* service);
    static VectorClockMutable* get(OperationContext* ctx);

    static void registerVectorClockOnServiceContext(ServiceContext* service,
                                                    VectorClockMutable* vectorClockMutable);

    // Ticking
    virtual LogicalTime tick(Component component, uint64_t nTicks) = 0;
    virtual void tickTo(Component component, LogicalTime newTime) = 0;

protected:
    static bool _lessThanOrEqualToMaxPossibleTime(LogicalTime time, uint64_t nTicks);

    VectorClockMutable();
    virtual ~VectorClockMutable();

    // Internal Ticking API
    LogicalTime _advanceComponentTimeByTicks(Component component, uint64_t nTicks);
    void _advanceComponentTimeTo(Component component, LogicalTime&& newTime);
};

}  // namespace mongo
