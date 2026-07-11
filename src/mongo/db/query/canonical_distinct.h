// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/db/pipeline/group_from_first_document_transformation.h"
#include "mongo/db/query/compiler/logical_model/sort_pattern/sort_pattern.h"
#include "mongo/util/modules.h"
#include "mongo/util/uuid.h"

#include <string>
#include <utility>

#include <boost/none.hpp>
#include <boost/optional/optional.hpp>

namespace mongo {

class BSONObj;
class ExtensionsCallback;
class OperationContext;

/**
 * The canonical form of the distinct query.
 */
class CanonicalDistinct {
public:
    static const char kKeyField[];
    static const char kQueryField[];
    static const char kCollationField[];
    static const char kCommentField[];
    static const char kUnwoundArrayFieldForViewUnwind[];
    static const char kHintField[];

    CanonicalDistinct(std::string key,
                      bool mirrored = false,
                      boost::optional<UUID> sampleId = boost::none,
                      boost::optional<BSONObj> projSpec = boost::none,
                      bool flipDistinctScanDirection = false)
        : _key(std::move(key)),
          _mirrored(mirrored),
          _sampleId(std::move(sampleId)),
          _projSpec(std::move(projSpec)),
          _flipDistinctScanDirection(flipDistinctScanDirection) {}

    CanonicalDistinct(const CanonicalDistinct& other)
        : _key(other.getKey()),
          _mirrored(other.isMirrored()),
          _sampleId(other.getSampleId()),
          _projSpec(other.getProjectionSpec()),
          _flipDistinctScanDirection(other.isDistinctScanDirectionFlipped()) {
        setSortRequirement(other.getSortRequirement());
    }

    CanonicalDistinct& operator=(const CanonicalDistinct&) = delete;

    const std::string& getKey() const {
        return _key;
    }

    boost::optional<UUID> getSampleId() const {
        return _sampleId;
    }

    bool isMirrored() const {
        return _mirrored;
    }

    const boost::optional<BSONObj>& getProjectionSpec() const {
        return _projSpec;
    }

    void setProjectionSpec(BSONObj projSpec) {
        _projSpec = std::move(projSpec);
    }

    bool isDistinctScanDirectionFlipped() const {
        return _flipDistinctScanDirection;
    }

    void setRewrittenGroupStage(
        std::unique_ptr<GroupFromFirstDocumentTransformation> rewrittenGroupStage) {
        _rewrittenGroupStage = std::move(rewrittenGroupStage);
    }

    std::unique_ptr<GroupFromFirstDocumentTransformation> releaseRewrittenGroupStage() {
        return std::move(_rewrittenGroupStage);
    }

    const boost::optional<SortPattern>& getSortRequirement() const {
        return _sortRequirement;
    }

    const BSONObj& getSerializedSortRequirement() const {
        return _serializedSortRequirement;
    }

    void setSortRequirement(boost::optional<SortPattern> sortRequirement) {
        _sortRequirement = std::move(sortRequirement);
        _serializedSortRequirement = _sortRequirement
            ? _sortRequirement
                  ->serialize(SortPattern::SortKeySerialization::kForPipelineSerialization)
                  .toBson()
            : BSONObj();
    }

private:
    // The field for which we are getting distinct values.
    const std::string _key;

    // Indicates that this was a mirrored operation.
    bool _mirrored = false;

    // The unique sample id for this operation if it has been chosen for sampling.
    boost::optional<UUID> _sampleId;

    // This is used when we have a covered distinct scan in order to materialize the output.
    boost::optional<BSONObj> _projSpec;

    // In certain situations we will need to flip the direction of any generated DISTINCT_SCAN to
    // preserve the semantics of the query.
    // TODO SERVER-94369: Remove this and rely entirely on '_sortRequirement'.
    bool _flipDistinctScanDirection{false};

    // When the aggreation rewriting is successful and the multiplanning generates a DISTINCT_SCAN,
    // we need to do the last modifications to the pipeline to be compatible with the DISTINCT_SCAN.
    std::unique_ptr<GroupFromFirstDocumentTransformation> _rewrittenGroupStage;

    // For some queries (e.g. $group with $top/$sortBy), a sort is needed to implement correct
    // semantics with a DISTINCT_SCAN.
    boost::optional<SortPattern> _sortRequirement;
    BSONObj _serializedSortRequirement;
};

}  // namespace mongo
