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

#include "mongo/db/query/query_shape/agg_cmd_shape.h"

#include "mongo/db/query/query_shape/shape_helpers.h"

namespace mongo::query_shape {

AggCmdShapeComponents::AggCmdShapeComponents(
    const AggregateCommandRequest& aggRequest,
    stdx::unordered_set<NamespaceString> involvedNamespaces_,
    std::vector<BSONObj> pipeline)
    : request(aggRequest),
      involvedNamespaces(std::move(involvedNamespaces_)),
      pipelineShape(std::move(pipeline)) {}

void AggCmdShapeComponents::HashValue(absl::HashState state) const {
    if (request.getAllowDiskUse()) {
        state = absl::HashState::combine(std::move(state), bool(request.getAllowDiskUse()));
    }
    if (request.getExplain()) {
        state = absl::HashState::combine(std::move(state), *request.getExplain());
    }
    for (auto&& shapifiedStage : pipelineShape) {
        state = absl::HashState::combine(std::move(state), simpleHash(shapifiedStage));
    }
}

void AggCmdShape::appendLetCmdSpecificShapeComponents(
    BSONObjBuilder& bob,
    const boost::intrusive_ptr<ExpressionContext>& expCtx,
    const SerializationOptions& opts) const {
    // TODO SERVER-76330 use representative shape.
    if (opts == SerializationOptions::kDebugQueryShapeSerializeOptions) {
        // We have this copy stored already!
        return _components.appendTo(bob);
    } else {
        // The cached pipeline shape doesn't match the requested options, so we have to
        // re-parse the pipeline from the initial request.
        expCtx->inMongos = _inMongos;
        expCtx->addResolvedNamespaces(_components.involvedNamespaces);
        auto reparsed = Pipeline::parse(_components.request.getPipeline(), expCtx);
        auto serializedPipeline = reparsed->serializeToBson(opts);
        AggCmdShapeComponents{
            _components.request, _components.involvedNamespaces, serializedPipeline}
            .appendTo(bob);
    }
}

void AggCmdShapeComponents::appendTo(BSONObjBuilder& bob) const {
    bob.append("command", "aggregate");

    // pipeline
    bob.append(AggregateCommandRequest::kPipelineFieldName, pipelineShape);

    // explain
    if (request.getExplain().has_value()) {
        bob.append(AggregateCommandRequest::kExplainFieldName, true);
    }

    // allowDiskUse
    if (auto param = request.getAllowDiskUse(); param.has_value()) {
        bob.append(AggregateCommandRequest::kAllowDiskUseFieldName, param.value_or(false));
    }
}

namespace {
int64_t sum(const std::initializer_list<int64_t>& sizes) {
    return std::accumulate(sizes.begin(), sizes.end(), 0, std::plus{});
}

int64_t _size(const std::vector<BSONObj>& objects) {
    return std::accumulate(objects.begin(), objects.end(), 0, [](int64_t total, const auto& obj) {
        // Include the 'sizeof' to account for the variable number in the vector.
        return total + sizeof(BSONObj) + obj.objsize();
    });
}

int64_t _size(const PassthroughToShardOptions& passthroughToShardOpts) {
    return passthroughToShardOpts.getShard().size();
}

int64_t _size(const ExchangeSpec& exchange) {
    return sum(
        {exchange.getKey().objsize(),
         (exchange.getBoundaries() ? _size(exchange.getBoundaries().get()) : 0),
         (exchange.getConsumerIds() ? 4 * static_cast<int64_t>(exchange.getConsumerIds()->size())
                                    : 0)});
}

int64_t _size(const EncryptionInformation& encryptInfo) {
    tasserted(7659700,
              "Unexpected encryption information - not expecting to collect query shape stats on "
              "encrypted querys");
}

int64_t singleDataSourceSize(int64_t runningTotal, const ExternalDataSourceInfo& source) {
    // Here we include the 'sizeof' since its expected to be contained in a vector, which will
    // have a variable number of these.
    return runningTotal + sizeof(ExternalDataSourceInfo) + source.getUrl().size();
}

int64_t _size(const std::vector<ExternalDataSourceOption>& externalDataSources) {
    // External data sources aren't currently expected to be used much in production Atlas
    // clusters, so it's probably pretty unlikely that this code will ever be exercised. That
    // said, there's not reason it shouldn't work and be tracked correctly.
    return std::accumulate(
        externalDataSources.begin(),
        externalDataSources.end(),
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

int64_t _size(const DatabaseName& dbName) {
    return dbName.size();
}

int64_t _size(const BSONObj& obj) {
    return obj.objsize();
}

template <typename T>
int64_t _size(const boost::optional<T>& optionalT) {
    return optionalT ? _size(*optionalT) : 0;
}

// variadic base case.
template <typename T>
int64_t sumOfSizes(const T& t) {
    return _size(t);
}

// variadic recursive case. Making the compiler expand the pluses everywhere to give us good
// formatting at the call site. sumOfSizes(x, y, z) rather than _size(x) + _size(y) + _size(z).
template <typename T, typename... Args>
int64_t sumOfSizes(const T& t, const Args&... args) {
    return _size(t) + sumOfSizes(args...);
}

int64_t aggRequestSize(const AggregateCommandRequest& request) {
    return sumOfSizes(request.getPipeline(),
                      request.getUnwrappedReadPref(),
                      request.getExchange(),
                      request.getPassthroughToShard(),
                      request.getEncryptionInformation(),
                      request.getExternalDataSources(),
                      request.getDbName());
}

}  // namespace

int64_t AggCmdShapeComponents::size() const {
    return sum({sizeof(*this), _size(pipelineShape), aggRequestSize(request)});
}

AggCmdShape::AggCmdShape(const AggregateCommandRequest& aggregateCommand,
                         NamespaceString origNss,
                         stdx::unordered_set<NamespaceString> involvedNamespaces_,
                         const Pipeline& pipeline,
                         const boost::intrusive_ptr<ExpressionContext>& expCtx)
    : CmdWithLetShape(aggregateCommand.getLet(),
                      expCtx,
                      _components,
                      std::move(origNss),
                      aggregateCommand.getCollation().value_or(BSONObj())),
      // TODO SERVER-76330 use representative shape.
      _components(aggregateCommand,
                  std::move(involvedNamespaces_),
                  pipeline.serializeToBson(SerializationOptions::kDebugQueryShapeSerializeOptions)),
      _inMongos(expCtx->inMongos) {}

}  // namespace mongo::query_shape
