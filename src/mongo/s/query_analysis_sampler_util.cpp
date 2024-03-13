/**
 *    Copyright (C) 2022-present MongoDB, Inc.
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

#include "mongo/s/query_analysis_sampler_util.h"

#include <iterator>
#include <new>
#include <utility>

#include <boost/move/utility_core.hpp>
#include <boost/none.hpp>
#include <boost/optional/optional.hpp>

#include "mongo/idl/idl_parser.h"
#include "mongo/platform/random.h"
#include "mongo/util/static_immortal.h"
#include "mongo/util/synchronized_value.h"

namespace mongo {
namespace analyze_shard_key {

namespace {

constexpr auto kSampleIdFieldName = "sampleId"_sd;

template <typename C>
auto sampleIter(C&& c) {
    static StaticImmortal<synchronized_value<PseudoRandom>> random{
        PseudoRandom{SecureRandom().nextInt64()}};
    return std::next(c.begin(), (*random)->nextInt64(c.size()));
}

StringData adjustCmdNameCase(StringData cmdName) {
    if (cmdName == "findandmodify") {
        return StringData("findAndModify");
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
                                          StringData cmdName) {
    return tryGenerateSampleId(opCtx,
                               nss,
                               SampledCommandName_parse(IDLParserContext("tryGenerateSampleId"),
                                                        adjustCmdNameCase(cmdName)));
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
                                                              StringData cmdName,
                                                              const std::set<ShardId>& shardIds) {
    return tryGenerateTargetedSampleId(
        opCtx,
        nss,
        SampledCommandName_parse(IDLParserContext("tryGenerateTargetedSampleId"),
                                 adjustCmdNameCase(cmdName)),
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
