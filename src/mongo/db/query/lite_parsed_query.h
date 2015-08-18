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

#include <boost/optional.hpp>
#include <string>

#include "mongo/db/jsobj.h"
#include "mongo/db/namespace_string.h"

namespace mongo {

class QueryMessage;
class Status;
template <typename T>
class StatusWith;

/**
 * Parses the QueryMessage or find command received from the user and makes the various fields
 * more easily accessible.
 */
class LiteParsedQuery {
public:
    static const char kFindCommandName[];
    static const char kShardVersionField[];

    /**
     * Parses a find command object, 'cmdObj'. Caller must indicate whether or not this lite
     * parsed query is an explained query or not via 'isExplain'.
     *
     * Returns a heap allocated LiteParsedQuery on success or an error if 'cmdObj' is not well
     * formed.
     */
    static StatusWith<std::unique_ptr<LiteParsedQuery>> makeFromFindCommand(NamespaceString nss,
                                                                            const BSONObj& cmdObj,
                                                                            bool isExplain);

    /**
     * Constructs a LiteParseQuery object as though it is from a legacy QueryMessage.
     */
    static StatusWith<std::unique_ptr<LiteParsedQuery>> makeAsOpQuery(NamespaceString nss,
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
                                                                      bool explain);

    /**
     * Constructs a LiteParseQuery object that can be used to serialize to find command
     * BSON object.
     *
     * Input must be fully validated (e.g. if there is a limit value, it must be non-negative).
     */
    static std::unique_ptr<LiteParsedQuery> makeAsFindCmd(
        NamespaceString nss,
        const BSONObj& filter = BSONObj(),
        const BSONObj& projection = BSONObj(),
        const BSONObj& sort = BSONObj(),
        const BSONObj& hint = BSONObj(),
        boost::optional<long long> skip = boost::none,
        boost::optional<long long> limit = boost::none,
        boost::optional<long long> batchSize = boost::none,
        boost::optional<long long> ntoreturn = boost::none,
        bool wantMore = true,
        bool isExplain = false,
        const std::string& comment = "",
        int maxScan = 0,
        int maxTimeMS = 0,
        const BSONObj& min = BSONObj(),
        const BSONObj& max = BSONObj(),
        bool returnKey = false,
        bool showRecordId = false,
        bool isSnapshot = false,
        bool hasReadPref = false,
        bool isTailable = false,
        bool isSlaveOk = false,
        bool isOplogReplay = false,
        bool isNoCursorTimeout = false,
        bool isAwaitData = false,
        bool isPartial = false);

    /**
     * Converts this LPQ into a find command.
     */
    BSONObj asFindCommand() const;
    void asFindCommand(BSONObjBuilder* cmdBuilder) const;

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

    // Name of the find command parameter used to pass read preference.
    static const char* kFindCommandReadPrefField;

    // Names of the maxTimeMS command and query option.
    static const std::string cmdOptionMaxTimeMS;
    static const std::string queryOptionMaxTimeMS;

    // Names of the $meta projection values.
    static const std::string metaGeoNearDistance;
    static const std::string metaGeoNearPoint;
    static const std::string metaIndexKey;
    static const std::string metaRecordId;
    static const std::string metaSortKey;
    static const std::string metaTextScore;

    const NamespaceString& nss() const {
        return _nss;
    }
    const std::string& ns() const {
        return _nss.ns();
    }

    const BSONObj& getFilter() const {
        return _filter;
    }
    const BSONObj& getProj() const {
        return _proj;
    }
    const BSONObj& getSort() const {
        return _sort;
    }
    const BSONObj& getHint() const {
        return _hint;
    }

    static const long long kDefaultBatchSize;

    boost::optional<long long> getSkip() const {
        return _skip;
    }
    boost::optional<long long> getLimit() const {
        return _limit;
    }
    boost::optional<long long> getBatchSize() const {
        return _batchSize;
    }
    boost::optional<long long> getNToReturn() const {
        return _ntoreturn;
    }

    /**
     * Returns batchSize or ntoreturn value if either is set. If neither is set,
     * returns boost::none.
     */
    boost::optional<long long> getEffectiveBatchSize() const;

    bool wantMore() const {
        return _wantMore;
    }

    bool isExplain() const {
        return _explain;
    }

    const std::string& getComment() const {
        return _comment;
    }

    int getMaxScan() const {
        return _maxScan;
    }
    int getMaxTimeMS() const {
        return _maxTimeMS;
    }

    const BSONObj& getMin() const {
        return _min;
    }
    const BSONObj& getMax() const {
        return _max;
    }

    bool returnKey() const {
        return _returnKey;
    }
    bool showRecordId() const {
        return _showRecordId;
    }
    bool isSnapshot() const {
        return _snapshot;
    }
    bool hasReadPref() const {
        return _hasReadPref;
    }

    bool isTailable() const {
        return _tailable;
    }
    bool isSlaveOk() const {
        return _slaveOk;
    }
    bool isOplogReplay() const {
        return _oplogReplay;
    }
    bool isNoCursorTimeout() const {
        return _noCursorTimeout;
    }
    bool isAwaitData() const {
        return _awaitData;
    }
    bool isExhaust() const {
        return _exhaust;
    }
    bool isPartial() const {
        return _partial;
    }

    boost::optional<long long> getReplicationTerm() const {
        return _replicationTerm;
    }

    /**
     * Return options as a bit vector.
     */
    int getOptions() const;

    //
    // Old parsing code: SOON TO BE DEPRECATED.
    //

    /**
     * Parse the provided QueryMessage and return a heap constructed LiteParsedQuery, which
     * represents it or an error.
     */
    static StatusWith<std::unique_ptr<LiteParsedQuery>> fromLegacyQueryMessage(
        const QueryMessage& qm);

private:
    LiteParsedQuery(NamespaceString nss);

    /**
     * Parsing code calls this after construction of the LPQ is complete. There are additional
     * semantic properties that must be checked even if "lexically" the parse is OK.
     */
    Status validate() const;

    Status init(int ntoskip,
                int ntoreturn,
                int queryOptions,
                const BSONObj& queryObj,
                const BSONObj& proj,
                bool fromQueryMessage);

    Status initFullQuery(const BSONObj& top);

    static StatusWith<int> parseMaxTimeMS(const BSONElement& maxTimeMSElt);

    /**
     * Updates the projection object with a $meta projection for the returnKey option.
     */
    void addReturnKeyMetaProj();

    /**
     * Updates the projection object with a $meta projection for the showRecordId option.
     */
    void addShowRecordIdMetaProj();

    /**
     * Initializes options based on the value of the 'options' bit vector.
     *
     * This contains flags such as tailable, exhaust, and noCursorTimeout.
     */
    void initFromInt(int options);

    /**
     * Add the meta projection to this object if needed.
     */
    void addMetaProjection();

    /**
     * Returns OK if this is valid in the find command context.
     */
    Status validateFindCmd();

    const NamespaceString _nss;

    BSONObj _filter;
    BSONObj _proj;
    BSONObj _sort;
    // The hint provided, if any.  If the hint was by index key pattern, the value of '_hint' is
    // the key pattern hinted.  If the hint was by index name, the value of '_hint' is
    // {$hint: <String>}, where <String> is the index name hinted.
    BSONObj _hint;

    bool _wantMore = true;

    // Must be either unset or positive. Negative skip is illegal and a skip of zero received from
    // the client is interpreted as the absence of a skip value.
    boost::optional<long long> _skip;

    // Must be either unset or positive. Negative limit is illegal and a limit value of zero
    // received from the client is interpreted as the absence of a limit value.
    boost::optional<long long> _limit;

    // Must be either unset or non-negative. Negative batchSize is illegal but batchSize of 0 is
    // allowed.
    boost::optional<long long> _batchSize;

    // Set only when parsed from an OP_QUERY find message. The value is computed by driver or shell
    // and is set to be a min of batchSize and limit provided by user. LPQ can have set either
    // ntoreturn or batchSize / limit.
    boost::optional<long long> _ntoreturn;

    bool _explain = false;

    std::string _comment;

    int _maxScan = 0;
    int _maxTimeMS = 0;

    BSONObj _min;
    BSONObj _max;

    bool _returnKey = false;
    bool _showRecordId = false;
    bool _snapshot = false;
    bool _hasReadPref = false;

    // Options that can be specified in the OP_QUERY 'flags' header.
    bool _tailable = false;
    bool _slaveOk = false;
    bool _oplogReplay = false;
    bool _noCursorTimeout = false;
    bool _awaitData = false;
    bool _exhaust = false;
    bool _partial = false;

    boost::optional<long long> _replicationTerm;
};

}  // namespace mongo
