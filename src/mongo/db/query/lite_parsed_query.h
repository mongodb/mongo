/**
 *    Copyright 2013 10gen Inc.
 *
 *    Licensed under the Apache License, Version 2.0 (the "License");
 *    you may not use this file except in compliance with the License.
 *    You may obtain a copy of the License at
 *
 *    http://www.apache.org/licenses/LICENSE-2.0
 *
 *    Unless required by applicable law or agreed to in writing, software
 *    distributed under the License is distributed on an "AS IS" BASIS,
 *    WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 *    See the License for the specific language governing permissions and
 *    limitations under the License.
 */

#pragma once

#include "mongo/db/jsobj.h"

namespace mongo {

    class QueryMessage;

    /**
     * Parses the QueryMessage received from the user and makes the various fields more easily
     * accessible.
     */
    class LiteParsedQuery {
    public:
        /**
         * Parse the provided QueryMessage and set *out to point to the output.
         *
         * Return Status::OK() if parsing succeeded.  Caller owns *out.
         * Otherwise, *out is invalid and the returned Status indicates why parsing failed.
         */
        static Status make(const QueryMessage& qm, LiteParsedQuery** out);

        /**
         * Fills out a LiteParsedQuery.  Used for debugging and testing, when we don't have a
         * QueryMessage.
         */
        static Status make(const string& ns,
                           int ntoskip,
                           int ntoreturn,
                           int queryoptions,
                           const BSONObj& query,
                           const BSONObj& proj,
                           const BSONObj& sort,
                           LiteParsedQuery** out);

        /**
         * Helper function to parse a "maxTimeMS" BSONElement.  Returns the contained value, or an
         * error on parsing fail.  When passed an EOO-type element, returns 0 (special value for
         * "allow to run indefinitely").
         */
        static StatusWith<int> parseMaxTimeMS(const BSONElement& maxTimeMSElt);

        const string& ns() const { return _ns; }
        bool isLocalDB() const { return _ns.compare(0, 6, "local.") == 0; }

        const BSONObj& getFilter() const { return _filter; }
        const BSONObj& getProj() const { return _proj; }
        const BSONObj& getSort() const { return _sort; }
        const BSONObj& getHint() const { return _hint; }

        int getSkip() const { return _ntoskip; }
        int getNumToReturn() const { return _ntoreturn; }
        bool wantMore() const { return _wantMore; }
        int getOptions() const { return _options; }
        bool hasOption(int x) const { return ( x & _options ) != 0; }
        bool hasReadPref() const { return _hasReadPref; }

        bool isExplain() const { return _explain; }
        bool isSnapshot() const { return _snapshot; }
        bool returnKey() const { return _returnKey; }
        bool showDiskLoc() const { return _showDiskLoc; }

        const BSONObj& getMin() const { return _min; }
        const BSONObj& getMax() const { return _max; }
        int getMaxScan() const { return _maxScan; }
        int getMaxTimeMS() const { return _maxTimeMS; }
        
    private:
        LiteParsedQuery();

        Status init(const string& ns, int ntoskip, int ntoreturn, int queryOptions,
                    const BSONObj& queryObj, const BSONObj& proj, bool fromQueryMessage);

        Status initFullQuery(const BSONObj& top);

        string _ns;
        int _ntoskip;
        int _ntoreturn;
        BSONObj _filter;
        BSONObj _sort;
        BSONObj _proj;
        int _options;
        bool _wantMore;
        bool _explain;
        bool _snapshot;
        bool _returnKey;
        bool _showDiskLoc;
        bool _hasReadPref;
        BSONObj _min;
        BSONObj _max;
        BSONObj _hint;
        int _maxScan;
        int _maxTimeMS;
    };

} // namespace mongo
