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
class QueryRequest {
public:
    static const char kFindCommandName[];
    static const char kShardVersionField[];

    QueryRequest(NamespaceString nss);

    /**
     * Returns a non-OK status if any property of the QR has a bad value (e.g. a negative skip
     * value) or if there is a bad combination of options (e.g. awaitData is illegal without
     * tailable).
     */
    Status validate() const;

    /**
     * Parses a find command object, 'cmdObj'. Caller must indicate whether or not this lite
     * parsed query is an explained query or not via 'isExplain'.
     *
     * Returns a heap allocated QueryRequest on success or an error if 'cmdObj' is not well
     * formed.
     */
    static StatusWith<std::unique_ptr<QueryRequest>> makeFromFindCommand(NamespaceString nss,
                                                                         const BSONObj& cmdObj,
                                                                         bool isExplain);

    /**
     * Converts this QR into a find command.
     */
    BSONObj asFindCommand() const;
    void asFindCommand(BSONObjBuilder* cmdBuilder) const;

    /**
     * Parses maxTimeMS from the BSONElement containing its value.
     */
    static StatusWith<int> parseMaxTimeMS(BSONElement maxTimeMSElt);

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

    // Read preference is attached to commands in "wrapped" form, e.g.
    //   { $query: { <cmd>: ... } , <kWrappedReadPrefField>: { ... } }
    //
    // However, mongos internally "unwraps" the read preference and adds it as a parameter to the
    // command, e.g.
    //  { <cmd>: ... , <kUnwrappedReadPrefField>: { <kWrappedReadPrefField>: { ... } } }
    static const std::string kWrappedReadPrefField;
    static const std::string kUnwrappedReadPrefField;

    // Names of the maxTimeMS command and query option.
    // Char arrays because they are used in static initialization.
    static const char cmdOptionMaxTimeMS[];
    static const char queryOptionMaxTimeMS[];

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

    void setFilter(BSONObj filter) {
        _filter = filter.getOwned();
    }

    const BSONObj& getProj() const {
        return _proj;
    }

    void setProj(BSONObj proj) {
        _proj = proj.getOwned();
    }

    const BSONObj& getSort() const {
        return _sort;
    }

    void setSort(BSONObj sort) {
        _sort = sort.getOwned();
    }

    const BSONObj& getHint() const {
        return _hint;
    }

    void setHint(BSONObj hint) {
        _hint = hint.getOwned();
    }

    const BSONObj& getReadConcern() const {
        return _readConcern;
    }

    void setReadConcern(BSONObj readConcern) {
        _readConcern = readConcern.getOwned();
    }

    const BSONObj& getCollation() const {
        return _collation;
    }

    void setCollation(BSONObj collation) {
        _collation = collation.getOwned();
    }

    static const long long kDefaultBatchSize;

    boost::optional<long long> getSkip() const {
        return _skip;
    }

    void setSkip(boost::optional<long long> skip) {
        _skip = skip;
    }

    boost::optional<long long> getLimit() const {
        return _limit;
    }

    void setLimit(boost::optional<long long> limit) {
        _limit = limit;
    }

    boost::optional<long long> getBatchSize() const {
        return _batchSize;
    }

    void setBatchSize(boost::optional<long long> batchSize) {
        _batchSize = batchSize;
    }

    boost::optional<long long> getNToReturn() const {
        return _ntoreturn;
    }

    void setNToReturn(boost::optional<long long> ntoreturn) {
        _ntoreturn = ntoreturn;
    }

    /**
     * Returns batchSize or ntoreturn value if either is set. If neither is set,
     * returns boost::none.
     */
    boost::optional<long long> getEffectiveBatchSize() const;

    bool wantMore() const {
        return _wantMore;
    }

    void setWantMore(bool wantMore) {
        _wantMore = wantMore;
    }

    bool isExplain() const {
        return _explain;
    }

    void setExplain(bool explain) {
        _explain = explain;
    }

    const std::string& getComment() const {
        return _comment;
    }

    void setComment(const std::string& comment) {
        _comment = comment;
    }

    int getMaxScan() const {
        return _maxScan;
    }

    void setMaxScan(int maxScan) {
        _maxScan = maxScan;
    }

    int getMaxTimeMS() const {
        return _maxTimeMS;
    }

    void setMaxTimeMS(int maxTimeMS) {
        _maxTimeMS = maxTimeMS;
    }

    const BSONObj& getMin() const {
        return _min;
    }

    void setMin(BSONObj min) {
        _min = min.getOwned();
    }

    const BSONObj& getMax() const {
        return _max;
    }

    void setMax(BSONObj max) {
        _max = max.getOwned();
    }

    bool returnKey() const {
        return _returnKey;
    }

    void setReturnKey(bool returnKey) {
        _returnKey = returnKey;
    }

    bool showRecordId() const {
        return _showRecordId;
    }

    void setShowRecordId(bool showRecordId) {
        _showRecordId = showRecordId;
    }

    bool isSnapshot() const {
        return _snapshot;
    }

    void setSnapshot(bool snapshot) {
        _snapshot = snapshot;
    }

    bool hasReadPref() const {
        return _hasReadPref;
    }

    void setHasReadPref(bool hasReadPref) {
        _hasReadPref = hasReadPref;
    }

    bool isTailable() const {
        return _tailable;
    }

    void setTailable(bool tailable) {
        _tailable = tailable;
    }

    bool isSlaveOk() const {
        return _slaveOk;
    }

    void setSlaveOk(bool slaveOk) {
        _slaveOk = slaveOk;
    }

    bool isOplogReplay() const {
        return _oplogReplay;
    }

    void setOplogReplay(bool oplogReplay) {
        _oplogReplay = oplogReplay;
    }

    bool isNoCursorTimeout() const {
        return _noCursorTimeout;
    }

    void setNoCursorTimeout(bool noCursorTimeout) {
        _noCursorTimeout = noCursorTimeout;
    }

    bool isAwaitData() const {
        return _awaitData;
    }

    void setAwaitData(bool awaitData) {
        _awaitData = awaitData;
    }

    bool isExhaust() const {
        return _exhaust;
    }

    void setExhaust(bool exhaust) {
        _exhaust = exhaust;
    }

    bool isAllowPartialResults() const {
        return _allowPartialResults;
    }

    void setAllowPartialResults(bool allowPartialResults) {
        _allowPartialResults = allowPartialResults;
    }

    boost::optional<long long> getReplicationTerm() const {
        return _replicationTerm;
    }

    void setReplicationTerm(boost::optional<long long> replicationTerm) {
        _replicationTerm = replicationTerm;
    }

    /**
     * Return options as a bit vector.
     */
    int getOptions() const;

    //
    // Old parsing code: SOON TO BE DEPRECATED.
    //

    /**
     * Parse the provided QueryMessage and return a heap constructed QueryRequest, which
     * represents it or an error.
     */
    static StatusWith<std::unique_ptr<QueryRequest>> fromLegacyQueryMessage(const QueryMessage& qm);

private:
    Status init(int ntoskip,
                int ntoreturn,
                int queryOptions,
                const BSONObj& queryObj,
                const BSONObj& proj,
                bool fromQueryMessage);

    Status initFullQuery(const BSONObj& top);

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

    const NamespaceString _nss;

    BSONObj _filter;
    BSONObj _proj;
    BSONObj _sort;
    // The hint provided, if any.  If the hint was by index key pattern, the value of '_hint' is
    // the key pattern hinted.  If the hint was by index name, the value of '_hint' is
    // {$hint: <String>}, where <String> is the index name hinted.
    BSONObj _hint;
    // The read concern is parsed elsewhere.
    BSONObj _readConcern;
    // The collation is parsed elsewhere.
    BSONObj _collation;

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
    // and is set to be a min of batchSize and limit provided by user. QR can have set either
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
    bool _allowPartialResults = false;

    boost::optional<long long> _replicationTerm;
};

}  // namespace mongo
