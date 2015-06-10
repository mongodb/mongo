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

#include "mongo/client/syncclusterconnection.h"
#include "mongo/s/client/multi_host_query.h"

namespace mongo {

    /**
     * Query handler that plugs in to a SyncClusterConnection and allows query on fastest host
     * (if enabled).
     *
     * Glue code which shields the MultiHostQuery and server parameters from the separate client
     * module which knows about neither.
     *
     * There is a *single* SCCFastQueryHandler for every SCC.  Each SCCFastQueryHandler contains
     * its own thread pool (lazily initialized) so that at maximum there is a thread-per-SCC-host
     * and this thread may have an open connection to the host until it times out.
     * If using the "fastestConfigReads" options, clients must be ready for the additional thread
     * and connection load when configs are slow.
     */
    class SCCFastQueryHandler : public SyncClusterConnection::QueryHandler {
    public:

        SCCFastQueryHandler();

        virtual ~SCCFastQueryHandler() {
        }

        virtual bool canHandleQuery(const std::string& ns, Query query);

        virtual std::unique_ptr<DBClientCursor> handleQuery(const std::vector<std::string>& hostStrings,
                                                          const std::string &ns,
                                                          Query query,
                                                          int nToReturn,
                                                          int nToSkip,
                                                          const BSONObj *fieldsToReturn,
                                                          int queryOptions,
                                                          int batchSize);

    private:

        // The thread pool itself is scoped to the handler and SCC, and lazily creates threads
        // per-host as needed.  This ensures query starvation cannot occur due to other active
        // client threads - though a thread must be created for every client.
        HostThreadPools _queryThreads;
    };

}
