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

#pragma once

#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/util/modules.h"

namespace mongo {
/**
 * Tracks statistics for the number of changes done to the authoritative database versioning in the
 * shard.
 *
 * The metrics reported are as follows:
 * - durableChanges: The number of persisted durable changes done to the databases owned by this
 *   shard.
 * - inMemoryChanges: The number of in-memory changes done to the databases owned by this shard.
 *   This can be higher than the number of persisted changes since clearing the database is an
 *   in-memory operation that will trigger repopulating it from the persisted state.
 */
class MONGO_MOD_PARENT_PRIVATE AuthoritativeDatabaseVersionUpdates {
public:
    void report(BSONObjBuilder& builder) const {
        builder.append("durableChanges", _numberOfDurableUpdatesDone.loadRelaxed());
        builder.append("inMemoryClears", _numberOfInMemoryClears.loadRelaxed());
        builder.append("inMemorySets", _numberOfInMemorySets.loadRelaxed());
        builder.append("inMemoryAccessTypeChanges",
                       _numberOfInMemoryAccessTypeChange.loadRelaxed());
        builder.append("movePrimariesInProgress", _numberOfInMemoryMovePrimaryActive.loadRelaxed());
    }

    void registerDurableUpdate() {
        _numberOfDurableUpdatesDone.fetchAndAddRelaxed(1);
    }

    void registerInMemorySet() {
        _numberOfInMemorySets.fetchAndAddRelaxed(1);
    }

    void registerInMemoryClear() {
        _numberOfInMemoryClears.fetchAndAddRelaxed(1);
    }

    void registerInMemoryMovePrimaryState(bool inProgress) {
        if (inProgress) {
            _numberOfInMemoryMovePrimaryActive.fetchAndAddRelaxed(1);
        } else {
            _numberOfInMemoryMovePrimaryActive.fetchAndAddRelaxed(-1);
        }
    }

    void registerInMemoryAccessTypeChange() {
        _numberOfInMemoryAccessTypeChange.fetchAndAddRelaxed(1);
    }

private:
    AtomicWord<int64_t> _numberOfDurableUpdatesDone{0};
    AtomicWord<int64_t> _numberOfInMemoryClears{0};
    AtomicWord<int64_t> _numberOfInMemorySets{0};
    AtomicWord<int64_t> _numberOfInMemoryMovePrimaryActive{0};
    AtomicWord<int64_t> _numberOfInMemoryAccessTypeChange{0};
};
}  // namespace mongo
