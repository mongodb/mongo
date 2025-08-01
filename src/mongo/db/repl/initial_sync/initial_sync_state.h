/**
 *    Copyright (C) 2018-present MongoDB, Inc.
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


#include "mongo/bson/timestamp.h"
#include "mongo/db/repl/initial_sync/all_database_cloner.h"
#include "mongo/util/modules.h"

namespace mongo {
namespace repl {

/**
 * Holder of state for initial sync (InitialSyncer).
 */
struct InitialSyncState {
    InitialSyncState(std::unique_ptr<AllDatabaseCloner> cloner)
        : allDatabaseCloner(std::move(cloner)) {};

    std::unique_ptr<AllDatabaseCloner>
        allDatabaseCloner;                 // Cloner for all databases included in initial sync.
    Future<void> allDatabaseClonerFuture;  // Future for holding result of AllDatabaseCloner
    Timestamp beginApplyingTimestamp;  // Timestamp from the latest entry in oplog when started. It
                                       // is also the timestamp after which we will start applying
                                       // operations during initial sync.
    Timestamp beginFetchingTimestamp;  // Timestamp from the earliest active transaction that had an
                                       // oplog entry.
    Timestamp stopTimestamp;  // Referred to as minvalid, or the place we can transition states.
    Timer timer;              // Timer for timing how long each initial sync attempt takes.
    size_t appliedOps = 0;
};

}  // namespace repl
}  // namespace mongo
