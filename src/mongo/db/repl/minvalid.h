/**
 *    Copyright (C) 2014 MongoDB Inc.
 *
 *    This program is free software: you can redistribute it and/or  modify
 *    it under the terms of the GNU Affero General Public License, version 3,
 *    as published by the Free Software Foundation.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    GNU Affero General Public License for more details.
 *
 *    You should have received a copy of the GNU Affero General Public License
 *    along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the GNU Affero General Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */

#pragma once

#include "mongo/db/repl/optime.h"

namespace mongo {
class BSONObj;
class OperationContext;

namespace repl {

struct BatchBoundaries {
    BatchBoundaries(const OpTime s, const OpTime e) : start(s), end(e) {}
    const OpTime start;
    const OpTime end;
};

enum class DurableRequirement {
    None,    // Does not require any durability of the write.
    Strong,  // Requires journal or checkpoint write.
};

/**
 * Helper functions for maintaining a single document in the local.replset.minvalid collection.
 *
 * When a member reaches its minValid optime it is in a consistent state.  Thus, minValid is
 * set as the last step in initial sync.  At the beginning of initial sync, doingInitialSync
 * is appended onto minValid to indicate that initial sync was started but has not yet
 * completed.
 *
 * The document is also updated during "normal" sync. The optime of the last op in each batch is
 * used to set minValid, along with a "begin" field to demark the start and the fact that a batch
 * is active. When the batch is done the "begin" field is removed to indicate that we are in a
 * consistent state when the batch has been fully applied.
 *
 * Example of all fields:
 * { _id:...,
 *      doingInitialSync: true // initial sync is active
 *      ts:..., t:...   // end-OpTime
 *      begin: {ts:..., t:...} // a batch is currently being applied, and not consistent
 * }
 */

/**
 * The initial sync flag is used to durably record that initial sync has not completed.
 *
 * These operations wait for durable writes (which will block on journaling/checkpointing).
 */
void clearInitialSyncFlag(OperationContext* txn);
void setInitialSyncFlag(OperationContext* txn);

/**
 * Returns true if the initial sync flag is set (and presumed active).
 */
bool getInitialSyncFlag();


/**
 * Returns the bounds of the current apply batch, if active. If start is null/missing, and
 * end is equal to the last oplog entry then we are in a consistent state and ready for reads.
 */
BatchBoundaries getMinValid(OperationContext* txn);

/**
 * The minValid value is the earliest (minimum) Timestamp that must be applied in order to
 * consider the dataset consistent.
 *
 * This is called when a batch finishes.
 *
 * Wait for durable writes (which will block on journaling/checkpointing) when specified.
 *
 */
void setMinValid(OperationContext* ctx, const OpTime& endOpTime, const DurableRequirement durReq);

/**
 * The bounds indicate an apply is active and we are not in a consistent state to allow reads
 * or transition from a non-visible state to primary/secondary.
 */
void setMinValid(OperationContext* ctx, const BatchBoundaries& boundaries);
}
}
