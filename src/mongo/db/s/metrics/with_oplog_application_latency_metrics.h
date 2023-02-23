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

#pragma once

#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/platform/atomic_word.h"
#include "mongo/util/duration.h"

namespace mongo {

template <typename Base>
class WithOplogApplicationLatencyMetrics : public Base {
public:
    template <typename... Args>
    WithOplogApplicationLatencyMetrics(Args&&... args) : Base{std::forward<Args>(args)...} {}

    void onBatchRetrievedDuringOplogFetching(Milliseconds elapsed) {
        _oplogFetchingTotalRemoteBatchesRetrieved.fetchAndAdd(1);
        _oplogFetchingTotalRemoteBatchesRetrievalTimeMillis.fetchAndAdd(
            durationCount<Milliseconds>(elapsed));
    }

    void onLocalInsertDuringOplogFetching(const Milliseconds& elapsedTime) {
        _oplogFetchingTotalLocalInserts.fetchAndAdd(1);
        _oplogFetchingTotalLocalInsertTimeMillis.fetchAndAdd(
            durationCount<Milliseconds>(elapsedTime));
    }

    void onBatchRetrievedDuringOplogApplying(const Milliseconds& elapsedTime) {
        _oplogApplyingTotalBatchesRetrieved.fetchAndAdd(1);
        _oplogApplyingTotalBatchesRetrievalTimeMillis.fetchAndAdd(
            durationCount<Milliseconds>(elapsedTime));
    }

    void onOplogLocalBatchApplied(Milliseconds elapsed) {
        _oplogBatchApplied.fetchAndAdd(1);
        _oplogBatchAppliedMillis.fetchAndAdd(durationCount<Milliseconds>(elapsed));
    }

protected:
    int64_t getOplogFetchingTotalRemoteBatchesRetrieved() const {
        return _oplogFetchingTotalRemoteBatchesRetrieved.load();
    }

    int64_t getOplogFetchingTotalRemoteBatchesRetrievalTimeMillis() const {
        return _oplogFetchingTotalRemoteBatchesRetrievalTimeMillis.load();
    }

    int64_t getOplogFetchingTotalLocalInserts() const {
        return _oplogFetchingTotalLocalInserts.load();
    }

    int64_t getOplogFetchingTotalLocalInsertTimeMillis() const {
        return _oplogFetchingTotalLocalInsertTimeMillis.load();
    }

    int64_t getOplogApplyingTotalBatchesRetrieved() const {
        return _oplogApplyingTotalBatchesRetrieved.load();
    }

    int64_t getOplogApplyingTotalBatchesRetrievalTimeMillis() const {
        return _oplogApplyingTotalBatchesRetrievalTimeMillis.load();
    }

    int64_t getOplogBatchApplied() const {
        return _oplogBatchApplied.load();
    }

    int64_t getOplogBatchAppliedMillis() const {
        return _oplogBatchAppliedMillis.load();
    }

    template <typename FieldNameProvider>
    void reportOplogApplicationLatencyMetrics(const FieldNameProvider* names,
                                              BSONObjBuilder* bob) const {
        bob->append(names->getForOplogFetchingTotalRemoteBatchRetrievalTimeMillis(),
                    getOplogFetchingTotalRemoteBatchesRetrievalTimeMillis());
        bob->append(names->getForOplogFetchingTotalRemoteBatchesRetrieved(),
                    getOplogFetchingTotalRemoteBatchesRetrieved());
        bob->append(names->getForOplogFetchingTotalLocalInsertTimeMillis(),
                    getOplogFetchingTotalLocalInsertTimeMillis());
        bob->append(names->getForOplogFetchingTotalLocalInserts(),
                    getOplogFetchingTotalLocalInserts());
        bob->append(names->getForOplogApplyingTotalLocalBatchRetrievalTimeMillis(),
                    getOplogApplyingTotalBatchesRetrievalTimeMillis());
        bob->append(names->getForOplogApplyingTotalLocalBatchesRetrieved(),
                    getOplogApplyingTotalBatchesRetrieved());
        bob->append(names->getForOplogApplyingTotalLocalBatchApplyTimeMillis(),
                    getOplogBatchAppliedMillis());
        bob->append(names->getForOplogApplyingTotalLocalBatchesApplied(), getOplogBatchApplied());
    }

private:
    AtomicWord<int64_t> _oplogFetchingTotalRemoteBatchesRetrieved{0};
    AtomicWord<int64_t> _oplogFetchingTotalRemoteBatchesRetrievalTimeMillis{0};
    AtomicWord<int64_t> _oplogFetchingTotalLocalInserts{0};
    AtomicWord<int64_t> _oplogFetchingTotalLocalInsertTimeMillis{0};
    AtomicWord<int64_t> _oplogApplyingTotalBatchesRetrieved{0};
    AtomicWord<int64_t> _oplogApplyingTotalBatchesRetrievalTimeMillis{0};
    AtomicWord<int64_t> _oplogBatchApplied{0};
    AtomicWord<int64_t> _oplogBatchAppliedMillis{0};
};

}  // namespace mongo
