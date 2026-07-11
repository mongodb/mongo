// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/base/error_codes.h"
#include "mongo/base/status.h"
#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/oid.h"
#include "mongo/db/auth/action_set.h"
#include "mongo/db/auth/action_type.h"
#include "mongo/db/auth/privilege.h"
#include "mongo/db/auth/resource_pattern.h"
#include "mongo/db/database_name.h"
#include "mongo/db/exec/document_value/document.h"
#include "mongo/db/exec/document_value/value.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/pipeline/document_source.h"
#include "mongo/db/pipeline/document_source_out_gen.h"
#include "mongo/db/pipeline/document_source_writer.h"
#include "mongo/db/pipeline/expression_context.h"
#include "mongo/db/pipeline/lite_parsed_document_source.h"
#include "mongo/db/pipeline/lite_parsed_document_source_nested_pipelines.h"
#include "mongo/db/pipeline/pipeline.h"
#include "mongo/db/pipeline/process_interface/mongo_process_interface.h"
#include "mongo/db/pipeline/stage_constraints.h"
#include "mongo/db/pipeline/variables.h"
#include "mongo/db/query/query_shape/serialization_options.h"
#include "mongo/db/query/write_ops/write_ops_gen.h"
#include "mongo/db/read_concern_support_result.h"
#include "mongo/db/repl/read_concern_level.h"
#include "mongo/db/timeseries/timeseries_gen.h"
#include "mongo/s/write_ops/batched_command_request.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/intrusive_counter.h"
#include "mongo/util/modules.h"

#include <list>
#include <memory>
#include <set>
#include <string>
#include <string_view>
#include <utility>

#include <boost/none.hpp>
#include <boost/optional/optional.hpp>
#include <boost/smart_ptr/intrusive_ptr.hpp>
#include <fmt/format.h>

namespace mongo {
using namespace std::literals::string_view_literals;

DECLARE_STAGE_PARAMS_DERIVED_DEFAULT(Out);

/**
 * Implementation for the $out aggregation stage.
 */
class DocumentSourceOut final : public DocumentSourceWriter {
public:
    static constexpr std::string_view kStageName = "$out"sv;

    /**
     * A "lite parsed" $out stage is similar to other stages involving foreign collections except in
     * some cases the foreign collection is allowed to be sharded.
     */
    class LiteParsed final : public LiteParsedDocumentSourceNestedPipelines<LiteParsed> {
    public:
        LiteParsed(const BSONElement& spec, NamespaceString foreignNss)
            : LiteParsedDocumentSourceNestedPipelines(
                  spec,
                  std::move(foreignNss),
                  std::vector<OwnedLiteParsedPipeline>{} /* no sub-pipelines for $out */) {}

        static std::unique_ptr<LiteParsed> parse(const NamespaceString& nss,
                                                 const BSONElement& spec,
                                                 const LiteParserOptions& options);

        Status checkShardedForeignCollAllowed(const NamespaceString& nss,
                                              bool inMultiDocumentTransaction) const final {
            tassert(12541300, "Expected foreign namespace to be set for $out", _foreignNss);
            if (*_foreignNss != nss) {
                return Status::OK();
            }

            return Status(ErrorCodes::NamespaceCannotBeSharded,
                          "$out to a sharded collection is not allowed");
        }

        PrivilegeVector requiredPrivileges(bool isMongos,
                                           bool bypassDocumentValidation) const override {
            ActionSet actions{ActionType::insert, ActionType::remove};
            if (bypassDocumentValidation) {
                actions.addAction(ActionType::bypassDocumentValidation);
            }

            tassert(12541301, "Expected foreign namespace to be set for $out", _foreignNss);
            return {Privilege(ResourcePattern::forExactNamespace(*_foreignNss), actions)};
        }

        ReadConcernSupportResult supportsReadConcern(repl::ReadConcernLevel level,
                                                     bool isImplicitDefault) const final {
            return {{level == repl::ReadConcernLevel::kLinearizableReadConcern,
                     {ErrorCodes::InvalidOptions,
                      fmt::format("{} cannot be used with a 'linearizable' read concern level",
                                  kStageName)}},
                    Status::OK()};
        }

        bool isWriteStage() const override {
            return true;
        }

        std::unique_ptr<StageParams> getStageParams() const override {
            return std::make_unique<OutStageParams>(_originalBson);
        }
    };

    StageConstraints constraints(PipelineSplitState pipeState) const final;

    Value serialize(const query_shape::SerializationOptions& opts =
                        query_shape::SerializationOptions{}) const final;

    /**
     * Creates a new $out stage from the given arguments.
     */
    static boost::intrusive_ptr<DocumentSource> create(
        NamespaceString outputNs,
        const boost::intrusive_ptr<ExpressionContext>& expCtx,
        boost::optional<TimeseriesOptions> timeseries = boost::none);

    /**
     * Parses a $out stage from the user-supplied BSON.
     */
    static boost::intrusive_ptr<DocumentSource> createFromBson(
        BSONElement elem, const boost::intrusive_ptr<ExpressionContext>& pExpCtx);

    std::string_view getSourceName() const final {
        return kStageName;
    }

    static const Id& id;

    Id getId() const override {
        return id;
    }

    void addVariableRefs(std::set<Variables::Id>* refs) const final {}

private:
    friend boost::intrusive_ptr<exec::agg::Stage> documentSourceOutToStageFn(
        const boost::intrusive_ptr<DocumentSource>& documentSource);

    /**
     * Used to track the $out state for the destructor. $out should clean up different
     * namespaces depending on when the stage was interrupted or failed.
     */
    enum class OutCleanUpProgress {
        kTmpCollExists,
        kRenameComplete,
        kViewCreatedIfNeeded,
        kComplete
    };

    DocumentSourceOut(NamespaceString outputNs,
                      boost::optional<TimeseriesOptions> timeseries,
                      const boost::intrusive_ptr<ExpressionContext>& expCtx)
        : DocumentSourceWriter(kStageName, std::move(outputNs), expCtx) {
        if (timeseries) {
            _timeseries = std::make_shared<TimeseriesOptions>(*timeseries);
        }
    }

    static DocumentSourceOutSpec parseOutSpecAndResolveTargetNamespace(
        const BSONElement& spec, const DatabaseName& defaultDB);


    /**
     * Set if $out is writing to a time-series collection. Its value is passed to the Stage class
     * and not used in DocumentSource at all.
     */
    std::shared_ptr<TimeseriesOptions> _timeseries;
};

}  // namespace mongo
