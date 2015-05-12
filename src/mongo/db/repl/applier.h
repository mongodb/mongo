/**
 *    Copyright (C) 2015 MongoDB Inc.
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

#include <boost/thread/condition.hpp>
#include <boost/thread/mutex.hpp>
#include <string>
#include <vector>

#include "mongo/base/disallow_copying.h"
#include "mongo/base/status.h"
#include "mongo/base/status_with.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/db/repl/replication_executor.h"
#include "mongo/stdx/functional.h"

namespace mongo {
namespace repl {

    class Applier {
        MONGO_DISALLOW_COPYING(Applier);
    public:

        using Operations = std::vector<BSONObj>;

        /**
         * Callback function to report final status of applying operations along with
         * list of operations (if applicable) that were not successfully applied.
         * On success, returns the timestamp of the last operation applied together with an empty
         * list of operations.
         */
        using CallbackFn = stdx::function<void (const StatusWith<Timestamp>&, const Operations&)>;

        /**
         * Type of function to to apply a single operation. In production, this function
         * would have the same outcome as calling SyncTail::syncApply() ('convertUpdatesToUpserts'
         * value will be embedded in the function implementation).
         */
        using ApplyOperationFn = stdx::function<Status (OperationContext*, const BSONObj&)>;

        /**
         * Creates Applier in inactive state.
         *
         * Accepts list of oplog entries to apply in 'operations'.
         *
         * The callback function will be invoked (after schedule()) when the applied has
         * successfully applied all the operations or encountered a failure. Failures may occur if
         * we failed to apply an operation; or if the underlying scheduled work item
         * on the replication executor was canceled.
         *
         * It is an error for 'operations' to be empty but individual oplog entries
         * contained in 'operations' are not validated.
         */
        Applier(ReplicationExecutor* executor,
                const Operations& operations,
                const ApplyOperationFn& applyOperation,
                const CallbackFn& onCompletion);

        /**
         * Blocks while applier is active.
         */
        virtual ~Applier();

        /**
         * Returns diagnostic information.
         */
        std::string getDiagnosticString() const;

        /**
         * Returns true if the applier has been started (but has not completed).
         */
        bool isActive() const;

        /**
         * Starts applier by scheduling initial db work to be run by the executor.
         */
        Status start();

        /**
         * Cancels current db work request.
         * Returns immediately if applier is not active.
         *
         * Callback function may be invoked with an ErrorCodes::CallbackCanceled status.
         */
        void cancel();

        /**
         * Waits for active database worker to complete.
         * Returns immediately if applier is not active.
         */
        void wait();

    private:

        /**
         * DB worker callback function - applies all operations.
         */
        void _callback(const ReplicationExecutor::CallbackData& cbd);

        // Not owned by us.
        ReplicationExecutor* _executor;

        Operations _operations;
        ApplyOperationFn _applyOperation;
        CallbackFn _onCompletion;

        // Protects member data of this Applier.
        mutable boost::mutex _mutex;

        boost::condition _condition;

        // _active is true when Applier is scheduled to be run by the executor.
        bool _active;

        ReplicationExecutor::CallbackHandle _dbWorkCallbackHandle;
};

} // namespace repl
} // namespace mongo
