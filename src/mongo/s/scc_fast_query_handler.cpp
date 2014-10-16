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

#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kSharding

#include "mongo/s/scc_fast_query_handler.h"

#include <vector>

#include "mongo/base/init.h"
#include "mongo/bson/util/builder.h"
#include "mongo/client/connpool.h"
#include "mongo/client/dbclientcursor.h"
#include "mongo/client/dbclientinterface.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/server_parameters.h"
#include "mongo/util/log.h"

namespace mongo {

    /**
     * This parameter turns on fastest config reads for auth data only - *.system.users collections
     * and the "usersInfo" command.  This should be enough to prevent a non-responsive config from
     * hanging other operations in a cluster.
     */
    MONGO_EXPORT_SERVER_PARAMETER(internalSCCAllowFastestAuthConfigReads, bool, false);

    /**
     * TESTING ONLY.
     *
     * This parameter turns on fastest config reads for *all* config.* collections except those
     * deemed extremely critical (config.version, config.locks).
     *
     * NOT FOR PRODUCTION USE.
     */
    MONGO_EXPORT_SERVER_PARAMETER(internalSCCAllowFastestMetadataConfigReads, bool, false);

    //
    // The shared environment for MultiHostQueries
    //

    namespace {

        class MultiQueryEnv : public MultiHostQueryOp::SystemEnv {
        public:

            virtual ~MultiQueryEnv() {
            }

            Date_t currentTimeMillis();

            StatusWith<DBClientCursor*> doBlockingQuery(const ConnectionString& host,
                                                        const QuerySpec& query);
        };

        StatusWith<DBClientCursor*> MultiQueryEnv::doBlockingQuery(const ConnectionString& host,
                                                                   const QuerySpec& query) {

            //
            // Note that this function may be active during shutdown.  This means that we must
            // handle connection pool shutdown exceptions (uasserts).  The results of this
            // operation must then be correctly discarded by the calling thread.
            //

            auto_ptr<DBClientCursor> cursor;

            try {

                ScopedDbConnection conn(host, 30.0 /* timeout secs */);

                cursor = conn->query(query.ns(),
                                     query.filter(),
                                     query.ntoreturn(),
                                     query.ntoskip(),
                                     query.fieldsPtr(),
                                     query.options(),
                                     0);

                if ( NULL == cursor.get()) {

                    // Confusingly, exceptions here are written to the log, not thrown

                    StringBuilder builder;
                    builder << "error querying server " << host.toString()
                            << ", could not create cursor for query";

                    warning() << builder.str() << endl;
                    return StatusWith<DBClientCursor*>(ErrorCodes::HostUnreachable, builder.str());
                }

                // Confusingly, this *detaches* the cursor from the connection it was established
                // on, further getMore calls will use a (potentially different) ScopedDbConnection.
                //
                // Calls done() too.
                cursor->attach(&conn);
            }
            catch (const DBException& ex) {
                return StatusWith<DBClientCursor*>(ex.toStatus());
            }

            return StatusWith<DBClientCursor*>(cursor.release());
        }

        Date_t MultiQueryEnv::currentTimeMillis() {
            return jsTime();
        }
    }

    // Shared networking environment which executes queries.
    // NOTE: This environment must stay in scope as long as per-host threads are executing queries -
    // i.e. for the lifetime of the server.
    // Once the thread pools are disposed and connections shut down, the per-host threads should
    // be self-contained and correctly shut down after discarding the results.
    static MultiQueryEnv* _multiQueryEnv;

    namespace {

        //
        // Create the shared multi-query environment at startup
        //

        MONGO_INITIALIZER(InitMultiQueryEnv)(InitializerContext* context) {
            // Leaked intentionally
            _multiQueryEnv = new MultiQueryEnv();
            return Status::OK();
        }
    }

    //
    // Per-SCC handling of queries
    //

    SCCFastQueryHandler::SCCFastQueryHandler() :
        _queryThreads(1, false) {
    }

    bool SCCFastQueryHandler::canHandleQuery(const string& ns, Query query) {

        if (!internalSCCAllowFastestAuthConfigReads &&
            !internalSCCAllowFastestMetadataConfigReads) {
            return false;
        }

        //
        // More operations can be added here
        //
        // NOTE: Not all operations actually pass through the SCC _queryOnActive path - notable
        // exceptions include anything related to direct query ops and direct operations for
        // connection maintenance.
        //

        NamespaceString nss(ns);
        if (nss.isCommand()) {
            BSONObj cmdObj = query.getFilter();
            string cmdName = cmdObj.firstElement().fieldName();
            if (cmdName == "usersInfo")
                return true;
        }
        else if (nss.coll() == "system.users") {
            return true;
        }

        //
        // Allow fastest config reads for all collections except for those involved in locks and
        // cluster versioning.
        //

        if (!internalSCCAllowFastestMetadataConfigReads)
            return false;

        if (nss.db() != "config")
            return false;

        if (nss.coll() != "version" && nss.coll() != "locks" && nss.coll() != "lockpings") {
            return true;
        }

        return false;
    }

    static vector<ConnectionString> getHosts(const vector<string> hostStrings) {

        vector<ConnectionString> hosts;
        for (vector<string>::const_iterator it = hostStrings.begin(); it != hostStrings.end();
            ++it) {

            string errMsg;
            ConnectionString host;
            hosts.push_back(ConnectionString::parse(*it, errMsg));
            invariant( hosts.back().type() != ConnectionString::INVALID );
        }

        return hosts;
    }

    auto_ptr<DBClientCursor> SCCFastQueryHandler::handleQuery(const vector<string>& hostStrings,
                                                              const string& ns,
                                                              Query query,
                                                              int nToReturn,
                                                              int nToSkip,
                                                              const BSONObj* fieldsToReturn,
                                                              int queryOptions,
                                                              int batchSize) {

        MultiHostQueryOp queryOp(_multiQueryEnv, &_queryThreads);

        QuerySpec querySpec(ns,
                            query.obj,
                            fieldsToReturn ? *fieldsToReturn : BSONObj(),
                            nToSkip,
                            nToReturn,
                            queryOptions);

        // TODO: Timeout must be passed down here as well - 30s timeout may not be applicable for
        // all operations handled.
        StatusWith<DBClientCursor*> status = queryOp.queryAny(getHosts(hostStrings),
                                                              querySpec,
                                                              30 * 1000);
        uassertStatusOK(status.getStatus());

        auto_ptr<DBClientCursor> cursor(status.getValue());
        return cursor;
    }

}

