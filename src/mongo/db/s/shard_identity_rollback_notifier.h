/*
*    Copyright (C) 2016 MongoDB Inc.
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

#include "mongo/base/disallow_copying.h"

namespace mongo {

class OperationContext;
class ServiceContext;

/**
 * Used to detect when a shard's shardIdentity document has been rolled back.  Since rolling back
 * the shardIdentity document is illegal (as we can't clear the in-memory state associated with it),
 * we force the mongod to shut down if it happens.  We detect shardIdentity rollback by checking in
 * the OpObserver for that document to be deleted.  We can't shut down right at that moment,
 * however, as doing so would interrupt the rollback process and leave the document in place for
 * when the server is restarted.  Instead, when we detect the shardIdentity document being deleted,
 * we call recordThatRollbackHappened() on this class, which records the fact that the shardIdentity
 * document was deleted as part of the rollback, and then when we exit rollback we check
 * didRollbackHappen() and shut down if so.
 *
 * No concurrency control is needed in this class as recordThatRollbackHappened can only happen in
 * an OpObserver detecting that the document was deleted, and didRollbackHappen is only called on
 * exiting rollback, and there can only be one of those things happening at any given time.
 */
class ShardIdentityRollbackNotifier {
    MONGO_DISALLOW_COPYING(ShardIdentityRollbackNotifier);

public:
    ShardIdentityRollbackNotifier();

    /**
     * Retrieves the ShardIdentityRollbackNotifier associated with the specified service context.
     */
    static ShardIdentityRollbackNotifier* get(OperationContext* txn);
    static ShardIdentityRollbackNotifier* get(ServiceContext* txn);

    /**
     * Records the fact that the shardIdentity document was rolled back.
     */
    void recordThatRollbackHappened() {
        _rollbackHappened = true;
    }

    /**
     * Checks whether the shardIdentity document was rolled back.
     */
    bool didRollbackHappen() {
        return _rollbackHappened;
    }

private:
    // Whether or not a rollback of the shardIdentity document has been detected.
    // No concurrency control necessary since this is only consulted during
    bool _rollbackHappened = false;
};

}  // namespace mongo
