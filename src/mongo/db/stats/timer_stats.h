// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/bson/bsonobj.h"
#include "mongo/platform/atomic.h"
#include "mongo/util/modules.h"
#include "mongo/util/timer.h"

namespace [[MONGO_MOD_PUBLIC]] mongo {

/**
 * Holds timing information in milliseconds. Keeps track of number of times and total milliseconds
 * so that a diff can be computed.
 */
class TimerStats {
public:
    void recordMillis(int millis);

    /**
     * @return number of millis
     */
    int record(const Timer& timer);

    BSONObj getReport() const;
    operator BSONObj() const {
        return getReport();
    }

private:
    Atomic<long long> _num;
    Atomic<long long> _totalMillis;
};

/**
 * Holds an instance of a Timer such that we the time is recorded
 * when the TimerHolder goes out of scope
 */
class TimerHolder {
public:
    /** Destructor will record to TimerStats */
    TimerHolder(TimerStats* stats);
    /** Will record stats if recordMillis hasn't (based on _recorded)  */
    ~TimerHolder();

    /**
     * returns elapsed millis from internal timer
     */
    int millis() const {
        return _t.millis();
    }

    /**
     * records the time in the TimerStats and marks that we've
     * already recorded so the destructor doesn't
     */
    int recordMillis();

private:
    TimerStats* _stats;
    bool _recorded;
    Timer _t;
};
}  // namespace mongo
