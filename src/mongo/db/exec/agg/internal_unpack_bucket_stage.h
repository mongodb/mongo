// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/db/exec/agg/stage.h"
#include "mongo/db/exec/timeseries/bucket_unpacker.h"
#include "mongo/db/pipeline/document_source_internal_unpack_bucket.h"
#include "mongo/util/modules.h"

#include <string_view>

#include <boost/smart_ptr/intrusive_ptr.hpp>

namespace mongo {
namespace exec {
namespace agg {

class InternalUnpackBucketStage final : public Stage {
public:
    InternalUnpackBucketStage(std::string_view stageName,
                              const boost::intrusive_ptr<ExpressionContext>& pExpCtx,
                              const std::shared_ptr<InternalUnpackBucketSharedState>& sharedState,
                              DepsTracker _eventFilterDeps,
                              bool _unpackToBson,
                              boost::optional<long long> _sampleSize);

    ~InternalUnpackBucketStage() override {}

private:
    boost::optional<Document> getNextMatchingMeasure();

    GetNextResult doGetNext() final;

    const DepsTracker _eventFilterDeps;
    const std::shared_ptr<InternalUnpackBucketSharedState> _sharedState;
    const bool _unpackToBson;
    const boost::optional<long long> _sampleSize;
};

}  // namespace agg
}  // namespace exec
}  // namespace mongo
