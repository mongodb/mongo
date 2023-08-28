/**
 *    Copyright (C) 2023-present MongoDB, Inc.
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

#include "mongo/db/query/query_stats/aggregate_key_generator.h"

#include <absl/container/node_hash_set.h>
#include <boost/cstdint.hpp>
#include <functional>
#include <initializer_list>
#include <memory>
#include <numeric>
#include <vector>

#include <boost/move/utility_core.hpp>
#include <boost/optional/optional.hpp>
#include <boost/smart_ptr/intrusive_ptr.hpp>

#include "mongo/crypto/fle_field_schema_gen.h"
#include "mongo/db/basic_types_gen.h"
#include "mongo/db/database_name.h"
#include "mongo/db/pipeline/exchange_spec_gen.h"
#include "mongo/db/pipeline/external_data_source_option_gen.h"
#include "mongo/db/pipeline/pipeline.h"
#include "mongo/db/query/query_shape.h"
#include "mongo/db/query/serialization_options.h"
#include "mongo/db/query/shape_helpers.h"
#include "mongo/util/assert_util.h"

namespace mongo::query_stats {

BSONObj AggregateKeyGenerator::generate(
    OperationContext* opCtx,
    boost::optional<SerializationOptions::TokenizeIdentifierFunc> hmacPolicy) const {
    // TODO SERVER-76087 We will likely want to set a flag here to stop $search from calling out
    // to mongot.
    auto expCtx = makeDummyExpCtx(opCtx);
    SerializationOptions opts = hmacPolicy
        ? SerializationOptions{LiteralSerializationPolicy::kToDebugTypeString,
                               /*transformIdentifiersBool*/ true,
                               *hmacPolicy,
                               /*includePath*/ true,
                               /*verbosity*/ boost::none,
                               /*inMatchExprSortAndDedupElements*/ false}
        : SerializationOptions{LiteralSerializationPolicy::kToDebugTypeString};

    return makeQueryStatsKey(opts, expCtx);
}

void AggregateKeyGenerator::appendCommandSpecificComponents(
    BSONObjBuilder& bob, const SerializationOptions& opts) const {
    // cursor
    if (auto param = _request.getCursor().getBatchSize()) {
        BSONObjBuilder cursorInfo = bob.subobjStart(AggregateCommandRequest::kCursorFieldName);
        opts.appendLiteral(&cursorInfo,
                           SimpleCursorOptions::kBatchSizeFieldName,
                           static_cast<long long>(param.get()));
        cursorInfo.doneFast();
    }

    // maxTimeMS
    if (auto param = _request.getMaxTimeMS()) {
        opts.appendLiteral(&bob,
                           AggregateCommandRequest::kMaxTimeMSFieldName,
                           static_cast<long long>(param.get()));
    }

    // bypassDocumentValidation
    if (auto param = _request.getBypassDocumentValidation()) {
        opts.appendLiteral(
            &bob, AggregateCommandRequest::kBypassDocumentValidationFieldName, bool(param.get()));
    }

    // otherNss
    if (!_involvedNamespaces.empty()) {
        BSONArrayBuilder otherNss = bob.subarrayStart(kOtherNssFieldName);
        for (const auto& nss : _involvedNamespaces) {
            BSONObjBuilder otherNsEntryBob = otherNss.subobjStart();
            shape_helpers::appendNamespaceShape(otherNsEntryBob, nss, opts);
            otherNsEntryBob.doneFast();
        }
        otherNss.doneFast();
    }
}

BSONObj AggregateKeyGenerator::makeQueryStatsKey(
    const SerializationOptions& opts, const boost::intrusive_ptr<ExpressionContext>& expCtx) const {
    auto pipeline = Pipeline::parse(_request.getPipeline(), expCtx);
    expCtx->setUserRoles();
    return _makeQueryStatsKeyHelper(opts, expCtx, *pipeline);
}

BSONObj AggregateKeyGenerator::_makeQueryStatsKeyHelper(
    const SerializationOptions& opts,
    const boost::intrusive_ptr<ExpressionContext>& expCtx,
    const Pipeline& pipeline) const {
    return generateWithQueryShape(
        query_shape::extractQueryShape(_request, pipeline, opts, expCtx, _origNss), opts);
}

namespace {

int64_t sum(const std::initializer_list<int64_t>& sizes) {
    return std::accumulate(sizes.begin(), sizes.end(), 0, std::plus{});
}

int64_t size(const std::vector<BSONObj>& objects) {
    return std::accumulate(objects.begin(), objects.end(), 0, [](int64_t total, const auto& obj) {
        // Include the 'sizeof' to account for the variable number in the vector.
        return total + sizeof(BSONObj) + obj.objsize();
    });
}

int64_t size(const boost::optional<PassthroughToShardOptions>& passthroughToShardOpts) {
    if (!passthroughToShardOpts) {
        return 0;
    }
    return passthroughToShardOpts->getShard().size();
}

int64_t size(const boost::optional<ExchangeSpec>& exchange) {
    if (!exchange) {
        return 0;
    }
    return sum(
        {exchange->getKey().objsize(),
         (exchange->getBoundaries() ? size(exchange->getBoundaries().get()) : 0),
         (exchange->getConsumerIds() ? 4 * static_cast<int64_t>(exchange->getConsumerIds()->size())
                                     : 0)});
}

int64_t size(const boost::optional<EncryptionInformation>& encryptInfo) {
    if (!encryptInfo) {
        return 0;
    }
    tasserted(7659700,
              "Unexpected encryption information - not expecting to collect query shape stats on "
              "encrypted querys");
}

int64_t singleDataSourceSize(int64_t runningTotal, const ExternalDataSourceInfo& source) {
    // Here we include the 'sizeof' since its expected to be contained in a vector, which will have
    // a variable number of these.
    return runningTotal + sizeof(ExternalDataSourceInfo) + source.getUrl().size();
}

int64_t size(const boost::optional<std::vector<ExternalDataSourceOption>>& externalDataSources) {
    if (!externalDataSources) {
        return 0;
    }
    // External data sources aren't currently expected to be used much in production Atlas clusters,
    // so it's probably pretty unlikely that this code will ever be exercised. That said, there's
    // not reason it shouldn't work and be tracked correctly.
    return std::accumulate(
        externalDataSources->begin(),
        externalDataSources->end(),
        0,
        [](int64_t runningTotal, const ExternalDataSourceOption& opt) {
            const auto& sources = opt.getDataSources();
            return sum({runningTotal,
                        // Include the 'sizeof' to account for the variable number in the vector.
                        sizeof(ExternalDataSourceOption),
                        static_cast<int64_t>(opt.getCollName().size()),
                        std::accumulate(sources.begin(), sources.end(), 0, singleDataSourceSize)});
        });
}

int64_t size(const boost::optional<DatabaseName>& dbName) {
    if (!dbName) {
        return 0;
    }
    return dbName->size();
}

int64_t size(const boost::optional<BSONObj>& obj) {
    return optionalObjSize(obj);
}

// variadic base case.
template <typename T>
int64_t sumOfSizes(const T& t) {
    return size(t);
}

// variadic recursive case. Making the compiler expand the pluses everywhere to give us good
// formatting at the call site. sumOfSizes(x, y, z) rather than size(x) + size(y) + size(z).
template <typename T, typename... Args>
int64_t sumOfSizes(const T& t, const Args&... args) {
    return size(t) + sumOfSizes(args...);
}

int64_t aggRequestSize(const AggregateCommandRequest& request) {
    return sumOfSizes(request.getPipeline(),
                      request.getLet(),
                      request.getUnwrappedReadPref(),
                      request.getExchange(),
                      request.getPassthroughToShard(),
                      request.getEncryptionInformation(),
                      request.getExternalDataSources(),
                      request.getDbName());
}

}  // namespace

int64_t AggregateKeyGenerator::doGetSize() const {
    return sum({sizeof(*this),
                static_cast<int64_t>(_origNss.size()),
                optionalObjSize(_initialQueryStatsKey),
                aggRequestSize(_request)});
}
}  // namespace mongo::query_stats
