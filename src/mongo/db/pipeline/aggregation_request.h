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
#include <vector>

#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/pipeline/exchange_spec_gen.h"
#include "mongo/db/pipeline/runtime_constants_gen.h"
#include "mongo/db/query/explain_options.h"
#include "mongo/db/write_concern_options.h"

namespace mongo {

template <typename T>
class StatusWith;
class Document;

/**
 * Represents the user-supplied options to the aggregate command.
 */
class AggregationRequest {
public:
    static constexpr StringData kCommandName = "aggregate"_sd;
    static constexpr StringData kCursorName = "cursor"_sd;
    static constexpr StringData kBatchSizeName = "batchSize"_sd;
    static constexpr StringData kFromMongosName = "fromMongos"_sd;
    static constexpr StringData kNeedsMergeName = "needsMerge"_sd;
    static constexpr StringData kPipelineName = "pipeline"_sd;
    static constexpr StringData kCollationName = "collation"_sd;
    static constexpr StringData kExplainName = "explain"_sd;
    static constexpr StringData kAllowDiskUseName = "allowDiskUse"_sd;
    static constexpr StringData kHintName = "hint"_sd;
    static constexpr StringData kExchangeName = "exchange"_sd;
    static constexpr StringData kRuntimeConstants = "runtimeConstants"_sd;

    static constexpr long long kDefaultBatchSize = 101;

    /**
     * Parse an aggregation pipeline definition from 'pipelineElem'. Returns a non-OK status if
     * pipeline is not an array or if any of the array elements are not objects.
     */
    static StatusWith<std::vector<BSONObj>> parsePipelineFromBSON(BSONElement pipelineElem) {
        std::vector<BSONObj> pipeline;
        if (pipelineElem.eoo() || pipelineElem.type() != BSONType::Array) {
            return {ErrorCodes::TypeMismatch, "'pipeline' option must be specified as an array"};
        }

        for (auto elem : pipelineElem.Obj()) {
            if (elem.type() != BSONType::Object) {
                return {ErrorCodes::TypeMismatch,
                        "Each element of the 'pipeline' array must be an object"};
            }
            pipeline.push_back(elem.embeddedObject().getOwned());
        }

        return std::move(pipeline);
    }


    /**
     * Create a new instance of AggregationRequest by parsing the raw command object. Returns a
     * non-OK status if a required field was missing, if there was an unrecognized field name or if
     * there was a bad value for one of the fields.
     *
     * If we are parsing a request for an explained aggregation with an explain verbosity provided,
     * then 'explainVerbosity' contains this information. In this case, 'cmdObj' may not itself
     * contain the explain specifier. Otherwise, 'explainVerbosity' should be boost::none.
     */
    static StatusWith<AggregationRequest> parseFromBSON(
        NamespaceString nss,
        const BSONObj& cmdObj,
        boost::optional<ExplainOptions::Verbosity> explainVerbosity = boost::none);

    /**
     * Convenience overload which constructs the request's NamespaceString from the given database
     * name and command object.
     */
    static StatusWith<AggregationRequest> parseFromBSON(
        const std::string& dbName,
        const BSONObj& cmdObj,
        boost::optional<ExplainOptions::Verbosity> explainVerbosity = boost::none);

    /*
     * The first field in 'cmdObj' must be a string representing a valid collection name, or the
     * number 1. In the latter case, returns a reserved namespace that does not represent a user
     * collection. See 'NamespaceString::makeCollectionlessAggregateNSS()'.
     */
    static NamespaceString parseNs(const std::string& dbname, const BSONObj& cmdObj);

    /**
     * Constructs an AggregationRequest over the given namespace with the given pipeline. All
     * options aside from the pipeline assume their default values.
     */
    AggregationRequest(NamespaceString nss, std::vector<BSONObj> pipeline)
        : _nss(std::move(nss)), _pipeline(std::move(pipeline)), _batchSize(kDefaultBatchSize) {}

    /**
     * Serializes the options to a Document. Note that this serialization includes the original
     * pipeline object, as specified. Callers will likely want to override this field with a
     * serialization of a parsed and optimized Pipeline object.
     *
     * The explain option is not serialized. Since the explain command format is {explain:
     * {aggregate: ...}, ...}, explain options are not part of the aggregate command object.
     */
    Document serializeToCommandObj() const;

    //
    // Getters.
    //

    long long getBatchSize() const {
        return _batchSize;
    }

    const NamespaceString& getNamespaceString() const {
        return _nss;
    }

    /**
     * An unparsed version of the pipeline. All BSONObjs are owned.
     */
    const std::vector<BSONObj>& getPipeline() const {
        return _pipeline;
    }

    /**
     * Returns true if this request originated from a mongoS.
     */
    bool isFromMongos() const {
        return _fromMongos;
    }

    /**
     * Returns true if this request represents the shards part of a split pipeline, and should
     * produce mergeable output.
     */
    bool needsMerge() const {
        return _needsMerge;
    }

    bool shouldAllowDiskUse() const {
        return _allowDiskUse;
    }

    bool shouldBypassDocumentValidation() const {
        return _bypassDocumentValidation;
    }

    /**
     * Returns an empty object if no collation was specified.
     */
    BSONObj getCollation() const {
        return _collation;
    }

    BSONObj getHint() const {
        return _hint;
    }

    boost::optional<ExplainOptions::Verbosity> getExplain() const {
        return _explainMode;
    }

    unsigned int getMaxTimeMS() const {
        return _maxTimeMS;
    }

    const BSONObj& getReadConcern() const {
        return _readConcern;
    }

    const BSONObj& getUnwrappedReadPref() const {
        return _unwrappedReadPref;
    }

    const auto& getExchangeSpec() const {
        return _exchangeSpec;
    }

    boost::optional<WriteConcernOptions> getWriteConcern() const {
        return _writeConcern;
    }

    const auto& getRuntimeConstants() const {
        return _runtimeConstants;
    }

    //
    // Setters for optional fields.
    //

    /**
     * Negative batchSize is illegal but batchSize of 0 is allowed.
     */
    void setBatchSize(long long batchSize) {
        uassert(40203, "batchSize must be non-negative", batchSize >= 0);
        _batchSize = batchSize;
    }

    void setCollation(BSONObj collation) {
        _collation = collation.getOwned();
    }

    void setHint(BSONObj hint) {
        _hint = hint.getOwned();
    }

    void setExplain(boost::optional<ExplainOptions::Verbosity> verbosity) {
        _explainMode = verbosity;
    }

    void setAllowDiskUse(bool allowDiskUse) {
        _allowDiskUse = allowDiskUse;
    }

    void setFromMongos(bool isFromMongos) {
        _fromMongos = isFromMongos;
    }

    void setNeedsMerge(bool needsMerge) {
        _needsMerge = needsMerge;
    }

    void setBypassDocumentValidation(bool shouldBypassDocumentValidation) {
        _bypassDocumentValidation = shouldBypassDocumentValidation;
    }

    void setMaxTimeMS(unsigned int maxTimeMS) {
        _maxTimeMS = maxTimeMS;
    }

    void setReadConcern(BSONObj readConcern) {
        _readConcern = readConcern.getOwned();
    }

    void setUnwrappedReadPref(BSONObj unwrappedReadPref) {
        _unwrappedReadPref = unwrappedReadPref.getOwned();
    }

    void setExchangeSpec(ExchangeSpec spec) {
        _exchangeSpec = std::move(spec);
    }

    void setWriteConcern(WriteConcernOptions writeConcern) {
        _writeConcern = writeConcern;
    }

    void setRuntimeConstants(RuntimeConstants runtimeConstants) {
        _runtimeConstants = std::move(runtimeConstants);
    }

private:
    // Required fields.
    const NamespaceString _nss;

    // An unparsed version of the pipeline.
    const std::vector<BSONObj> _pipeline;

    long long _batchSize;

    // Optional fields.

    // An owned copy of the user-specified collation object, or an empty object if no collation was
    // specified.
    BSONObj _collation;

    // The hint provided, if any.  If the hint was by index key pattern, the value of '_hint' is
    // the key pattern hinted.  If the hint was by index name, the value of '_hint' is
    // {$hint: <String>}, where <String> is the index name hinted.
    BSONObj _hint;

    BSONObj _readConcern;

    // The unwrapped readPreference object, if one was given to us by the mongos command processor.
    // This object will be empty when no readPreference is specified or if the request does not
    // originate from mongos.
    BSONObj _unwrappedReadPref;

    // The explain mode to use, or boost::none if this is not a request for an aggregation explain.
    boost::optional<ExplainOptions::Verbosity> _explainMode;

    bool _allowDiskUse = false;
    bool _fromMongos = false;
    bool _needsMerge = false;
    bool _bypassDocumentValidation = false;

    // A user-specified maxTimeMS limit, or a value of '0' if not specified.
    unsigned int _maxTimeMS = 0;

    // An optional exchange specification for this request. If set it means that the request
    // represents a producer running as a part of the exchange machinery.
    // This is an internal option; we do not expect it to be set on requests from users or drivers.
    boost::optional<ExchangeSpec> _exchangeSpec;

    // The explicit writeConcern for the operation or boost::none if the user did not specifiy one.
    boost::optional<WriteConcernOptions> _writeConcern;

    // A document containing runtime constants; i.e. values that do not change once computed (e.g.
    // $$NOW).
    boost::optional<RuntimeConstants> _runtimeConstants;
};
}  // namespace mongo
