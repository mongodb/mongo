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

namespace mongo {

template <typename Base>
class WithOplogApplicationCountMetrics : public Base {
public:
    template <typename... Args>
    WithOplogApplicationCountMetrics(Args&&... args) : Base{std::forward<Args>(args)...} {}

    virtual void onInsertApplied() {
        _insertsApplied.fetchAndAdd(1);
    }

    virtual void onUpdateApplied() {
        _updatesApplied.fetchAndAdd(1);
    }

    virtual void onDeleteApplied() {
        _deletesApplied.fetchAndAdd(1);
    }

    virtual void onOplogEntriesFetched(int64_t numEntries) {
        _oplogEntriesFetched.fetchAndAdd(numEntries);
    }

    virtual void onOplogEntriesApplied(int64_t numEntries) {
        _oplogEntriesApplied.fetchAndAdd(numEntries);
    }

protected:
    int64_t getInsertsApplied() const {
        return _insertsApplied.load();
    }

    int64_t getUpdatesApplied() const {
        return _updatesApplied.load();
    }

    int64_t getDeletesApplied() const {
        return _deletesApplied.load();
    }

    int64_t getOplogEntriesFetched() const {
        return _oplogEntriesFetched.load();
    }

    int64_t getOplogEntriesApplied() const {
        return _oplogEntriesApplied.load();
    }

    void restoreInsertsApplied(int64_t count) {
        _insertsApplied.store(count);
    }

    void restoreUpdatesApplied(int64_t count) {
        _updatesApplied.store(count);
    }

    void restoreDeletesApplied(int64_t count) {
        _deletesApplied.store(count);
    }

    void restoreOplogEntriesFetched(int64_t count) {
        _oplogEntriesFetched.store(count);
    }

    void restoreOplogEntriesApplied(int64_t count) {
        _oplogEntriesApplied.store(count);
    }

    template <typename FieldNameProvider>
    void reportOplogApplicationCountMetrics(const FieldNameProvider* names,
                                            BSONObjBuilder* bob) const {
        bob->append(names->getForOplogEntriesFetched(), getOplogEntriesFetched());
        bob->append(names->getForOplogEntriesApplied(), getOplogEntriesApplied());
        bob->append(names->getForInsertsApplied(), getInsertsApplied());
        bob->append(names->getForUpdatesApplied(), getUpdatesApplied());
        bob->append(names->getForDeletesApplied(), getDeletesApplied());
    }

private:
    AtomicWord<int64_t> _insertsApplied{0};
    AtomicWord<int64_t> _updatesApplied{0};
    AtomicWord<int64_t> _deletesApplied{0};
    AtomicWord<int64_t> _oplogEntriesApplied{0};
    AtomicWord<int64_t> _oplogEntriesFetched{0};
};

}  // namespace mongo
