/**
 *    Copyright 2013 10gen Inc.
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
 *    must comply with the GNU Affero General Public License in all respects
 *    for all of the code used other than as permitted herein. If you modify
 *    file(s) with this exception, you may extend this exception to your
 *    version of the file(s), but you are not obligated to do so. If you do not
 *    wish to do so, delete this exception statement from your version. If you
 *    delete this exception statement from all source files in the program,
 *    then also delete it in the license file.
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
        static Status make(const std::string& ns,
                           int ntoskip,
                           int ntoreturn,
                           int queryoptions,
                           const BSONObj& query,
                           const BSONObj& proj,
                           const BSONObj& sort,
                           const BSONObj& hint,
                           const BSONObj& minObj,
                           const BSONObj& maxObj,
                           bool snapshot,
                           bool explain,
                           LiteParsedQuery** out);

        /**
         * Helper functions to parse maxTimeMS from a command object.  Returns the contained value,
         * or an error on parsing fail.  When passed an EOO-type element, returns 0 (special value
         * for "allow to run indefinitely").
         */
        static StatusWith<int> parseMaxTimeMSCommand(const BSONObj& cmdObj);

        /**
         * Same as parseMaxTimeMSCommand, but for a query object.
         */
        static StatusWith<int> parseMaxTimeMSQuery(const BSONObj& queryObj);

        /**
         * Helper function to identify text search sort key
         * Example: {a: {$meta: "textScore"}}
         */
        static bool isTextScoreMeta(BSONElement elt);

        /**
         * Helper function to identify diskLoc projection
         * Example: {a: {$meta: "diskloc"}}.
         */
        static bool isDiskLocMeta(BSONElement elt);

        /**
         * Helper function to validate a sort object.
         * Returns true if each element satisfies one of:
         * 1. a number with value 1
         * 2. a number with value -1
         * 3. isTextScoreMeta
         */
        static bool isValidSortOrder(const BSONObj& sortObj);

        /**
         * Returns true if the query described by "query" should execute
         * at an elevated level of isolation (i.e., $isolated was specified).
         */
        static bool isQueryIsolated(const BSONObj& query);

        /**
         * Helper function to create a normalized sort object.
         * Each element of the object returned satisfies one of:
         * 1. a number with value 1
         * 2. a number with value -1
         * 3. isTextScoreMeta
         */
        static BSONObj normalizeSortOrder(const BSONObj& sortObj);

        // Names of the maxTimeMS command and query option.
        static const std::string cmdOptionMaxTimeMS;
        static const std::string queryOptionMaxTimeMS;

        // Names of the $meta projection values.
        static const std::string metaTextScore;
        static const std::string metaGeoNearDistance;
        static const std::string metaGeoNearPoint;
        static const std::string metaDiskLoc;
        static const std::string metaIndexKey;

        const std::string& ns() const { return _ns; }
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

        Status init(const std::string& ns, int ntoskip, int ntoreturn, int queryOptions,
                    const BSONObj& queryObj, const BSONObj& proj, bool fromQueryMessage);

        Status initFullQuery(const BSONObj& top);

        static StatusWith<int> parseMaxTimeMS(const BSONElement& maxTimeMSElt);

        std::string _ns;
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
