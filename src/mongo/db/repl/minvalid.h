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

namespace mongo {
    class BSONObj;
    class OperationContext;
    class Timestamp;

namespace repl {

    /**
     * Helper functions for maintaining local.replset.minvalid collection contents.
     *
     * When a member reaches its minValid optime it is in a consistent state.  Thus, minValid is
     * set as the last step in initial sync.  At the beginning of initial sync, _initialSyncFlag
     * is appended onto minValid to indicate that initial sync was started but has not yet 
     * completed.
     * minValid is also used during "normal" sync: the last op in each batch is used to set 
     * minValid, to indicate that we are in a consistent state when the batch has been fully 
     * applied.
     */

    /**
     * The initial sync flag is used to durably record the state of an initial sync; its boolean
     * value is true when an initial sync is in progress and hasn't yet completed.  The flag
     * is stored as part of the local.replset.minvalid collection.
     */
    void clearInitialSyncFlag(OperationContext* txn);
    void setInitialSyncFlag(OperationContext* txn);
    bool getInitialSyncFlag();

    /**
     * The minValid value is the earliest (minimum) Timestamp that must be applied in order to
     * consider the dataset consistent.  Do not allow client reads if our last applied operation is
     * before the minValid time.
     */
    void setMinValid(OperationContext* ctx, Timestamp ts);
    Timestamp getMinValid(OperationContext* txn);
}
}
