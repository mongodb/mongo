// cloner.h - copy a database (export/import basically)

/**
 *    Copyright (C) 2011 10gen Inc.
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

#include "mongo/db/jsobj.h"

namespace mongo {

    struct CloneOptions;
    class DBClientBase;
    class DBClientCursor;
    class Query;

    class Cloner: boost::noncopyable {
    public:
        Cloner();
        /**
         *  slaveOk     - if true it is ok if the source of the data is !ismaster.
         *  useReplAuth - use the credentials we normally use as a replication slave for the cloning
         *  snapshot    - use $snapshot mode for copying collections.  note this should not be used
         *                when it isn't required, as it will be slower.  for example,
         *                repairDatabase need not use it.
         */
        void setConnection( DBClientBase *c ) { _conn.reset( c ); }

        /** copy the entire database */
        bool go(const char *masterHost, string& errmsg, const string& fromdb, bool logForRepl,
                bool slaveOk, bool useReplAuth, bool snapshot, bool mayYield,
                bool mayBeInterrupted, int *errCode = 0);

        bool go(const char *masterHost, const CloneOptions& opts, set<string>& clonedColls,
                string& errmsg, int *errCode = 0);

        bool go(const char *masterHost, const CloneOptions& opts, string& errmsg, int *errCode = 0);

        bool copyCollection(const string& ns, const BSONObj& query, string& errmsg,
                            bool mayYield, bool mayBeInterrupted, bool copyIndexes = true,
                            bool logForRepl = true );
        /**
         * validate the cloner query was successful
         * @param cur   Cursor the query was executed on
         * @param errCode out  Error code encountered during the query
         * @param errmsg out  Error message encountered during the query
         */
        static bool validateQueryResults(const auto_ptr<DBClientCursor>& cur, int32_t* errCode,
                                         string& errmsg);

        /**
         * @param errmsg out  - Error message (if encountered).
         * @param slaveOk     - if true it is ok if the source of the data is !ismaster.
         * @param useReplAuth - use the credentials we normally use as a replication slave for the
         *                      cloning.
         * @param snapshot    - use $snapshot mode for copying collections.  note this should not be
         *                      used when it isn't required, as it will be slower.  for example
         *                      repairDatabase need not use it.
         * @param errCode out - If provided, this will be set on error to the server's error code.
         *                      Currently this will only be set if there is an error in the initial
         *                      system.namespaces query.
         */
        static bool cloneFrom(const char *masterHost, string& errmsg, const string& fromdb,
                              bool logForReplication, bool slaveOk, bool useReplAuth,
                              bool snapshot, bool mayYield, bool mayBeInterrupted,
                              int *errCode = 0);

        static bool cloneFrom(const string& masterHost, const CloneOptions& options,
                              string& errmsg, int* errCode = 0,
                              set<string>* clonedCollections = 0);

        /**
         * Copy a collection (and indexes) from a remote host
         */
        static bool copyCollectionFromRemote(const string& host, const string& ns, string& errmsg);

    private:
        void copy(const char *from_ns, const char *to_ns, bool isindex, bool logForRepl,
                  bool masterSameProcess, bool slaveOk, bool mayYield, bool mayBeInterrupted,
                  Query q);

        struct Fun;
        auto_ptr<DBClientBase> _conn;
    };

    struct CloneOptions {

        CloneOptions() {
            logForRepl = true;
            slaveOk = false;
            useReplAuth = false;
            snapshot = true;
            mayYield = true;
            mayBeInterrupted = false;

            syncData = true;
            syncIndexes = true;
        }
            
        string fromDB;
        set<string> collsToIgnore;

        bool logForRepl;
        bool slaveOk;
        bool useReplAuth;
        bool snapshot;
        bool mayYield;
        bool mayBeInterrupted;

        bool syncData;
        bool syncIndexes;
    };

} // namespace mongo
