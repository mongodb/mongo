/**
 * Copyright (C) 2013 10gen Inc.
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Affero General Public License, version 3,
 * as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU Affero General Public License for more details.
 *
 * You should have received a copy of the GNU Affero General Public License
 * along with this program. If not, see <http://www.gnu.org/licenses/>.
 */

#pragma once

#include <boost/scoped_ptr.hpp>
#include <vector>

#include "mongo/bson/bsonobj.h"
#include "mongo/s/batched_error_detail.h"
#include "mongo/s/batched_command_request.h"
#include "mongo/s/ns_targeter.h"

namespace mongo {

    struct TargetedWrite;
    struct ChildWriteOp;

    enum WriteOpState {

        // Item is ready to be targeted
        WriteOpState_Ready,

        // Item is targeted and we're waiting for outstanding shard requests to populate
        // responses
        WriteOpState_Pending,

        // Op was successful, write completed
        WriteOpState_Completed,

        // Op failed with some error
        WriteOpState_Error,

        // Catch-all error state.
        WriteOpState_Unknown
    };

    /**
     * State of a single write item in-progress from a client request.
     *
     * The lifecyle of a write op:
     *
     *   0. Begins at _Ready,
     *
     *   1a. Targeted, and a ChildWriteOp created to track the state of each returned TargetedWrite.
     *      The state is changed to _Pending.
     *   1b. If the op cannot be targeted, the error is set directly (_Error), and the write op is
     *       completed.
     *
     *   2.  TargetedWrites finish successfully and unsuccessfully.
     *
     *   On the last error arriving...
     *
     *   3a. If the errors allow for retry, the WriteOp is reset to _Ready, previous ChildWriteOps
     *       are placed in the history, and goto 0.
     *   3b. If the errors don't allow for retry, they are combined into a single error and the
     *       state is changed to _Error.
     *   3c. If there are no errors, the state is changed to _Completed.
     *
     * WriteOps finish in a _Completed or _Error state.
     */
    class WriteOp {
    public:

        WriteOp( const BatchItemRef& itemRef ) :
            _itemRef( itemRef ), _state( WriteOpState_Ready ) {
        }

        ~WriteOp();

        /**
         * Returns the op's current state.
         */
        WriteOpState getWriteState() const;

        /**
         * Returns the op's error.
         *
         * Can only be used in state _Error
         */
        const BatchedErrorDetail& getOpError() const;

        /**
         * Creates TargetedWrite operations for every applicable shard, which contain the
         * information needed to send the child writes generated from this write item.
         *
         * The ShardTargeter determines the ShardEndpoints to send child writes to, but is not
         * modified by this operation.
         *
         * Returns !OK if the targeting process itself fails
         *             (no TargetedWrites will be added, state unchanged)
         */
        Status targetWrites( const NSTargeter& targeter,
                             std::vector<TargetedWrite*>* targetedWrites );

        /**
         * Marks the targeted write as finished for this write op.
         *
         * One of noteWriteComplete or noteWriteError should be called exactly once for every
         * TargetedWrite.
         */
        void noteWriteComplete( const TargetedWrite& targetedWrite );

        /**
         * Stores the error response of a TargetedWrite for later use, marks the write as finished.
         *
         * As above, one of noteWriteComplete or noteWriteError should be called exactly once for
         * every TargetedWrite.
         */
        void noteWriteError( const TargetedWrite& targetedWrite, const BatchedErrorDetail& error );

        /**
         * Sets the error for this write op directly, and forces the state to _Error.
         *
         * Should only be used when in state _Ready.
         */
        void setOpError( const BatchedErrorDetail& error );

    private:

        /**
         * Updates the op state after new information is received.
         */
        void updateOpState();

        // Owned elsewhere, reference to a batch with a write item
        const BatchItemRef _itemRef;

        // What stage of the operation we are at
        WriteOpState _state;

        // filled when state == _Pending
        std::vector<ChildWriteOp*> _childOps;

        // filled when state == _Error
        scoped_ptr<BatchedErrorDetail> _error;

        // Finished child operations, for debugging
        std::vector<ChildWriteOp*> _history;
    };

    /**
     * State of a write in-progress (to a single shard) which is one part of a larger write
     * operation.
     *
     * As above, the write op may finish in either a successful (_Completed) or unsuccessful
     * (_Error) state.
     */
    struct ChildWriteOp {

        ChildWriteOp( WriteOp* const parent ) :
            parentOp( parent ), state( WriteOpState_Ready ), pendingWrite( NULL ) {
        }

        const WriteOp* const parentOp;
        WriteOpState state;

        // non-zero when state == _Pending
        // Not owned here but tracked for reporting
        TargetedWrite* pendingWrite;

        // filled when state > _Pending
        scoped_ptr<ShardEndpoint> endpoint;

        // filled when state == _Error
        scoped_ptr<BatchedErrorDetail> error;
    };

    // First value is write item index in the batch, second value is child write op index
    typedef pair<int, int> WriteOpRef;

    /**
     * A write with A) a request targeted at a particular shard endpoint, and B) a response targeted
     * at a particular WriteOp.
     *
     * TargetedWrites are the link between the RPC layer and the in-progress write
     * operation.
     */
    struct TargetedWrite {

        TargetedWrite( const ShardEndpoint& endpoint, WriteOpRef writeOpRef ) :
            endpoint( endpoint ), writeOpRef( writeOpRef ) {
        }

        // Where to send the write
        ShardEndpoint endpoint;

        // Where to find the write item and put the response
        // TODO: Could be a more complex handle, shared between write state and networking code if
        // we need to be able to cancel ops.
        WriteOpRef writeOpRef;
    };

}
