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
#include "mongo/db/pipeline/legacy_runtime_constants_gen.h"
#include "mongo/db/query/find_command_gen.h"
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
    static constexpr auto kMaxTimeMSOpOnlyField = "maxTimeMSOpOnly";

    // Field names for sorting options.
    static constexpr auto kNaturalSortField = "$natural";

    static constexpr auto kShardVersionField = "shardVersion";

    explicit QueryRequest(NamespaceStringOrUUID nss, bool preferNssForSerialization = true);
    explicit QueryRequest(FindCommand findCommand);

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
    static std::unique_ptr<QueryRequest> makeFromFindCommand(
        const BSONObj& cmdObj, bool isExplain, boost::optional<NamespaceString> nss = boost::none);

    /**
     * If _uuid exists for this QueryRequest, update the value of _nss.
     */
    void refreshNSS(const NamespaceString& nss);

    void setNSS(const NamespaceString& nss) {
        auto& nssOrUuid = _findCommand.getNamespaceOrUUID();
        nssOrUuid.setNss(nss);
    }

    /**
     * Converts this QR into a find command.
     * The withUuid variants make a UUID-based find command instead of a namespace-based ones.
     */
    BSONObj asFindCommand() const;

    /**
     * Common code for UUID and namespace-based find commands.
     */
    void asFindCommand(BSONObjBuilder* cmdBuilder) const;

    /**
     * Converts this QR into an aggregation using $match. If this QR has options that cannot be
     * satisfied by aggregation, a non-OK status is returned and 'cmdBuilder' is not modified.
     */
    StatusWith<BSONObj> asAggregationCommand() const;

    /**
     * Parses maxTimeMS from the BSONElement containing its value.
     * The field name of the 'maxTimeMSElt' is used to determine what maximum value to enforce for
     * the provided max time. 'maxTimeMSOpOnly' needs a slightly higher max value than regular
     * 'maxTimeMS' to account for the case where a user provides the max possible value for
     * 'maxTimeMS' to one server process (mongod or mongos), then that server process passes the max
     * time on to another server as 'maxTimeMSOpOnly', but after adding a small amount to the max
     * time to account for clock precision.  This can push the 'maxTimeMSOpOnly' sent to the mongod
     * over the max value allowed for users to provide. This is safe because 'maxTimeMSOpOnly' is
     * only allowed to be provided for internal intra-cluster requests.
     */
    static StatusWith<int> parseMaxTimeMS(BSONElement maxTimeMSElt);

    static int32_t parseMaxTimeMSForIDL(BSONElement maxTimeMSElt);

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
    static constexpr auto kWrappedReadPrefField = "$readPreference";
    static constexpr auto kUnwrappedReadPrefField = "$queryOptions";

    // Names of the maxTimeMS command and query option.
    // Char arrays because they are used in static initialization.
    static constexpr auto cmdOptionMaxTimeMS = "maxTimeMS";
    static constexpr auto queryOptionMaxTimeMS = "$maxTimeMS";

    // Names of the $meta projection values.
    static constexpr auto metaGeoNearDistance = "geoNearDistance";
    static constexpr auto metaGeoNearPoint = "geoNearPoint";
    static constexpr auto metaRecordId = "recordId";
    static constexpr auto metaSortKey = "sortKey";
    static constexpr auto metaTextScore = "textScore";

    // Allow using disk during the find command.
    static constexpr auto kAllowDiskUseField = "allowDiskUse";

    // A constant by which 'maxTimeMSOpOnly' values are allowed to exceed the max allowed value for
    // 'maxTimeMS'.  This is because mongod and mongos server processes add a small amount to the
    // 'maxTimeMS' value they are given before passing it on as 'maxTimeMSOpOnly', to allow for
    // clock precision.
    static constexpr auto kMaxTimeMSOpOnlyMaxPadding = 100LL;

    const NamespaceString& nss() const {
        if (_findCommand.getNamespaceOrUUID().nss()) {
            return *_findCommand.getNamespaceOrUUID().nss();
        } else {
            static NamespaceString nss = NamespaceString();
            return nss;
        }
    }

    boost::optional<UUID> uuid() const {
        return _findCommand.getNamespaceOrUUID().uuid();
    }

    const BSONObj& getFilter() const {
        return _findCommand.getFilter();
    }

    void setFilter(BSONObj filter) {
        _findCommand.setFilter(filter.getOwned());
    }

    const BSONObj& getProj() const {
        return _findCommand.getProjection();
    }

    void setProj(BSONObj proj) {
        _findCommand.setProjection(proj.getOwned());
    }

    const BSONObj& getSort() const {
        return _findCommand.getSort();
    }

    void setSort(BSONObj sort) {
        _findCommand.setSort(sort.getOwned());
    }

    const BSONObj& getHint() const {
        return _findCommand.getHint();
    }

    void setHint(BSONObj hint) {
        _findCommand.setHint(hint.getOwned());
    }

    boost::optional<BSONObj> getReadConcern() const {
        return _findCommand.getReadConcern();
    }

    void setReadConcern(BSONObj readConcern) {
        _findCommand.setReadConcern(readConcern.getOwned());
    }

    const BSONObj& getCollation() const {
        return _findCommand.getCollation();
    }

    void setCollation(BSONObj collation) {
        _findCommand.setCollation(collation.getOwned());
    }

    static constexpr auto kDefaultBatchSize = 101ll;

    boost::optional<std::int64_t> getSkip() const {
        return _findCommand.getSkip();
    }

    void setSkip(boost::optional<std::int64_t> skip) {
        _findCommand.setSkip(skip);
    }

    boost::optional<std::int64_t> getLimit() const {
        return _findCommand.getLimit();
    }

    void setLimit(boost::optional<std::int64_t> limit) {
        _findCommand.setLimit(limit);
    }

    boost::optional<std::int64_t> getBatchSize() const {
        return _findCommand.getBatchSize();
    }

    void setBatchSize(boost::optional<std::int64_t> batchSize) {
        _findCommand.setBatchSize(batchSize);
    }

    boost::optional<std::int64_t> getNToReturn() const {
        return _findCommand.getNtoreturn();
    }

    void setNToReturn(boost::optional<std::int64_t> ntoreturn) {
        _findCommand.setNtoreturn(ntoreturn);
    }

    /**
     * Returns batchSize or ntoreturn value if either is set. If neither is set,
     * returns boost::none.
     */
    boost::optional<std::int64_t> getEffectiveBatchSize() const;

    bool isSingleBatch() const {
        return _findCommand.getSingleBatch();
    }

    void setSingleBatchField(bool singleBatch) {
        _findCommand.setSingleBatch(singleBatch);
    }

    bool allowDiskUse() const {
        return _findCommand.getAllowDiskUse();
    }

    void setAllowDiskUse(bool allowDiskUse) {
        _findCommand.setAllowDiskUse(allowDiskUse);
    }

    bool isExplain() const {
        return _explain;
    }

    void setExplain(bool explain) {
        _explain = explain;
    }

    const BSONObj& getUnwrappedReadPref() const {
        return _findCommand.getUnwrappedReadPref();
    }

    void setUnwrappedReadPref(BSONObj unwrappedReadPref) {
        _findCommand.setUnwrappedReadPref(unwrappedReadPref.getOwned());
    }

    int getMaxTimeMS() const {
        return _findCommand.getMaxTimeMS() ? static_cast<int>(*_findCommand.getMaxTimeMS()) : 0;
    }

    void setMaxTimeMS(int maxTimeMS) {
        _findCommand.setMaxTimeMS(maxTimeMS);
    }

    const BSONObj& getMin() const {
        return _findCommand.getMin();
    }

    void setMin(BSONObj min) {
        _findCommand.setMin(min.getOwned());
    }

    const BSONObj& getMax() const {
        return _findCommand.getMax();
    }

    void setMax(BSONObj max) {
        _findCommand.setMax(max.getOwned());
    }

    bool returnKey() const {
        return _findCommand.getReturnKey();
    }

    void setReturnKey(bool returnKey) {
        _findCommand.setReturnKey(returnKey);
    }

    bool showRecordId() const {
        return _findCommand.getShowRecordId();
    }

    void setShowRecordId(bool showRecordId) {
        _findCommand.setShowRecordId(showRecordId);
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
        if (_tailableMode == TailableModeEnum::kTailableAndAwaitData) {
            _findCommand.setAwaitData(true);
            _findCommand.setTailable(true);
        } else if (_tailableMode == TailableModeEnum::kTailable) {
            _findCommand.setTailable(true);
        }
    }

    TailableModeEnum getTailableMode() const {
        return _tailableMode;
    }

    void setLegacyRuntimeConstants(LegacyRuntimeConstants runtimeConstants) {
        _findCommand.setLegacyRuntimeConstants(std::move(runtimeConstants));
    }

    const boost::optional<LegacyRuntimeConstants>& getLegacyRuntimeConstants() const {
        return _findCommand.getLegacyRuntimeConstants();
    }

    bool getTailable() const {
        return _findCommand.getTailable();
    }

    bool getAwaitData() const {
        return _findCommand.getAwaitData();
    }

    void setLetParameters(BSONObj letParams) {
        _findCommand.setLet(std::move(letParams));
    }

    const boost::optional<BSONObj>& getLetParameters() const {
        return _findCommand.getLet();
    }

    bool isSlaveOk() const {
        return _slaveOk;
    }

    void setSlaveOk(bool slaveOk) {
        _slaveOk = slaveOk;
    }

    bool isNoCursorTimeout() const {
        return _findCommand.getNoCursorTimeout();
    }

    void setNoCursorTimeout(bool noCursorTimeout) {
        _findCommand.setNoCursorTimeout(noCursorTimeout);
    }

    bool isExhaust() const {
        return _exhaust;
    }

    void setExhaust(bool exhaust) {
        _exhaust = exhaust;
    }

    bool isAllowPartialResults() const {
        return _findCommand.getAllowPartialResults();
    }

    void setAllowPartialResults(bool allowPartialResults) {
        _findCommand.setAllowPartialResults(allowPartialResults);
    }

    boost::optional<std::int64_t> getReplicationTerm() const {
        return _findCommand.getTerm();
    }

    void setReplicationTerm(boost::optional<std::int64_t> replicationTerm) {
        _findCommand.setTerm(replicationTerm);
    }

    bool isReadOnce() const {
        return _findCommand.getReadOnce();
    }

    void setReadOnce(bool readOnce) {
        _findCommand.setReadOnce(readOnce);
    }

    void setAllowSpeculativeMajorityRead(bool allowSpeculativeMajorityRead) {
        _findCommand.setAllowSpeculativeMajorityRead(allowSpeculativeMajorityRead);
    }

    bool allowSpeculativeMajorityRead() const {
        return _findCommand.getAllowSpeculativeMajorityRead();
    }

    bool getRequestResumeToken() const {
        return _findCommand.getRequestResumeToken();
    }

    void setRequestResumeToken(bool requestResumeToken) {
        _findCommand.setRequestResumeToken(requestResumeToken);
    }

    const BSONObj& getResumeAfter() const {
        return _findCommand.getResumeAfter();
    }

    void setResumeAfter(BSONObj resumeAfter) {
        _findCommand.setResumeAfter(resumeAfter.getOwned());
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

    // TODO SERVER-53060: This additional nesting can be avoided if we move the below fields
    // (_explain, _tailableMode, etc.) into the CanonicalQuery class.
    FindCommand _findCommand;

    bool _explain = false;

    // Options that can be specified in the OP_QUERY 'flags' header.
    TailableModeEnum _tailableMode = TailableModeEnum::kNormal;
    bool _slaveOk = false;
    bool _exhaust = false;

    // Parameters used only by the legacy query request.
    bool _hasReadPref = false;
};

}  // namespace mongo
