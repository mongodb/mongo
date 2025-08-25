/**
 *    Copyright (C) 2019-present MongoDB, Inc.
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

#include "mongo/base/error_codes.h"
#include "mongo/base/status.h"
#include "mongo/base/string_data.h"
#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/oid.h"
#include "mongo/db/auth/action_set.h"
#include "mongo/db/auth/privilege.h"
#include "mongo/db/exec/document_value/document.h"
#include "mongo/db/exec/document_value/value.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/pipeline/document_source.h"
#include "mongo/db/pipeline/document_source_merge_gen.h"
#include "mongo/db/pipeline/document_source_merge_modes_gen.h"
#include "mongo/db/pipeline/document_source_writer.h"
#include "mongo/db/pipeline/expression.h"
#include "mongo/db/pipeline/expression_context.h"
#include "mongo/db/pipeline/field_path.h"
#include "mongo/db/pipeline/lite_parsed_document_source.h"
#include "mongo/db/pipeline/lite_parsed_pipeline.h"
#include "mongo/db/pipeline/merge_processor.h"
#include "mongo/db/pipeline/pipeline.h"
#include "mongo/db/pipeline/process_interface/mongo_process_interface.h"
#include "mongo/db/pipeline/stage_constraints.h"
#include "mongo/db/pipeline/variables.h"
#include "mongo/db/query/compiler/dependency_analysis/expression_dependencies.h"
#include "mongo/db/query/query_shape/serialization_options.h"
#include "mongo/db/query/write_ops/write_ops_parsers.h"
#include "mongo/db/read_concern_support_result.h"
#include "mongo/db/repl/read_concern_level.h"
#include "mongo/db/versioning_protocol/chunk_version.h"
#include "mongo/db/write_concern_options.h"
#include "mongo/s/write_ops/batched_command_request.h"
#include "mongo/stdx/unordered_map.h"
#include "mongo/util/intrusive_counter.h"

#include <functional>
#include <memory>
#include <set>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

#include <absl/container/node_hash_map.h>
#include <boost/move/utility_core.hpp>
#include <boost/none.hpp>
#include <boost/optional/optional.hpp>
#include <boost/smart_ptr/intrusive_ptr.hpp>
#include <fmt/format.h>

namespace mongo {

/**
 * A class for the $merge aggregation stage to handle all supported merge modes. Each instance of
 * this class must be initialized (via a constructor) with a 'MergeDescriptor', which defines a
 * a particular merge strategy for a pair of 'whenMatched' and 'whenNotMatched' merge  modes.
 */
class DocumentSourceMerge final : public DocumentSourceWriter {
public:
    static constexpr StringData kStageName = "$merge"_sd;
    static constexpr auto kDefaultWhenMatched = MergeStrategyDescriptor::WhenMatched::kMerge;
    static constexpr auto kDefaultWhenNotMatched = MergeStrategyDescriptor::WhenNotMatched::kInsert;

    /**
     * A "lite parsed" $merge stage to disallow passthrough from mongos even if the source
     * collection is unsharded. This ensures that the unique index verification happens once on
     * mongos and can be bypassed on the shards.
     */
    class LiteParsed final : public LiteParsedDocumentSourceNestedPipelines {
    public:
        LiteParsed(std::string parseTimeName,
                   NamespaceString foreignNss,
                   MergeWhenMatchedModeEnum whenMatched,
                   MergeWhenNotMatchedModeEnum whenNotMatched,
                   boost::optional<LiteParsedPipeline> onMatchedPipeline)
            : LiteParsedDocumentSourceNestedPipelines(
                  std::move(parseTimeName), std::move(foreignNss), std::move(onMatchedPipeline)),
              _whenMatched(whenMatched),
              _whenNotMatched(whenNotMatched) {}

        static std::unique_ptr<LiteParsed> parse(const NamespaceString& nss,
                                                 const BSONElement& spec,
                                                 const LiteParserOptions& options);

        ReadConcernSupportResult supportsReadConcern(repl::ReadConcernLevel level,
                                                     bool isImplicitDefault) const final {
            ReadConcernSupportResult result = {
                {level == repl::ReadConcernLevel::kLinearizableReadConcern,
                 {ErrorCodes::InvalidOptions,
                  fmt::format("{} cannot be used with a 'linearizable' read concern level",
                              kStageName)}},
                Status::OK()};
            auto pipelineReadConcern = LiteParsedDocumentSourceNestedPipelines::supportsReadConcern(
                level, isImplicitDefault);
            // Merge the result from the sub-pipeline into the $merge specific read concern result
            // to preserve the $merge errors over the internal pipeline errors.
            result.merge(pipelineReadConcern);
            return result;
        }

        PrivilegeVector requiredPrivileges(bool isMongos,
                                           bool bypassDocumentValidation) const final;

        /**
         * We must know the aggregation's collation when parsing a $merge in order to correctly
         * verify that the target namespace guarantees the uniqueness of the 'mergeOnFields'.
         */
        bool requiresCollationForParsingUnshardedAggregate() const final {
            return true;
        }

        bool isWriteStage() const override {
            return true;
        }

    private:
        MergeWhenMatchedModeEnum _whenMatched;
        MergeWhenNotMatchedModeEnum _whenNotMatched;
    };

    ~DocumentSourceMerge() override = default;

    const char* getSourceName() const final {
        return kStageName.data();
    }

    static const Id& id;

    Id getId() const override {
        return id;
    }

    MergeProcessor* getMergeProcessor() {
        return _mergeProcessor.get();
    }

    StageConstraints constraints(PipelineSplitState pipeState) const final;

    boost::optional<DistributedPlanLogic> distributedPlanLogic() final;

    Value serialize(const SerializationOptions& opts = SerializationOptions{}) const final;

    /**
     * Creates a new $merge stage from the given arguments.
     */
    static boost::intrusive_ptr<DocumentSource> create(
        NamespaceString outputNs,
        const boost::intrusive_ptr<ExpressionContext>& expCtx,
        MergeStrategyDescriptor::WhenMatched whenMatched,
        MergeStrategyDescriptor::WhenNotMatched whenNotMatched,
        boost::optional<BSONObj> letVariables,
        boost::optional<std::vector<BSONObj>> pipeline,
        std::set<FieldPath> mergeOnFields,
        boost::optional<ChunkVersion> collectionPlacementVersion,
        bool allowMergeOnNullishValues);

    /**
     * Parses a $merge stage from the user-supplied BSON.
     */
    static boost::intrusive_ptr<DocumentSource> createFromBson(
        BSONElement elem, const boost::intrusive_ptr<ExpressionContext>& pExpCtx);

    auto getPipeline() const {
        return _mergeProcessor->getPipeline();
    }

    void addVariableRefs(std::set<Variables::Id>* refs) const final {
        // Although $merge is not allowed in sub-pipelines and this method is used for correlation
        // analysis, the method is generic enough to be used in the future for other purposes.
        for (const auto& letVar : _mergeProcessor->getLetVariables()) {
            expression::addVariableRefs(letVar.expression.get(), refs);
        }
    }


private:
    friend boost::intrusive_ptr<exec::agg::Stage> documentSourceMergeToStageFn(
        const boost::intrusive_ptr<DocumentSource>& documentSource);

    /**
     * Builds a new $merge stage which will merge all documents into 'outputNs'. If
     * 'collectionPlacementVersion' is provided then processing will stop with an error if the
     * collection's epoch changes during the course of execution. This is used as a mechanism to
     * prevent the shard key from changing.
     */
    DocumentSourceMerge(NamespaceString outputNs,
                        const boost::intrusive_ptr<ExpressionContext>& expCtx,
                        MergeStrategyDescriptor::WhenMatched whenMatched,
                        MergeStrategyDescriptor::WhenNotMatched whenNotMatched,
                        boost::optional<BSONObj> letVariables,
                        boost::optional<std::vector<BSONObj>> pipeline,
                        std::set<FieldPath> mergeOnFields,
                        boost::optional<ChunkVersion> collectionPlacementVersion,
                        bool allowMergeOnNullishValues);


    // Holds the fields used for uniquely identifying documents. There must exist a unique index
    // with this key pattern. Default is "_id" for unsharded collections, and "_id" plus the shard
    // key for sharded collections.
    std::shared_ptr<std::set<FieldPath>> _mergeOnFields;

    // True if '_mergeOnFields' contains the _id. We store this as a separate boolean to avoid
    // repeated lookups into the set.
    bool _mergeOnFieldsIncludesId;

    std::shared_ptr<MergeProcessor> _mergeProcessor;
};

}  // namespace mongo
