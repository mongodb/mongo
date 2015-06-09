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

#include <string>
#include <vector>

#include "mongo/base/disallow_copying.h"
#include "mongo/base/status.h"
#include "mongo/base/status_with.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/db/clientcursor.h"
#include "mongo/db/namespace_string.h"
#include "mongo/executor/task_executor.h"
#include "mongo/stdx/functional.h"
#include "mongo/stdx/condition_variable.h"
#include "mongo/stdx/mutex.h"
#include "mongo/util/net/hostandport.h"

namespace mongo {
    class Fetcher {
        MONGO_DISALLOW_COPYING(Fetcher);
    public:

        /**
         * Container for BSON documents extracted from cursor results.
         */
        typedef std::vector<BSONObj> Documents;

        /**
         * Documents in current batch with cursor ID and associated namespace name.
         * If cursor ID is zero, there are no additional batches.
         */
        struct BatchData {
            BatchData() = default;
            BatchData(CursorId theCursorId, const NamespaceString& theNss, Documents theDocuments);
            CursorId cursorId = 0;
            NamespaceString nss;
            Documents documents;
        };

        /**
         * Represents next steps of fetcher.
         */
        enum class NextAction : int {
            kInvalid=0,
            kNoAction=1,
            kGetMore=2
        };

        /**
         * Type of a fetcher callback function.
         */
        typedef stdx::function<void (const StatusWith<BatchData>&,
                                     NextAction*,
                                     BSONObjBuilder*)> CallbackFn;

        /**
         * Creates Fetcher task but does not schedule it to be run by the executor.
         *
         * First remote command to be run by the executor will be 'cmdObj'. The results
         * of 'cmdObj' must contain a cursor response object.
         * See Commands::appendCursorResponseObject.
         *
         * Callback function 'work' will be called 1 or more times after a successful
         * schedule() call depending on the results of the remote command.
         *
         * Depending on the cursor ID in the initial cursor response object, the fetcher may run
         * subsequent getMore commands on the remote server in order to obtain a complete
         * set of results.
         *
         * Failed remote commands will also cause 'work' to be invoked with the
         * error details provided by the remote server. On failure, the fetcher will stop
         * sending getMore requests to the remote server.
         *
         * If the fetcher is canceled (either by calling cancel() or shutting down the executor),
         * 'work' will not be invoked.
         *
         * Fetcher uses the NextAction argument to inform client via callback if a getMore command
         * will be scheduled to be run by the executor to retrieve additional results.
         * Also, note that the NextAction is both an input and output argument to allow
         * the client to suggest a different action for the fetcher to take post-callback.
         *
         * The callback function 'work' is not allowed to call into the Fetcher instance. This
         * behavior is undefined and may result in a deadlock.
         */
        Fetcher(executor::TaskExecutor* executor,
                const HostAndPort& source,
                const std::string& dbname,
                const BSONObj& cmdObj,
                const CallbackFn& work);

        virtual ~Fetcher();

        /**
         * Returns diagnostic information.
         */
        std::string getDiagnosticString() const;

        /**
         * Returns true if a remote command has been scheduled (but not completed)
         * with the executor.
         */
        bool isActive() const;

        /**
         * Schedules 'cmdObj' to be run on the remote server.
         */
        Status schedule();

        /**
         * Cancels remote command request.
         * Returns immediately if fetcher is not active.
         */
        void cancel();

        /**
         * Waits for remote command requests to complete.
         * Returns immediately if fetcher is not active.
         */
        void wait();

    private:

        /**
         * Schedules remote command to be run by the executor
         */
        Status _schedule_inlock(const BSONObj& cmdObj, const char* batchFieldName);

        /**
         * Callback for remote command.
         */
        void _callback(const executor::TaskExecutor::RemoteCommandCallbackArgs& rcbd,
                       const char* batchFieldName);

        /**
         * Sets fetcher state to inactive and notifies waiters.
         */
        void _finishCallback();

        // Not owned by us.
        executor::TaskExecutor* _executor;

        HostAndPort _source;
        std::string _dbname;
        BSONObj _cmdObj;
        CallbackFn _work;

        // Protects member data of this Fetcher.
        mutable stdx::mutex _mutex;

        mutable stdx::condition_variable _condition;

        // _active is true when Fetcher is scheduled to be run by the executor.
        bool _active;
        // Callback handle to the scheduled remote command.
        executor::TaskExecutor::CallbackHandle _remoteCommandCallbackHandle;
    };

} // namespace mongo
