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
bool getInitialSyncFlag(OperationContext* txn);
bool getInitialSyncFlag();

/**
 * The minValid value is the earliest (minimum) Timestamp that must be applied in order to
 * consider the dataset consistent.
 */
void setMinValid(OperationContext* txn, const OpTime& minValid);
OpTime getMinValid(OperationContext* txn);

/**
 * Sets minValid only if it is not already higher than endOpTime.
 * Warning, this compares the term and timestamp independently. Do not use if the current
 * minValid could be from the other fork of a rollback.
 */
void setMinValidToAtLeast(OperationContext* txn, const OpTime& endOpTime);

/**
 * On startup all oplog entries with a value >= the oplog delete from point should be deleted.
 * If null, no documents should be deleted.
 */
void setOplogDeleteFromPoint(OperationContext* txn, const Timestamp& timestamp);
Timestamp getOplogDeleteFromPoint(OperationContext* txn);

/**
 * The applied through point is a persistent record of where we've applied through. If null, the
 * applied through point is the top of the oplog.
 */
void setAppliedThrough(OperationContext* txn, const OpTime& optime);

/**
 * You should probably be calling ReplicationCoordinator::getLastAppliedOpTime() instead.
 *
 * This reads the value from storage which isn't always updated when the ReplicationCoordinator
 * is.
 */
OpTime getAppliedThrough(OperationContext* txn);
}
}
