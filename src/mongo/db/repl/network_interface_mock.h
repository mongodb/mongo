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

#include <boost/thread/mutex.hpp>
#include <boost/thread/condition_variable.hpp>
#include <map>

#include "mongo/db/repl/replication_executor.h"

namespace mongo {
namespace repl {

    class NetworkInterfaceMock : public ReplicationExecutor::NetworkInterface {
    public:
        NetworkInterfaceMock() : _simulatedNetworkLatencyMillis(0) {}
        virtual ~NetworkInterfaceMock() {}
        virtual Date_t now();
        virtual StatusWith<BSONObj> runCommand(
                const ReplicationExecutor::RemoteCommandRequest& request);
        virtual void runCallbackWithGlobalExclusiveLock(
                const stdx::function<void ()>& callback);

        /**
         * Adds a response (StatusWith<BSONObj>) for this mock to return for a given request.
         * For each request, the mock will return the corresponding response for all future calls.
         *
         * If "isBlocked" is set to true, the network will block in runCommand for the given
         * request until unblockResponse is called with "request" as an argument.
         */
        bool addResponse(const ReplicationExecutor::RemoteCommandRequest& request,
                         const StatusWith<BSONObj>& response,
                         bool isBlocked = false);

        /**
         * Unblocks response to "request" that was blocked when it was added.
         */
        void unblockResponse(const ReplicationExecutor::RemoteCommandRequest& request);

        /**
         * Unblocks all responses that were blocked when added.
         */
        void unblockAll();

        /**
         * Network latency added for each remote command, defaults to 0.
         */
        void simulatedNetworkLatency(int millis);

    private:
        struct ResponseInfo {
            ResponseInfo(const StatusWith<BSONObj>& r, bool block);

            StatusWith<BSONObj> response;
            bool isBlocked;
        };
        typedef std::map<ReplicationExecutor::RemoteCommandRequest,
                         ResponseInfo > RequestResponseMap;
        boost::mutex _mutex;
        boost::condition_variable _someResponseUnblocked;
        RequestResponseMap _responses;
        int _simulatedNetworkLatencyMillis;
    };

}  // namespace repl
}  // namespace mongo
