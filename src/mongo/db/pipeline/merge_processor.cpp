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

#include "mongo/db/pipeline/merge_processor.h"

#include "mongo/db/operation_context.h"
#include "mongo/db/pipeline/variable_validation.h"
#include "mongo/db/pipeline/writer_util.h"
#include "mongo/db/query/collation/collator_factory_interface.h"
#include "mongo/db/storage/duplicate_key_error_info.h"
#include "mongo/logv2/log.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kQuery

namespace mongo {

namespace {

using MergeStrategy = MergeStrategyDescriptor::MergeStrategy;
using BatchedCommandGenerator = MergeStrategyDescriptor::BatchedCommandGenerator;
using MergeStrategyDescriptorsMap =
    std::map<const MergeStrategyDescriptor::MergeMode, const MergeStrategyDescriptor>;
using WhenMatched = MergeStrategyDescriptor::WhenMatched;
using WhenNotMatched = MergeStrategyDescriptor::WhenNotMatched;
using BatchTransform = MergeStrategyDescriptor::BatchTransform;
using UpdateModification = write_ops::UpdateModification;
using UpsertType = MongoProcessInterface::UpsertType;

enum AllowDuplicateKeyErrorsFromMergeIndex : bool {};

BatchedCommandGenerator makeInsertCommandGenerator() {
    return [](const auto& expCtx, const auto& ns) -> BatchedCommandRequest {
        return makeInsertCommand(ns, expCtx->getBypassDocumentValidation());
    };
}

BatchedCommandGenerator makeUpdateCommandGenerator() {
    return [](const auto& expCtx, const auto& ns) -> BatchedCommandRequest {
        write_ops::UpdateCommandRequest updateOp(ns);
        updateOp.setWriteCommandRequestBase([&] {
            write_ops::WriteCommandRequestBase wcb;
            wcb.setOrdered(false);
            wcb.setBypassDocumentValidation(expCtx->getBypassDocumentValidation());
            return wcb;
        }());
        auto [constants, letParams] =
            expCtx->variablesParseState.transitionalCompatibilitySerialize(expCtx->variables);
        updateOp.setLegacyRuntimeConstants(std::move(constants));
        if (!letParams.isEmpty()) {
            updateOp.setLet(std::move(letParams));
        }
        return BatchedCommandRequest(std::move(updateOp));
    };
}

/**
 * Converts 'batch' into a vector of UpdateOpEntries.
 */
std::vector<write_ops::UpdateOpEntry> constructUpdateEntries(
    const boost::intrusive_ptr<ExpressionContext>& expCtx,
    MongoProcessInterface::BatchedObjects&& batch,
    UpsertType upsert,
    bool multi) {
    std::vector<write_ops::UpdateOpEntry> updateEntries;
    updateEntries.reserve(batch.size());
    for (auto&& obj : batch) {
        write_ops::UpdateOpEntry entry;
        auto&& [q, u, c] = obj;
        entry.setQ(std::move(q));
        entry.setU(std::move(u));
        entry.setC(std::move(c));
        entry.setUpsert(upsert != UpsertType::kNone);
        entry.setUpsertSupplied({{entry.getUpsert(), upsert == UpsertType::kInsertSuppliedDoc}});
        entry.setMulti(multi);
        entry.setCollation(expCtx->getCollatorBSON());

        updateEntries.push_back(std::move(entry));
    }
    return updateEntries;
}

/**
 * Creates a merge strategy which uses update semantics to perform a merge operation.
 */
MergeStrategy makeUpdateStrategy() {
    return [](const auto& expCtx,
              const auto& ns,
              const auto& mergeOnFields,
              const auto& wc,
              auto epoch,
              auto&& batch,
              auto&& bcr,
              UpsertType upsert,
              InsertStrategyStatistics& _) {
        constexpr auto multi = false;
        auto updateCommand = bcr.extractUpdateRequest();
        updateCommand->setUpdates(constructUpdateEntries(expCtx, std::move(batch), upsert, multi));
        uassertStatusOK(expCtx->getMongoProcessInterface()->update(
            expCtx, ns, std::move(updateCommand), wc, upsert, multi, epoch));
    };
}

/**
 * Creates a merge strategy which uses update semantics to perform a merge operation and ensures
 * that each document in the batch has a matching document in the 'ns' collection (note that a
 * matching document may not be modified as a result of an update operation, yet it still will be
 * counted as matching). If at least one document doesn't have a match, this strategy returns an
 * error.
 */
MergeStrategy makeStrictUpdateStrategy() {
    return [](const auto& expCtx,
              const auto& ns,
              const auto& mergeOnFields,
              const auto& wc,
              auto epoch,
              auto&& batch,
              auto&& bcr,
              UpsertType upsert,
              InsertStrategyStatistics& _) {
        const int64_t batchSize = batch.size();
        constexpr auto multi = false;
        auto updateCommand = bcr.extractUpdateRequest();
        updateCommand->setUpdates(constructUpdateEntries(expCtx, std::move(batch), upsert, multi));
        auto updateResult = uassertStatusOK(expCtx->getMongoProcessInterface()->update(
            expCtx, ns, std::move(updateCommand), wc, upsert, multi, epoch));
        uassert(ErrorCodes::MergeStageNoMatchingDocument,
                "$merge could not find a matching document in the target collection "
                "for at least one document in the source collection",
                updateResult.nMatched == batchSize);
    };
}

bool collatorsMatch(const ExpressionContext* expCtx, const BSONObj& indexCollator) {
    if (CollatorInterface::isSimpleCollator(expCtx->getCollator())) {
        return indexCollator.isEmpty() || indexCollator.woCompare(CollationSpec::kSimpleSpec) == 0;
    } else {
        auto indexCollatorInterface = uassertStatusOK(
            CollatorFactoryInterface::get(expCtx->getOperationContext()->getServiceContext())
                ->makeFromBSON(indexCollator));
        return CollatorInterface::collatorsMatch(expCtx->getCollator(),
                                                 indexCollatorInterface.get());
    }
}

bool keyPatternNamesExactPaths(const BSONObj& keyPattern,
                               const std::set<FieldPath>& uniqueKeyPaths) {
    size_t nFieldsMatched = 0;
    for (auto&& elem : keyPattern) {
        if (!elem.isNumber()) {
            return false;
        }
        if (uniqueKeyPaths.find(elem.fieldNameStringData()) == uniqueKeyPaths.end()) {
            return false;
        }
        ++nFieldsMatched;
    }
    return nFieldsMatched == uniqueKeyPaths.size();
}

bool ignoreDuplicateKeyError(const boost::intrusive_ptr<ExpressionContext>& expCtx,
                             const std::set<FieldPath>& mergeOnFields,
                             const DuplicateKeyErrorInfo& errorInfo) {
    if (!collatorsMatch(expCtx.get(), errorInfo.getCollation())) {
        return false;
    }
    const auto& keyPattern = errorInfo.getKeyPattern();
    if (keyPatternNamesExactPaths(keyPattern, mergeOnFields)) {
        return true;
    }
    if (keyPattern.nFields() == 1 && keyPattern.firstElementFieldNameStringData() == "_id" &&
        mergeOnFields.contains("_id")) {
        return true;
    }
    return false;
}

template <AllowDuplicateKeyErrorsFromMergeIndex allowDuplicateKeyErrorsFromMergeIndex>
inline bool canIgnoreInsertStatus(const Status& status,
                                  const boost::intrusive_ptr<ExpressionContext>& expCtx,
                                  const std::set<FieldPath>& mergeOnFields) {
    if constexpr (!allowDuplicateKeyErrorsFromMergeIndex) {
        return false;
    } else {
        if (status.code() == ErrorCodes::DuplicateKey) {
            const auto& extraInfo = *status.template extraInfo<DuplicateKeyErrorInfo>();
            return ignoreDuplicateKeyError(expCtx, mergeOnFields, extraInfo);
        }
        return false;
    }
}

/**
 * Creates a merge strategy which uses insert semantics to perform a merge operation.
 */
MergeStrategy makeInsertStrategy() {
    return [](const boost::intrusive_ptr<ExpressionContext>& expCtx,
              const NamespaceString& ns,
              const std::set<FieldPath>& mergeOnFields,
              const auto& wc,
              auto epoch,
              auto&& batch,
              auto&& bcr,
              UpsertType upsertType,
              InsertStrategyStatistics& _) {
        std::vector<BSONObj> objectsToInsert(batch.size());
        // The batch stores replacement style updates, but for this "insert" style of $merge we'd
        // like to just insert the new document without attempting any sort of replacement.
        std::transform(batch.begin(), batch.end(), objectsToInsert.begin(), [](const auto& obj) {
            return get<UpdateModification>(obj).getUpdateReplacement();
        });
        auto insertCommand = bcr.extractInsertRequest();
        insertCommand->setDocuments(std::move(objectsToInsert));
        auto insertResult = expCtx->getMongoProcessInterface()->insert(
            expCtx, ns, std::move(insertCommand), wc, epoch);
        for (const write_ops::WriteError& writeError : insertResult) {
            uassertStatusOK(writeError.getStatus());
        }
    };
}

bool shouldAttemptInsert(const InsertStrategyStatistics& insertStats) {
    return insertStats.insertDocAttempts <
        static_cast<size_t>(internalQueryMergeMinInsertAttempts.loadRelaxed()) ||
        static_cast<double>(insertStats.insertErrors) / insertStats.insertDocAttempts <=
        internalQueryMergeMaxInsertErrorRate.loadRelaxed();
}

void runBackupStrategy(const boost::intrusive_ptr<ExpressionContext>& expCtx,
                       const NamespaceString& ns,
                       const std::set<FieldPath>& mergeOnFields,
                       const auto& wc,
                       auto epoch,
                       MongoProcessInterface::BatchedObjects&& batch,
                       BatchedCommandRequest&& bcr,
                       UpsertType upsertType,
                       const MergeStrategy& backupStrategy,
                       const BatchTransform& backupTransform,
                       InsertStrategyStatistics& insertStats) {
    if (batch.empty()) {
        return;
    }

    if (backupTransform) {
        for (auto& batchObject : batch) {
            backupTransform(batchObject);
        }
    }
    backupStrategy(expCtx,
                   ns,
                   mergeOnFields,
                   wc,
                   epoch,
                   std::move(batch),
                   std::move(bcr),
                   upsertType,
                   insertStats);
}

template <AllowDuplicateKeyErrorsFromMergeIndex allowDuplicateKeyErrorsFromMergeIndex>
MergeStrategy makeInsertStrategyWithBackup(BatchTransform backupTransform,
                                           UpsertType backupUpsertType) {
    return [backupTransform = std::move(backupTransform),
            backupUpsertType = backupUpsertType,
            backupStrategy = makeUpdateStrategy(),
            backupCommandGenerator =
                makeUpdateCommandGenerator()](const boost::intrusive_ptr<ExpressionContext>& expCtx,
                                              const NamespaceString& ns,
                                              const std::set<FieldPath>& mergeOnFields,
                                              const auto& wc,
                                              auto epoch,
                                              MongoProcessInterface::BatchedObjects&& batch,
                                              auto&& bcr,
                                              UpsertType upsertType,
                                              InsertStrategyStatistics& insertStats) {
        if (!shouldAttemptInsert(insertStats)) {
            return runBackupStrategy(expCtx,
                                     ns,
                                     mergeOnFields,
                                     wc,
                                     epoch,
                                     std::move(batch),
                                     backupCommandGenerator(expCtx, ns),
                                     backupUpsertType,
                                     backupStrategy,
                                     backupTransform,
                                     insertStats);
        }

        std::vector<BSONObj> objectsToInsert(batch.size());
        // The batch stores replacement style updates, but for this "insert" style of $merge we'd
        // like to just insert the new document without attempting any sort of replacement.
        std::transform(batch.begin(), batch.end(), objectsToInsert.begin(), [](const auto& obj) {
            return get<UpdateModification>(obj).getUpdateReplacement();
        });
        auto insertCommand = bcr.extractInsertRequest();
        insertCommand->setDocuments(std::move(objectsToInsert));

        auto insertResult = expCtx->getMongoProcessInterface()->insert(
            expCtx, ns, std::move(insertCommand), wc, epoch);
        insertStats.insertDocAttempts += batch.size();
        insertStats.insertErrors += insertResult.size();
        if (insertResult.empty()) {
            return;
        }

        MongoProcessInterface::BatchedObjects updateBatch;
        updateBatch.reserve(insertResult.size());
        for (const write_ops::WriteError& writeError : insertResult) {
            if (writeError.getStatus().isOK() ||
                canIgnoreInsertStatus<allowDuplicateKeyErrorsFromMergeIndex>(
                    writeError.getStatus(), expCtx, mergeOnFields)) {
                continue;
            }
            // DuplicateKey error might mean that matching document exists, so we retry the
            // operation with update strategy instead. Any other error should be propagated.
            if (writeError.getStatus().code() != ErrorCodes::DuplicateKey) {
                uassertStatusOK(writeError.getStatus());
            }
            updateBatch.push_back(std::move(batch[writeError.getIndex()]));
        }

        runBackupStrategy(expCtx,
                          ns,
                          mergeOnFields,
                          wc,
                          epoch,
                          std::move(updateBatch),
                          backupCommandGenerator(expCtx, ns),
                          backupUpsertType,
                          backupStrategy,
                          backupTransform,
                          insertStats);
    };
}

/**
 * Creates a batched object transformation function which wraps 'obj' into the given 'updateOp'
 * operator.
 */
BatchTransform makeUpdateTransform(const std::string& updateOp) {
    return [updateOp](auto& obj) {
        get<UpdateModification>(obj) = UpdateModification::parseFromClassicUpdate(
            BSON(updateOp << get<UpdateModification>(obj).getUpdateReplacement()));
    };
}

template <typename BaseContainer, typename ExtendedContainer>
BaseContainer extendContainer(BaseContainer baseContainer, ExtendedContainer extendedContainer) {
    baseContainer.insert(std::make_move_iterator(extendedContainer.begin()),
                         std::make_move_iterator(extendedContainer.end()));
    return baseContainer;
}

}  // namespace

/**
 * Returns a map that contains descriptors for all supported merge strategies for the $merge stage.
 * Each descriptor is constant and stateless and thus, can be shared by all $merge stages. A
 * descriptor is accessed using a pair of whenMatched/whenNotMatched merge modes, which defines the
 * semantics of the merge operation. When a $merge stage is created, a merge descriptor is selected
 * from this map based on the requested merge modes, and then passed to the $merge stage
 * constructor.
 */
const MergeStrategyDescriptorsMap& getMergeStrategyDescriptors(
    MergeProcessor::AllowInsertWithUpdateBackupStrategies allowInsertWithUpdateBackupStrategies) {
    // Rather than defining this map as a global object, we'll wrap the static map into a function
    // to prevent static initialization order fiasco which may happen since ActionType instances
    // are also defined as global objects and there is no way to tell the linker which objects must
    // be initialized first. By wrapping the map into a function we can guarantee that it won't be
    // initialized until the first use, which is when the program already started and all global
    // variables had been initialized.

    // This map contains merge strategy descriptors that don't depend on the feature flag
    static const auto kBaseMergeStrategyDescriptors =
        MergeStrategyDescriptorsMap{// whenMatched: replace, whenNotMatched: fail
                                    {MergeStrategyDescriptor::kReplaceFailMode,
                                     {MergeStrategyDescriptor::kReplaceFailMode,
                                      {ActionType::update},
                                      makeStrictUpdateStrategy(),
                                      {},
                                      UpsertType::kNone,
                                      makeUpdateCommandGenerator()}},
                                    // whenMatched: replace, whenNotMatched: discard
                                    {MergeStrategyDescriptor::kReplaceDiscardMode,
                                     {MergeStrategyDescriptor::kReplaceDiscardMode,
                                      {ActionType::update},
                                      makeUpdateStrategy(),
                                      {},
                                      UpsertType::kNone,
                                      makeUpdateCommandGenerator()}},
                                    // whenMatched: merge, whenNotMatched: fail
                                    {MergeStrategyDescriptor::kMergeFailMode,
                                     {MergeStrategyDescriptor::kMergeFailMode,
                                      {ActionType::update},
                                      makeStrictUpdateStrategy(),
                                      makeUpdateTransform("$set"),
                                      UpsertType::kNone,
                                      makeUpdateCommandGenerator()}},
                                    // whenMatched: merge, whenNotMatched: discard
                                    {MergeStrategyDescriptor::kMergeDiscardMode,
                                     {MergeStrategyDescriptor::kMergeDiscardMode,
                                      {ActionType::update},
                                      makeUpdateStrategy(),
                                      makeUpdateTransform("$set"),
                                      UpsertType::kNone,
                                      makeUpdateCommandGenerator()}},
                                    // whenMatched: [pipeline], whenNotMatched: insert
                                    {MergeStrategyDescriptor::kPipelineInsertMode,
                                     {MergeStrategyDescriptor::kPipelineInsertMode,
                                      {ActionType::insert, ActionType::update},
                                      makeUpdateStrategy(),
                                      {},
                                      UpsertType::kInsertSuppliedDoc,
                                      makeUpdateCommandGenerator()}},
                                    // whenMatched: [pipeline], whenNotMatched: fail
                                    {MergeStrategyDescriptor::kPipelineFailMode,
                                     {MergeStrategyDescriptor::kPipelineFailMode,
                                      {ActionType::update},
                                      makeStrictUpdateStrategy(),
                                      {},
                                      UpsertType::kNone,
                                      makeUpdateCommandGenerator()}},
                                    // whenMatched: [pipeline], whenNotMatched: discard
                                    {MergeStrategyDescriptor::kPipelineDiscardMode,
                                     {MergeStrategyDescriptor::kPipelineDiscardMode,
                                      {ActionType::update},
                                      makeUpdateStrategy(),
                                      {},
                                      UpsertType::kNone,
                                      makeUpdateCommandGenerator()}},
                                    // whenMatched: fail, whenNotMatched: insert
                                    {MergeStrategyDescriptor::kFailInsertMode,
                                     {MergeStrategyDescriptor::kFailInsertMode,
                                      {ActionType::insert},
                                      makeInsertStrategy(),
                                      {},
                                      UpsertType::kNone,
                                      makeInsertCommandGenerator()}}};

    static const auto kMergeStrategyDescriptorsWithoutBackup = extendContainer(
        kBaseMergeStrategyDescriptors,
        MergeStrategyDescriptorsMap{// whenMatched: replace, whenNotMatched: insert
                                    {MergeStrategyDescriptor::kReplaceInsertMode,
                                     {MergeStrategyDescriptor::kReplaceInsertMode,
                                      {ActionType::insert, ActionType::update},
                                      makeUpdateStrategy(),
                                      {},
                                      UpsertType::kGenerateNewDoc,
                                      makeUpdateCommandGenerator()}},
                                    // whenMatched: merge, whenNotMatched: insert
                                    {MergeStrategyDescriptor::kMergeInsertMode,
                                     {MergeStrategyDescriptor::kMergeInsertMode,
                                      {ActionType::insert, ActionType::update},
                                      makeUpdateStrategy(),
                                      makeUpdateTransform("$set"),
                                      UpsertType::kGenerateNewDoc,
                                      makeUpdateCommandGenerator()}},
                                    // whenMatched: keepExisting, whenNotMatched: insert
                                    {MergeStrategyDescriptor::kKeepExistingInsertMode,
                                     {MergeStrategyDescriptor::kKeepExistingInsertMode,
                                      {ActionType::insert, ActionType::update},
                                      makeUpdateStrategy(),
                                      makeUpdateTransform("$setOnInsert"),
                                      UpsertType::kGenerateNewDoc,
                                      makeUpdateCommandGenerator()}}});

    static const auto kMergeStrategyDescriptorsWithBackup = extendContainer(
        kBaseMergeStrategyDescriptors,
        MergeStrategyDescriptorsMap{
            // whenMatched: replace, whenNotMatched: insert with backup
            {MergeStrategyDescriptor::kReplaceInsertMode,
             {MergeStrategyDescriptor::kReplaceInsertMode,
              {ActionType::insert, ActionType::update},
              makeInsertStrategyWithBackup<AllowDuplicateKeyErrorsFromMergeIndex{false}>(
                  {}, UpsertType::kGenerateNewDoc),
              {},
              UpsertType::kNone,
              makeInsertCommandGenerator(),
              /*.isInsertWithUpdateBackupStrategy=*/true}},
            // whenMatched: merge, whenNotMatched: insert  with backup
            {MergeStrategyDescriptor::kMergeInsertMode,
             {MergeStrategyDescriptor::kMergeInsertMode,
              {ActionType::insert, ActionType::update},
              makeInsertStrategyWithBackup<AllowDuplicateKeyErrorsFromMergeIndex{false}>(
                  makeUpdateTransform("$set"), UpsertType::kGenerateNewDoc),
              {},
              UpsertType::kNone,
              makeInsertCommandGenerator(),
              /*.isInsertWithUpdateBackupStrategy=*/true}},
            // whenMatched: keepExisting, whenNotMatched: insert with backup
            {MergeStrategyDescriptor::kKeepExistingInsertMode,
             {MergeStrategyDescriptor::kKeepExistingInsertMode,
              {ActionType::insert, ActionType::update},
              makeInsertStrategyWithBackup<AllowDuplicateKeyErrorsFromMergeIndex{true}>(
                  makeUpdateTransform("$setOnInsert"), UpsertType::kGenerateNewDoc),
              {},
              UpsertType::kNone,
              makeInsertCommandGenerator(),
              /*.isInsertWithUpdateBackupStrategy=*/true}},
        });

    if (allowInsertWithUpdateBackupStrategies) {
        return kMergeStrategyDescriptorsWithBackup;
    } else {
        return kMergeStrategyDescriptorsWithoutBackup;
    }
}

MergeProcessor::MergeProcessor(
    const boost::intrusive_ptr<ExpressionContext>& expCtx,
    MergeStrategyDescriptor::WhenMatched whenMatched,
    MergeStrategyDescriptor::WhenNotMatched whenNotMatched,
    boost::optional<BSONObj> letVariables,
    boost::optional<std::vector<BSONObj>> pipeline,
    boost::optional<ChunkVersion> collectionPlacementVersion,
    bool allowMergeOnNullishValues,
    AllowInsertWithUpdateBackupStrategies allowInsertWithUpdateBackupStrategies)
    : _expCtx(expCtx),
      _writeConcern(expCtx->getOperationContext()->getWriteConcern()),
      _descriptor(getMergeStrategyDescriptors(allowInsertWithUpdateBackupStrategies)
                      .at({whenMatched, whenNotMatched})),
      _pipeline(std::move(pipeline)),
      _collectionPlacementVersion(collectionPlacementVersion),
      _allowMergeOnNullishValues(allowMergeOnNullishValues) {
    if (!letVariables) {
        return;
    }

    for (auto&& varElem : *letVariables) {
        const auto varName = varElem.fieldNameStringData();
        variableValidation::validateNameForUserWrite(varName);

        _letVariables.emplace_back(
            std::string{varName},
            Expression::parseOperand(expCtx.get(), varElem, expCtx->variablesParseState),
            // Variable::Id is set to INT64_MIN as it is not needed for processing $merge stage.
            // The '_letVariables' are evaluated in resolveLetVariablesIfNeeded(), serialized into
            // BSONObj and later placed into MongoProcessInterface::BatchObject.
            INT64_MIN);
    }
}

MongoProcessInterface::BatchObject MergeProcessor::makeBatchObject(
    Document doc,
    const std::set<FieldPath>& mergeOnFieldPaths,
    bool mergeOnFieldPathsIncludeId) const {
    // Generate an _id if the uniqueKey includes _id but the document doesn't have one.
    if (mergeOnFieldPathsIncludeId && doc.getField("_id"_sd).missing()) {
        MutableDocument mutableDoc(std::move(doc));
        mutableDoc["_id"_sd] = Value(OID::gen());
        doc = mutableDoc.freeze();
    }

    auto mergeOnFields = _extractMergeOnFieldsFromDoc(doc, mergeOnFieldPaths);
    auto mod = makeBatchUpdateModification(doc);
    auto vars = resolveLetVariablesIfNeeded(doc);
    MongoProcessInterface::BatchObject batchObject{
        std::move(mergeOnFields), std::move(mod), std::move(vars)};
    if (_descriptor.transform) {
        _descriptor.transform(batchObject);
    }

    return batchObject;
}

void MergeProcessor::flush(const NamespaceString& outputNs,
                           const std::set<FieldPath>& mergeOnFields,
                           BatchedCommandRequest bcr,
                           MongoProcessInterface::BatchedObjects batch) {
    auto targetEpoch = _collectionPlacementVersion
        ? boost::optional<OID>(_collectionPlacementVersion->epoch())
        : boost::none;
    _descriptor.strategy(_expCtx,
                         outputNs,
                         mergeOnFields,
                         _writeConcern,
                         targetEpoch,
                         std::move(batch),
                         std::move(bcr),
                         _descriptor.upsertType,
                         _insertStats);
}

BSONObj MergeProcessor::_extractMergeOnFieldsFromDoc(
    const Document& doc, const std::set<FieldPath>& mergeOnFields) const {
    MutableDocument result;
    for (const auto& field : mergeOnFields) {
        Value value{doc};
        for (size_t i = 0; i < field.getPathLength(); ++i) {
            value = value.getDocument().getField(field.getFieldName(i));
            uassert(
                51185,
                fmt::format("$merge write error: 'on' field '{}' is an array", field.fullPath()),
                !value.isArray());
            if (i + 1 < field.getPathLength() && !value.isObject()) {
                value = Value();
                break;
            }
        }
        uassert(51132,
                fmt::format("$merge write error: 'on' field '{}' cannot be missing, null or "
                            "undefined if supporting index is sparse",
                            field.fullPath()),
                _allowMergeOnNullishValues || !value.nullish());
        if (value.nullish()) {
            result.addField(field.fullPath(), Value(BSONNULL));
        } else {
            result.addField(field.fullPath(), std::move(value));
        }
    }
    return result.freeze().toBson();
}

bool MergeProcessor::shouldFlush(size_t currentBatchSize) {
    return _descriptor.isInsertWithUpdateBackupStrategy &&
        _insertStats.insertDocAttempts < _insertStats.minInsertAttempts &&
        _insertStats.insertDocAttempts + currentBatchSize >= _insertStats.minInsertAttempts;
}

}  // namespace mongo
