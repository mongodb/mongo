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

#include <boost/move/utility_core.hpp>
#include <boost/none.hpp>
#include <boost/optional/optional.hpp>
#include <boost/smart_ptr/intrusive_ptr.hpp>
#include <fmt/format.h>
#include <list>
#include <memory>
#include <set>
#include <string>
#include <utility>

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

namespace mongo {
/**
 * Implementation for the $out aggregation stage.
 */
class DocumentSourceOut final : public DocumentSourceWriter<BSONObj> {
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
                                                 const BSONElement& spec);

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
            using namespace fmt::literals;
            return {
                {level == repl::ReadConcernLevel::kLinearizableReadConcern,
                 {ErrorCodes::InvalidOptions,
                  "{} cannot be used with a 'linearizable' read concern level"_format(kStageName)}},
                Status::OK()};
        }

        bool isWriteStage() const override {
            return true;
        }
    };

    ~DocumentSourceOut() override;

    StageConstraints constraints(Pipeline::SplitState pipeState) const final;

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
        return kStageName.rawData();
    }

    static const Id& id;

    Id getId() const override {
        return id;
    }

    void addVariableRefs(std::set<Variables::Id>* refs) const final {}

private:
    /**
     * Used to track the $out state for the destructor. $out should clean up different namespaces
     * depending on when the stage was interrupted or failed.
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
        : DocumentSourceWriter(kStageName.rawData(), std::move(outputNs), expCtx),
          _writeConcern(expCtx->getOperationContext()->getWriteConcern()),
          _timeseries(std::move(timeseries)) {}

    static DocumentSourceOutSpec parseOutSpecAndResolveTargetNamespace(
        const BSONElement& spec, const DatabaseName& defaultDB);
    void initialize() override;

    void finalize() override;

    void flush(BatchedCommandRequest bcr, BatchedObjects batch) override {
        DocumentSourceWriteBlock writeBlock(pExpCtx->getOperationContext());

        auto insertCommand = bcr.extractInsertRequest();
        insertCommand->setDocuments(std::move(batch));
        auto targetEpoch = boost::none;

        if (_timeseries) {
            uassertStatusOK(pExpCtx->getMongoProcessInterface()->insertTimeseries(
                pExpCtx, _tempNs, std::move(insertCommand), _writeConcern, targetEpoch));
        } else {
            uassertStatusOK(pExpCtx->getMongoProcessInterface()->insert(
                pExpCtx, _tempNs, std::move(insertCommand), _writeConcern, targetEpoch));
        }
    }

    std::pair<BSONObj, int> makeBatchObject(Document doc) const override {
        auto obj = doc.toBson();
        tassert(6628900, "_writeSizeEstimator should be initialized", _writeSizeEstimator);
        return {obj, _writeSizeEstimator->estimateInsertSizeBytes(obj)};
    }

    BatchedCommandRequest makeBatchedWriteRequest() const override;

    void waitWhileFailPointEnabled() override;

    /**
     * Determines if an error exists with the user input and existing collections. This function
     * sets the '_timeseries' member variable and must be run before referencing '_timeseries'
     * variable. The function will error if:
     * 1. The user provides the 'timeseries' field, but a non time-series collection or view exists
     * in that namespace.
     * 2. The user provides the 'timeseries' field with a specification that does not match an
     * existing time-series collection. The function will replace the value of '_timeseries' if the
     * user does not provide the 'timeseries' field, but a time-series collection exists.
     */
    boost::optional<TimeseriesOptions> validateTimeseries();

    NamespaceString makeBucketNsIfTimeseries(const NamespaceString& ns);

    /**
     * Runs a createCollection command on the temporary namespace. Returns
     * nothing, but if the function returns, we assume the temporary collection is created.
     */
    void createTemporaryCollection();

    /**
     * Runs a renameCollection from the temporary namespace to the user requested namespace. Returns
     * nothing, but if the function returns, we assume the rename has succeeded and the temporary
     * namespace no longer exists.
     */
    void renameTemporaryCollection();

    /**
     * Runs a createCollection command to create the view backing the time-series buckets
     * collection. This should only be called if $out is writing to a time-series collection. If the
     * function returns, we assume the view is created.
     */
    void createTimeseriesView();

    // Stash the writeConcern of the original command as the operation context may change by the
    // time we start to flush writes. This is because certain aggregations (e.g. $exchange)
    // establish cursors with batchSize 0 then run subsequent getMore's which use a new operation
    // context. The getMore's will not have an attached writeConcern however we still want to
    // respect the writeConcern of the original command.
    WriteConcernOptions _writeConcern;

    // Holds on to the original collection options and index specs so we can check they didn't
    // change during computation. For time-series collection these values will be on the buckets
    // namespace.
    BSONObj _originalOutOptions;
    std::list<BSONObj> _originalIndexes;

    // The temporary namespace for the $out writes.
    NamespaceString _tempNs;

    // Set if $out is writing to a time-series collection. This is how $out determines if it is
    // writing to a time-series collection or not. Any reference to this variable **must** be after
    // 'validateTimeseries', since 'validateTimeseries' sets this value.
    boost::optional<TimeseriesOptions> _timeseries;

    // Tracks the current state of the temporary collection, and is used for cleanup.
    OutCleanUpProgress _tmpCleanUpState = OutCleanUpProgress::kComplete;
};

}  // namespace mongo
