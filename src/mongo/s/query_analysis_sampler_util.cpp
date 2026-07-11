// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/s/query_analysis_sampler_util.h"

#include "mongo/idl/idl_parser.h"
#include "mongo/platform/random.h"
#include "mongo/util/static_immortal.h"
#include "mongo/util/synchronized_value.h"

#include <iterator>
#include <new>
#include <string_view>
#include <utility>

#include <boost/move/utility_core.hpp>
#include <boost/none.hpp>
#include <boost/optional/optional.hpp>

namespace mongo {
namespace analyze_shard_key {

namespace {
using namespace std::literals::string_view_literals;

constexpr auto kSampleIdFieldName = "sampleId"sv;

template <typename C>
auto sampleIter(C&& c) {
    static StaticImmortal<synchronized_value<PseudoRandom>> random{
        PseudoRandom{SecureRandom().nextInt64()}};
    return std::next(c.begin(), (*random)->nextInt64(c.size()));
}

std::string_view adjustCmdNameCase(std::string_view cmdName) {
    if (cmdName == "findandmodify") {
        return std::string_view("findAndModify");
    } else {
        return cmdName;
    }
}

}  // namespace

boost::optional<UUID> tryGenerateSampleId(OperationContext* opCtx,
                                          const NamespaceString& nss,
                                          const SampledCommandNameEnum cmdName) {
    return supportsSamplingQueries(opCtx)
        ? QueryAnalysisSampler::get(opCtx).tryGenerateSampleId(opCtx, nss, cmdName)
        : boost::none;
}

boost::optional<UUID> tryGenerateSampleId(OperationContext* opCtx,
                                          const NamespaceString& nss,
                                          std::string_view cmdName) {
    return tryGenerateSampleId(
        opCtx,
        nss,
        idl::deserialize<SampledCommandNameEnum>(adjustCmdNameCase(cmdName),
                                                 IDLParserContext("tryGenerateSampleId")));
}

boost::optional<TargetedSampleId> tryGenerateTargetedSampleId(OperationContext* opCtx,
                                                              const NamespaceString& nss,
                                                              const SampledCommandNameEnum cmdName,
                                                              const std::set<ShardId>& shardIds) {
    if (auto sampleId = tryGenerateSampleId(opCtx, nss, cmdName)) {
        return TargetedSampleId{*sampleId, getRandomShardId(shardIds)};
    }
    return boost::none;
}

boost::optional<TargetedSampleId> tryGenerateTargetedSampleId(OperationContext* opCtx,
                                                              const NamespaceString& nss,
                                                              std::string_view cmdName,
                                                              const std::set<ShardId>& shardIds) {
    return tryGenerateTargetedSampleId(
        opCtx,
        nss,
        idl::deserialize<SampledCommandNameEnum>(adjustCmdNameCase(cmdName),
                                                 IDLParserContext("tryGenerateTargetedSampleId")),
        shardIds);
}

boost::optional<TargetedSampleId> tryGenerateTargetedSampleId(
    OperationContext* opCtx,
    const NamespaceString& nss,
    const SampledCommandNameEnum cmdName,
    const std::vector<ShardEndpoint>& endpoints) {
    if (auto sampleId = tryGenerateSampleId(opCtx, nss, cmdName)) {
        return TargetedSampleId{*sampleId, getRandomShardId(endpoints)};
    }
    return boost::none;
}
boost::optional<TargetedSampleId> tryGenerateTargetedSampleId(
    OperationContext* opCtx,
    const NamespaceString& nss,
    const BatchedCommandRequest::BatchType batchType,
    const std::vector<ShardEndpoint>& endpoints) {
    auto cmdName = [&] {
        switch (batchType) {
            case BatchedCommandRequest::BatchType::BatchType_Delete:
                return SampledCommandNameEnum::kDelete;
            case BatchedCommandRequest::BatchType::BatchType_Insert:
                return SampledCommandNameEnum::kInsert;
            case BatchedCommandRequest::BatchType::BatchType_Update:
                return SampledCommandNameEnum::kUpdate;
        }
        MONGO_UNREACHABLE;
    }();
    return tryGenerateTargetedSampleId(opCtx, nss, cmdName, endpoints);
}

ShardId getRandomShardId(const std::set<ShardId>& shardIds) {
    return *sampleIter(shardIds);
}

ShardId getRandomShardId(const std::vector<ShardEndpoint>& endpoints) {
    return sampleIter(endpoints)->shardName;
}

BSONObj appendSampleId(const BSONObj& cmdObj, const UUID& sampleId) {
    BSONObjBuilder bob(std::move(cmdObj));
    appendSampleId(&bob, sampleId);
    return bob.obj();
}

void appendSampleId(BSONObjBuilder* bob, const UUID& sampleId) {
    sampleId.appendToBuilder(bob, kSampleIdFieldName);
}

}  // namespace analyze_shard_key
}  // namespace mongo
