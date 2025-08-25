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

#include "mongo/base/error_codes.h"
#include "mongo/base/status.h"
#include "mongo/base/string_data.h"
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

#include <list>
#include <memory>
#include <set>
#include <string>
#include <utility>

#include <boost/move/utility_core.hpp>
#include <boost/none.hpp>
#include <boost/optional/optional.hpp>
#include <boost/smart_ptr/intrusive_ptr.hpp>
#include <fmt/format.h>

namespace mongo {
/**
 * Implementation for the $out aggregation stage.
 */
class DocumentSourceOut final : public DocumentSourceWriter {
public:
    static constexpr StringData kStageName = "$out"_sd;

    /**
     * A "lite parsed" $out stage is similar to other stages involving foreign collections except in
     * some cases the foreign collection is allowed to be sharded.
     */
    class LiteParsed final : public LiteParsedDocumentSourceForeignCollection {
    public:
        using LiteParsedDocumentSourceForeignCollection::LiteParsedDocumentSourceForeignCollection;

        static std::unique_ptr<LiteParsed> parse(const NamespaceString& nss,
                                                 const BSONElement& spec,
                                                 const LiteParserOptions& options);

        Status checkShardedForeignCollAllowed(const NamespaceString& nss,
                                              bool inMultiDocumentTransaction) const final {
            if (_foreignNss != nss) {
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

            return {Privilege(ResourcePattern::forExactNamespace(_foreignNss), actions)};
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
    };

    StageConstraints constraints(PipelineSplitState pipeState) const final;

    Value serialize(const SerializationOptions& opts = SerializationOptions{}) const final;

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

    const char* getSourceName() const final {
        return kStageName.data();
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
        : DocumentSourceWriter(kStageName.data(), std::move(outputNs), expCtx) {
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
