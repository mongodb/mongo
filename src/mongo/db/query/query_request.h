/**
 *    Copyright (C) 2018-present MongoDB, Inc.
 *
 *    This program is free software: you can redistribute it and/or modify
 *    it under the terms of the Server Side Public License, version 1,
 *    as published by MongoDB, Inc.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    Server Side Public License for more details.
 *
 *    You should have received a copy of the Server Side Public License
 *    along with this program. If not, see
 *    <http://www.mongodb.com/licensing/server-side-public-license>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the Server Side Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */

#pragma once

#include <boost/optional.hpp>
#include <string>

#include "mongo/db/catalog/collection_options.h"
#include "mongo/db/jsobj.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/pipeline/runtime_constants_gen.h"
#include "mongo/db/query/tailable_mode.h"

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
    static const char kFilterField[];
    static const char kProjectionField[];
    static const char kSortField[];
    static const char kHintField[];
    static const char kCollationField[];
    static const char kSkipField[];
    static const char kLimitField[];
    static const char kBatchSizeField[];
    static const char kNToReturnField[];
    static const char kSingleBatchField[];
    static const char kMaxField[];
    static const char kMinField[];
    static const char kReturnKeyField[];
    static const char kShowRecordIdField[];
    static const char kTailableField[];
    static const char kOplogReplayField[];
    static const char kNoCursorTimeoutField[];
    static const char kAwaitDataField[];
    static const char kPartialResultsField[];
    static const char kRuntimeConstantsField[];
    static const char kTermField[];
    static const char kOptionsField[];
    static const char kReadOnceField[];
    static const char kAllowSpeculativeMajorityReadField[];
    static const char kInternalReadAtClusterTimeField[];
    static const char kRequestResumeTokenField[];
    static const char kResumeAfterField[];
    static const char kUse44SortKeys[];
    static const char kMaxTimeMSOpOnlyField[];

    static const char kNaturalSortField[];

    static const char kFindCommandName[];
    static const char kShardVersionField[];

    explicit QueryRequest(NamespaceStringOrUUID nss);

    /**
     * Returns a non-OK status if any property of the QR has a bad value (e.g. a negative skip
     * value) or if there is a bad combination of options (e.g. awaitData is illegal without
     * tailable).
     */
    Status validate() const;

    /**
     * Parses a find command object, 'cmdObj'. Caller must indicate whether or not this lite
     * parsed query is an explained query or not via 'isExplain'. Accepts a NSS with which
     * to initialize the QueryRequest if there is no UUID in cmdObj.
     *
     * Returns a heap allocated QueryRequest on success or an error if 'cmdObj' is not well
     * formed.
     */
    static StatusWith<std::unique_ptr<QueryRequest>> makeFromFindCommand(NamespaceString nss,
                                                                         const BSONObj& cmdObj,
                                                                         bool isExplain);

    /**
     * If _uuid exists for this QueryRequest, use it to update the value of _nss via the
     * CollectionCatalog associated with opCtx. This should only be called when we hold a DBLock
     * on the database to which _uuid belongs, if the _uuid is present in the CollectionCatalog.
     */
    void refreshNSS(OperationContext* opCtx);

    /**
     * Converts this QR into a find command.
     * The withUuid variants make a UUID-based find command instead of a namespace-based ones.
     */
    BSONObj asFindCommand() const;
    BSONObj asFindCommandWithUuid() const;
    void asFindCommand(BSONObjBuilder* cmdBuilder) const;
    void asFindCommandWithUuid(BSONObjBuilder* cmdBuilder) const;

    /**
     * Converts this QR into an aggregation using $match. If this QR has options that cannot be
     * satisfied by aggregation, a non-OK status is returned and 'cmdBuilder' is not modified.
     */
    StatusWith<BSONObj> asAggregationCommand() const;

    /**
     * Parses maxTimeMS from the BSONElement containing its value.
     */
    static StatusWith<int> parseMaxTimeMS(BSONElement maxTimeMSElt);

    /**
     * Helper function to identify text search sort key
     * Example: {a: {$meta: "textScore"}}
     */
    static bool isTextScoreMeta(BSONElement elt);

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
    static const std::string metaRecordId;
    static const std::string metaSortKey;
    static const std::string metaTextScore;

    // Allow using disk during the find command.
    static const std::string kAllowDiskUseField;

    const NamespaceString& nss() const {
        return _nss;
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
        if (_readConcern) {
            return *_readConcern;
        } else {
            static const auto empty = BSONObj();
            return empty;
        }
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

    bool allowDiskUse() const {
        return _allowDiskUse;
    }

    void setAllowDiskUse(bool allowDiskUse) {
        _allowDiskUse = allowDiskUse;
    }

    bool isExplain() const {
        return _explain;
    }

    void setExplain(bool explain) {
        _explain = explain;
    }

    const BSONObj& getUnwrappedReadPref() const {
        return _unwrappedReadPref;
    }

    void setUnwrappedReadPref(BSONObj unwrappedReadPref) {
        _unwrappedReadPref = unwrappedReadPref.getOwned();
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

    bool hasReadPref() const {
        return _hasReadPref;
    }

    void setHasReadPref(bool hasReadPref) {
        _hasReadPref = hasReadPref;
    }

    bool isTailable() const {
        return _tailableMode == TailableModeEnum::kTailable ||
            _tailableMode == TailableModeEnum::kTailableAndAwaitData;
    }

    bool isTailableAndAwaitData() const {
        return _tailableMode == TailableModeEnum::kTailableAndAwaitData;
    }

    void setTailableMode(TailableModeEnum tailableMode) {
        _tailableMode = tailableMode;
    }

    TailableModeEnum getTailableMode() const {
        return _tailableMode;
    }

    void setRuntimeConstants(RuntimeConstants runtimeConstants) {
        _runtimeConstants = std::move(runtimeConstants);
    }

    const boost::optional<RuntimeConstants>& getRuntimeConstants() const {
        return _runtimeConstants;
    }

    bool isSlaveOk() const {
        return _slaveOk;
    }

    void setSlaveOk(bool slaveOk) {
        _slaveOk = slaveOk;
    }

    bool isNoCursorTimeout() const {
        return _noCursorTimeout;
    }

    void setNoCursorTimeout(bool noCursorTimeout) {
        _noCursorTimeout = noCursorTimeout;
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

    bool isReadOnce() const {
        return _readOnce;
    }

    void setReadOnce(bool readOnce) {
        _readOnce = readOnce;
    }

    void setAllowSpeculativeMajorityRead(bool allowSpeculativeMajorityRead) {
        _allowSpeculativeMajorityRead = allowSpeculativeMajorityRead;
    }

    bool allowSpeculativeMajorityRead() const {
        return _allowSpeculativeMajorityRead;
    }

    boost::optional<Timestamp> getReadAtClusterTime() const {
        return _internalReadAtClusterTime;
    }

    bool getRequestResumeToken() const {
        return _requestResumeToken;
    }

    void setRequestResumeToken(bool requestResumeToken) {
        _requestResumeToken = requestResumeToken;
    }

    const BSONObj& getResumeAfter() const {
        return _resumeAfter;
    }

    void setResumeAfter(BSONObj resumeAfter) {
        _resumeAfter = resumeAfter;
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

    /**
     * Parse the provided legacy query object and parameters to construct a QueryRequest.
     */
    static StatusWith<std::unique_ptr<QueryRequest>> fromLegacyQuery(NamespaceStringOrUUID nsOrUuid,
                                                                     const BSONObj& queryObj,
                                                                     const BSONObj& proj,
                                                                     int ntoskip,
                                                                     int ntoreturn,
                                                                     int queryOptions);

private:
    static StatusWith<std::unique_ptr<QueryRequest>> parseFromFindCommand(
        std::unique_ptr<QueryRequest> qr, const BSONObj& cmdObj, bool isExplain);
    Status init(int ntoskip,
                int ntoreturn,
                int queryOptions,
                const BSONObj& queryObj,
                const BSONObj& proj,
                bool fromQueryMessage);

    Status initFullQuery(const BSONObj& top);

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
     * Common code for UUID and namespace-based find commands.
     */
    void asFindCommandInternal(BSONObjBuilder* cmdBuilder) const;

    NamespaceString _nss;
    OptionalCollectionUUID _uuid;

    BSONObj _filter;
    BSONObj _proj;
    BSONObj _sort;
    // The hint provided, if any.  If the hint was by index key pattern, the value of '_hint' is
    // the key pattern hinted.  If the hint was by index name, the value of '_hint' is
    // {$hint: <String>}, where <String> is the index name hinted.
    BSONObj _hint;
    // The read concern is parsed elsewhere.
    boost::optional<BSONObj> _readConcern;
    // The collation is parsed elsewhere.
    BSONObj _collation;

    // The unwrapped readPreference object, if one was given to us by the mongos command processor.
    // This object will be empty when no readPreference is specified or if the request does not
    // originate from mongos.
    BSONObj _unwrappedReadPref;

    // If true, each cursor response will include a 'postBatchResumeToken' field containing the
    // RecordID of the last observed document.
    bool _requestResumeToken = false;
    // If non-empty, instructs the query to resume from the RecordId given by the object's $recordId
    // field.
    BSONObj _resumeAfter;

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

    bool _allowDiskUse = false;

    // Set only when parsed from an OP_QUERY find message. The value is computed by driver or shell
    // and is set to be a min of batchSize and limit provided by user. QR can have set either
    // ntoreturn or batchSize / limit.
    boost::optional<long long> _ntoreturn;

    bool _explain = false;

    // A user-specified maxTimeMS limit, or a value of '0' if not specified.
    int _maxTimeMS = 0;

    BSONObj _min;
    BSONObj _max;

    bool _returnKey = false;
    bool _showRecordId = false;
    bool _hasReadPref = false;

    // Runtime constants which may be referenced by $expr, if present.
    boost::optional<RuntimeConstants> _runtimeConstants;

    // Options that can be specified in the OP_QUERY 'flags' header.
    TailableModeEnum _tailableMode = TailableModeEnum::kNormal;
    bool _slaveOk = false;
    bool _noCursorTimeout = false;
    bool _exhaust = false;
    bool _allowPartialResults = false;
    bool _readOnce = false;
    bool _allowSpeculativeMajorityRead = false;

    boost::optional<long long> _replicationTerm;

    // The Timestamp that RecoveryUnit::setTimestampReadSource() should be called with. The optional
    // should only ever be engaged when testing commands are enabled.
    boost::optional<Timestamp> _internalReadAtClusterTime;
};

}  // namespace mongo
