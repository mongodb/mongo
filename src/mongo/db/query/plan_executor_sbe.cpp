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

#include "mongo/db/query/plan_executor_sbe.h"

#include "mongo/base/string_data.h"
#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/bsontypes.h"
#include "mongo/bson/bsontypes_util.h"
#include "mongo/bson/oid.h"
#include "mongo/db/exec/document_value/value.h"
#include "mongo/db/exec/sbe/values/bson.h"
#include "mongo/db/local_catalog/shard_role_api/transaction_resources.h"
#include "mongo/db/pipeline/expression_context.h"
#include "mongo/db/query/plan_explainer_factory.h"
#include "mongo/db/query/plan_insert_listener.h"
#include "mongo/db/query/plan_yield_policy_remote_cursor.h"
#include "mongo/db/query/sbe_plan_ranker.h"
#include "mongo/db/query/stage_builder/sbe/builder.h"
#include "mongo/db/repl/replication_coordinator.h"
#include "mongo/db/storage/recovery_unit.h"
#include "mongo/logv2/log.h"
#include "mongo/platform/compiler.h"
#include "mongo/platform/decimal128.h"
#include "mongo/s/resharding/resume_token_gen.h"
#include "mongo/util/fail_point.h"
#include "mongo/util/namespace_string_util.h"
#include "mongo/util/shared_buffer.h"
#include "mongo/util/str.h"
#include "mongo/util/time_support.h"
#include "mongo/util/uuid.h"

#include <cstddef>
#include <cstdint>
#include <tuple>

#include <boost/none.hpp>
#include <boost/optional/optional.hpp>
#include <boost/smart_ptr/intrusive_ptr.hpp>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kQuery


namespace mongo {
// This failpoint is defined by the classic executor but is also accessed here.
extern FailPoint planExecutorHangBeforeShouldWaitForInserts;

PlanExecutorSBE::PlanExecutorSBE(OperationContext* opCtx,
                                 std::unique_ptr<CanonicalQuery> cq,
                                 sbe::plan_ranker::CandidatePlan plan,
                                 bool returnOwnedBson,
                                 NamespaceString nss,
                                 bool isOpen,
                                 std::unique_ptr<PlanYieldPolicySBE> yieldPolicy,
                                 boost::optional<size_t> cachedPlanHash,
                                 std::unique_ptr<RemoteCursorMap> remoteCursors,
                                 std::unique_ptr<RemoteExplainVector> remoteExplains,
                                 std::unique_ptr<MultiPlanStage> classicRuntimePlannerStage,
                                 const MultipleCollectionAccessor& mca)
    : _state{isOpen ? State::kOpened : State::kClosed},
      _opCtx(opCtx),
      _nss(std::move(nss)),
      _mustReturnOwnedBson(returnOwnedBson),
      _root{std::move(plan.root)},
      _rootData{std::move(plan.data.stageData)},
      _solution{std::move(plan.solution)},
      _stash{std::move(plan.results)},
      _cq{std::move(cq)},
      _yieldPolicy(std::move(yieldPolicy)),
      _remoteCursors(std::move(remoteCursors)),
      _remoteExplains(std::move(remoteExplains)) {
    invariant(!_nss.isEmpty());
    invariant(_root);
    _root->attachCollectionAcquisition(mca);
    auto& env = _rootData.env;
    if (auto slot = _rootData.staticData->resultSlot) {
        _result = _root->getAccessor(env.ctx, *slot);
        uassert(4822865, "Query does not have result slot.", _result);
    }

    if (auto slot = _rootData.staticData->recordIdSlot) {
        _resultRecordId = _root->getAccessor(env.ctx, *slot);
        uassert(4822866, "Query does not have recordId slot.", _resultRecordId);
    }

    _minRecordIdSlot = env->getSlotIfExists("minRecordId"_sd);
    _maxRecordIdSlot = env->getSlotIfExists("maxRecordId"_sd);

    if (_cq) {
        initializeAccessors(_metadataAccessors, _rootData.staticData->metadataSlots);
    }

    if (!_stash.empty()) {
        // The PlanExecutor keeps an extra reference to the last object pulled out of the PlanStage
        // tree. This is because we want to ensure that the caller of PlanExecutor::getNext() does
        // not free the object and leave a dangling pointer in the PlanStage tree.
        _lastGetNext = _stash.back().first;
    }

    // Callers are allowed to disable yielding for this plan by passing a null yield policy.
    if (_yieldPolicy) {
        // Clear any formerly registered plans and register '_root' to yield. This is needed because
        // multiple candidate plans may have been registered during runtime planning, before the
        // PlanExecutor was created. All but one candidate plan ('_root') have since been discarded.
        _yieldPolicy->clearRegisteredPlans();
        _yieldPolicy->registerPlan(_root.get());
    }
    const bool isMultiPlan = classicRuntimePlannerStage != nullptr;
    const bool isCachedCandidate = plan.fromPlanCache;
    if (!_cq || !_cq->getExpCtx()->getExplain()) {
        // If we're not in explain mode, there is no need to keep rejected candidate plans around.
        classicRuntimePlannerStage.reset();
    }

    // '_solution' can be a nullptr. The following stunt using the "querySolutionPtr" variable is
    // necessary to pacify a Coverity check.
    const QuerySolution* querySolutionPtr = nullptr;
    if (_solution) {
        _secondaryNssVector = _solution->getAllSecondaryNamespaces(_nss);
        querySolutionPtr = _solution.get();
    }

    _planExplainer = plan_explainer_factory::make(_root.get(),
                                                  &_rootData,
                                                  querySolutionPtr,  ///< May be a nullptr.
                                                  isMultiPlan,
                                                  isCachedCandidate,
                                                  cachedPlanHash,
                                                  _rootData.debugInfo,
                                                  std::move(classicRuntimePlannerStage),
                                                  _remoteExplains.get());
    _cursorType = _rootData.staticData->cursorType;

    if (_remoteCursors) {
        for (auto& it : *_remoteCursors) {
            if (auto yieldPolicy =
                    dynamic_cast<PlanYieldPolicyRemoteCursor*>(it.second->getYieldPolicy())) {
                yieldPolicy->registerPlanExecutor(this);
            }
        }
    }
}

void PlanExecutorSBE::saveState() {
    // Discard the slots as we won't access them before subsequent PlanExecutorSBE::getNext()
    // method call.
    const bool discardSlotState = true;
    _root->saveState(discardSlotState);
    _lastGetNext = BSONObj();
}

void PlanExecutorSBE::restoreState(const RestoreContext& context) {
    _root->restoreState();
}

void PlanExecutorSBE::detachFromOperationContext() {
    invariant(_opCtx);
    _root->detachFromOperationContext();
    _opCtx = nullptr;
}

void PlanExecutorSBE::reattachToOperationContext(OperationContext* opCtx) {
    invariant(!_opCtx);
    _root->attachToOperationContext(opCtx);
    _opCtx = opCtx;
}

void PlanExecutorSBE::markAsKilled(Status killStatus) {
    invariant(!killStatus.isOK());
    // If killed multiple times, only retain the first status.
    if (_killStatus.isOK()) {
        _killStatus = killStatus;
    }
}

void PlanExecutorSBE::dispose(OperationContext* opCtx) {
    if (_state != State::kClosed) {
        _root->close();
        _state = State::kClosed;
    }

    _isDisposed = true;
}

void PlanExecutorSBE::stashResult(const BSONObj& obj) {
    invariant(_state == State::kOpened);
    invariant(!_isDisposed);
    _stash.push_front({obj.getOwned(), boost::none});
}

PlanExecutor::ExecState PlanExecutorSBE::getNextDocument(Document& objOut) {
    invariant(!_isDisposed);

    checkFailPointPlanExecAlwaysFails();

    return getNextImpl(&objOut, nullptr);
}

PlanExecutor::ExecState PlanExecutorSBE::getNext(BSONObj* out, RecordId* dlOut) {
    invariant(!_isDisposed);

    checkFailPointPlanExecAlwaysFails();

    BSONObj obj;
    auto result = getNextImpl(&obj, dlOut);
    if (out && result == PlanExecutor::ExecState::ADVANCED) {
        *out = std::move(obj);
    }
    return result;
}

template <typename ObjectType, typename BSONTraits = BSONObj::DefaultSizeTrait>
sbe::PlanState fetchNextImpl(sbe::PlanStage* root,
                             sbe::value::SlotAccessor* resultSlot,
                             sbe::value::SlotAccessor* recordIdSlot,
                             ObjectType* out,
                             RecordId* dlOut,
                             bool returnOwnedBson,
                             const PlanExecutorSBE::MetaDataAccessor* metadata);

template <typename ObjectType>
PlanExecutor::ExecState PlanExecutorSBE::getNextImpl(ObjectType* out, RecordId* dlOut) {
    constexpr bool isDocument = std::is_same_v<ObjectType, Document>;
    constexpr bool isBson = std::is_same_v<ObjectType, BSONObj>;
    static_assert(isDocument || isBson);

    invariant(!_isDisposed);

    // TODO SERVER-93079: This function expects to always produce an output document. This could be
    // optimized in the future if we wish to use SBE for count-like queries which do not need to
    // produce any documents.
    tassert(9212602, "fetchNextImpl() expects a non-null object pointer", out);

    checkFailPointPlanExecAlwaysFails();

    if (!_stash.empty()) {
        auto&& [doc, recordId] = _stash.front();
        if constexpr (isBson) {
            *out = std::move(doc);
        } else {
            *out = Document{doc};
        }
        if (dlOut && recordId) {
            *dlOut = std::move(*recordId);
        }
        _stash.pop_front();
        return PlanExecutor::ExecState::ADVANCED;
    } else if (_root->getCommonStats()->isEOF) {
        // If we had stashed elements and consumed them all, but the PlanStage is also
        // already exhausted, we can return EOF straight away. Otherwise, proceed with
        // fetching the next document.
        _root->close();
        _state = State::kClosed;
        if (!_resumeRecordIdSlot) {
            return PlanExecutor::ExecState::IS_EOF;
        }
    }

    // Capped insert data; declared outside the loop so we hold a shared pointer to the capped
    // insert notifier the entire time we are in the loop. Holding a shared pointer to the capped
    // insert notifier is necessary for the notifierVersion to advance.
    std::unique_ptr<insert_listener::Notifier> notifier;
    if (insert_listener::shouldListenForInserts(_opCtx, _cq.get())) {
        notifier = insert_listener::getCappedInsertNotifier(_opCtx, _nss, _yieldPolicy.get());
    }

    for (;;) {
        if (_state == State::kClosed) {
            if (_resumeRecordIdSlot) {
                uassert(4946306,
                        "Collection scan was asked to track resume token, but found a result "
                        "without a valid RecordId",
                        _tagLastRecordId == sbe::value::TypeTags::RecordId ||
                            _tagLastRecordId == sbe::value::TypeTags::Nothing);
                _rootData.env->resetSlot(
                    *_resumeRecordIdSlot, _tagLastRecordId, _valLastRecordId, false);
            }
            _state = State::kOpened;
            _root->open(false);
        }

        invariant(_state == State::kOpened);

        const MetaDataAccessor* metadataAccessors = isDocument ||
                (_cq &&
                 (_cq->getExpCtxRaw()->getNeedsMerge() ||
                  _cq->getExpCtxRaw()->getForPerShardCursor()))
            ? &_metadataAccessors
            : nullptr;
        auto result = fetchNextImpl(_root.get(),
                                    _result,
                                    _resultRecordId,
                                    out,
                                    dlOut,
                                    _mustReturnOwnedBson,
                                    metadataAccessors);

        if (result == sbe::PlanState::IS_EOF) {
            _root->close();
            _state = State::kClosed;
            _lastGetNext = BSONObj();

            if (MONGO_unlikely(planExecutorHangBeforeShouldWaitForInserts.shouldFail(
                    [this](const BSONObj& data) {
                        const auto fpNss =
                            NamespaceStringUtil::parseFailPointData(data, "namespace"_sd);
                        return fpNss.isEmpty() || _nss == fpNss;
                    }))) {
                LOGV2(5567001,
                      "PlanExecutor - planExecutorHangBeforeShouldWaitForInserts fail point "
                      "enabled. Blocking until fail point is disabled");
                planExecutorHangBeforeShouldWaitForInserts.pauseWhileSet();
            }

            // The !notifier check is necessary because shouldWaitForInserts can return 'true' when
            // shouldListenForInserts returned 'false' (above) in the case of a deadline becoming
            // "unexpired" due to the system clock going backwards.
            if (!_yieldPolicy || !notifier ||
                !insert_listener::shouldWaitForInserts(_opCtx, _cq.get(), _yieldPolicy.get())) {
                return PlanExecutor::ExecState::IS_EOF;
            }

            insert_listener::waitForInserts(_opCtx, _yieldPolicy.get(), notifier);
            // There may be more results, keep going.
            continue;
        } else if (_resumeRecordIdSlot) {
            invariant(_resultRecordId);

            std::tie(_tagLastRecordId, _valLastRecordId) = _resultRecordId->getViewOfValue();
        }

        invariant(result == sbe::PlanState::ADVANCED);
        if (_mustReturnOwnedBson) {
            if constexpr (isBson) {
                _lastGetNext = *out;
            } else {
                auto bson = out->toBsonIfTriviallyConvertible();
                if (bson) {
                    // This basically means that the 'Document' is just a wrapper around BSON
                    // returned by the plan. In this case, 'out' must own it.
                    invariant(out->isOwned());
                    _lastGetNext = *bson;
                } else {
                    // 'Document' was created from 'sbe::Object' and there is no backing BSON for
                    // it.
                    _lastGetNext = BSONObj();
                }
            }
        }
        return PlanExecutor::ExecState::ADVANCED;
    }
}

// Explicitly instantiate the only 2 types supported by 'PlanExecutorSBE::getNextImpl'.
template PlanExecutor::ExecState PlanExecutorSBE::getNextImpl<BSONObj>(BSONObj* out,
                                                                       RecordId* dlOut);
template PlanExecutor::ExecState PlanExecutorSBE::getNextImpl<Document>(Document* out,
                                                                        RecordId* dlOut);

Timestamp PlanExecutorSBE::getLatestOplogTimestamp() const {
    return {};
}

BSONObj PlanExecutorSBE::getPostBatchResumeToken() const {
    return {};
}

namespace {

Document convertToDocument(const sbe::value::Object& obj);

Value convertToValue(sbe::value::TypeTags tag, sbe::value::Value val) {
    switch (tag) {
        case sbe::value::TypeTags::Nothing:
            return Value();

        case sbe::value::TypeTags::NumberInt32:
            return Value(sbe::value::bitcastTo<int32_t>(val));

        case sbe::value::TypeTags::NumberInt64:
            return Value(sbe::value::bitcastTo<long long>(val));

        case sbe::value::TypeTags::NumberDouble:
            return Value(sbe::value::bitcastTo<double>(val));

        case sbe::value::TypeTags::NumberDecimal:
            return Value(sbe::value::bitcastTo<Decimal128>(val));

        case sbe::value::TypeTags::Date:
            return Value(Date_t::fromMillisSinceEpoch(sbe::value::bitcastTo<int64_t>(val)));

        case sbe::value::TypeTags::Timestamp:
            return Value(Timestamp(sbe::value::bitcastTo<uint64_t>(val)));

        case sbe::value::TypeTags::Boolean:
            return Value(sbe::value::bitcastTo<bool>(val));

        case sbe::value::TypeTags::Null:
            return Value(BSONNULL);

        case sbe::value::TypeTags::StringSmall:
        case sbe::value::TypeTags::StringBig:
        case sbe::value::TypeTags::bsonString:
        case sbe::value::TypeTags::bsonSymbol:
            return Value(sbe::value::getStringOrSymbolView(tag, val));

        case sbe::value::TypeTags::Array:
        case sbe::value::TypeTags::ArraySet:
        case sbe::value::TypeTags::ArrayMultiSet: {
            std::vector<Value> vals;
            auto enumerator = sbe::value::ArrayEnumerator{tag, val};
            while (!enumerator.atEnd()) {
                auto [arrTag, arrVal] = enumerator.getViewOfValue();
                enumerator.advance();
                vals.push_back(convertToValue(arrTag, arrVal));
            }
            return Value(std::move(vals));
        }

        case sbe::value::TypeTags::Object:
            return Value(convertToDocument(*sbe::value::getObjectView(val)));

        case sbe::value::TypeTags::ObjectId:
            return Value(OID::from(sbe::value::getObjectIdView(val)->data()));

        case sbe::value::TypeTags::MinKey:
            return Value(kMinBSONKey);

        case sbe::value::TypeTags::MaxKey:
            return Value(kMaxBSONKey);

        case sbe::value::TypeTags::bsonObject:
            return Value(BSONObj{sbe::value::bitcastTo<const char*>(val)});

        case sbe::value::TypeTags::bsonArray:
            return Value(BSONArray{BSONObj{sbe::value::bitcastTo<const char*>(val)}});

        case sbe::value::TypeTags::bsonObjectId:
            return Value(OID::from(sbe::value::bitcastTo<const char*>(val)));

        case sbe::value::TypeTags::bsonBinData:
            return Value(BSONBinData(sbe::value::getBSONBinData(tag, val),
                                     sbe::value::getBSONBinDataSize(tag, val),
                                     sbe::value::getBSONBinDataSubtype(tag, val)));

        case sbe::value::TypeTags::bsonUndefined:
            return Value(BSONUndefined);

        case sbe::value::TypeTags::bsonRegex: {
            auto regex = sbe::value::getBsonRegexView(val);
            return Value(BSONRegEx(regex.pattern, regex.flags));
        }

        case sbe::value::TypeTags::bsonJavascript:
            return Value(sbe::value::getBsonJavascriptView(val));

        case sbe::value::TypeTags::bsonDBPointer: {
            auto dbptr = sbe::value::getBsonDBPointerView(val);
            return Value(BSONDBRef(dbptr.ns, OID::from(dbptr.id)));
        }

        case sbe::value::TypeTags::bsonCodeWScope: {
            auto bcws = sbe::value::getBsonCodeWScopeView(val);
            return Value(BSONCodeWScope(bcws.code, BSONObj(bcws.scope)));
        }

        default:
            MONGO_UNREACHABLE;
    }
}

Document convertToDocument(const sbe::value::Object& obj) {
    MutableDocument doc;
    for (size_t idx = 0; idx < obj.size(); ++idx) {
        auto [tag, val] = obj.getAt(idx);
        const auto& name = obj.field(idx);
        doc.addField(name, convertToValue(tag, val));
    }
    return doc.freeze();
}

}  // namespace

void PlanExecutorSBE::initializeAccessors(
    MetaDataAccessor& accessor, const stage_builder::PlanStageMetadataSlots& metadataSlots) {
    // If Search in SBE is used, we should only initialize these metadata slots if the metadata type
    // is requested by the pipeline. However, since Search in SBE is currently not in use,
    // SERVER-99589 changed it to always initialize these slots, for simplicity of a refactor.
    if (auto slot = metadataSlots.searchScoreSlot) {
        accessor.metadataSearchScore = _root->getAccessor(_rootData.env.ctx, *slot);
    }
    if (auto slot = metadataSlots.searchHighlightsSlot) {
        accessor.metadataSearchHighlights = _root->getAccessor(_rootData.env.ctx, *slot);
    }
    if (auto slot = metadataSlots.searchDetailsSlot) {
        accessor.metadataSearchDetails = _root->getAccessor(_rootData.env.ctx, *slot);
    }
    if (auto slot = metadataSlots.searchSortValuesSlot) {
        accessor.metadataSearchSortValues = _root->getAccessor(_rootData.env.ctx, *slot);
    }
    if (auto slot = metadataSlots.sortKeySlot) {
        accessor.sortKey = _root->getAccessor(_rootData.env.ctx, *slot);
        if (auto sortSpecSlot = _rootData.env->getSlotIfExists("searchSortSpec"_sd)) {
            auto [sortSpecTag, sortSpecVal] =
                _root->getAccessor(_rootData.env.ctx, *sortSpecSlot)->getViewOfValue();
            if (sortSpecTag != sbe::value::TypeTags::Nothing) {
                tassert(7856004,
                        "Incorrect search sort spec type.",
                        sortSpecTag == sbe::value::TypeTags::sortSpec);
                auto sortSpec = sbe::value::bitcastTo<sbe::SortSpec*>(sortSpecVal);
                accessor.isSingleSortKey = sortSpec->getSortPattern().isSingleElementKey();
            }
        }
    }
    if (auto slot = metadataSlots.searchSequenceToken) {
        accessor.metadataSearchSequenceToken = _root->getAccessor(_rootData.env.ctx, *slot);
    }
}

template <typename BSONTraits>
BSONObj PlanExecutorSBE::MetaDataAccessor::appendToBson(BSONObj doc) const {
    if (metadataSearchScore || metadataSearchHighlights || metadataSearchDetails ||
        metadataSearchSortValues || sortKey || metadataSearchSequenceToken) {
        BSONObjBuilder bb(std::move(doc));
        if (metadataSearchScore) {
            auto [tag, val] = metadataSearchScore->getViewOfValue();
            sbe::bson::appendValueToBsonObj(bb, Document::metaFieldSearchScore, tag, val);
        }
        if (metadataSearchHighlights) {
            auto [tag, val] = metadataSearchHighlights->getViewOfValue();
            sbe::bson::appendValueToBsonObj(bb, Document::metaFieldSearchHighlights, tag, val);
        }
        if (metadataSearchDetails) {
            auto [tag, val] = metadataSearchDetails->getViewOfValue();
            sbe::bson::appendValueToBsonObj(bb, Document::metaFieldSearchScoreDetails, tag, val);
        }
        if (metadataSearchSortValues) {
            auto [tag, val] = metadataSearchSortValues->getViewOfValue();
            sbe::bson::appendValueToBsonObj(bb, Document::metaFieldSearchSortValues, tag, val);
        }
        if (sortKey) {
            auto [tag, val] = sortKey->getViewOfValue();
            if (tag != sbe::value::TypeTags::Nothing) {
                bb.append(Document::metaFieldSortKey,
                          DocumentMetadataFields::serializeSortKey(isSingleSortKey,
                                                                   convertToValue(tag, val)));
            }
        }
        if (metadataSearchSequenceToken) {
            auto [tag, val] = metadataSearchSequenceToken->getViewOfValue();
            sbe::bson::appendValueToBsonObj(bb, Document::metaFieldSearchSequenceToken, tag, val);
        }
        return bb.obj<BSONTraits>();
    }
    return doc;
}

template BSONObj PlanExecutorSBE::MetaDataAccessor::appendToBson<BSONObj::DefaultSizeTrait>(
    BSONObj doc) const;
template BSONObj PlanExecutorSBE::MetaDataAccessor::appendToBson<BSONObj::LargeSizeTrait>(
    BSONObj doc) const;

Document PlanExecutorSBE::MetaDataAccessor::appendToDocument(Document doc) const {
    if (metadataSearchScore || metadataSearchHighlights || metadataSearchDetails ||
        metadataSearchSortValues || sortKey || metadataSearchSequenceToken) {
        MutableDocument out(std::move(doc));
        if (metadataSearchScore) {
            auto [tag, val] = metadataSearchScore->getViewOfValue();
            if (tag != sbe::value::TypeTags::Nothing) {
                uassert(7856601,
                        "Metadata search score must be double.",
                        tag == sbe::value::TypeTags::NumberDouble);
                out.metadata().setSearchScore(sbe::value::bitcastTo<double>(val));
            }
        }
        if (metadataSearchHighlights) {
            auto [tag, val] = metadataSearchHighlights->getViewOfValue();
            if (tag != sbe::value::TypeTags::Nothing) {
                uassert(7856602,
                        "Metadata search highlights must be bson array.",
                        tag == sbe::value::TypeTags::bsonArray);
                out.metadata().setSearchHighlights(
                    Value(BSONArray{BSONObj{sbe::value::bitcastTo<const char*>(val)}}));
            }
        }
        if (metadataSearchDetails) {
            auto [tag, val] = metadataSearchDetails->getViewOfValue();
            if (tag != sbe::value::TypeTags::Nothing) {
                uassert(7856603,
                        "Metadata search score details must be bson object.",
                        tag == sbe::value::TypeTags::bsonObject);
                out.metadata().setSearchScoreDetails(
                    BSONObj{sbe::value::bitcastTo<const char*>(val)});
            }
        }
        if (metadataSearchSortValues) {
            auto [tag, val] = metadataSearchSortValues->getViewOfValue();
            if (tag != sbe::value::TypeTags::Nothing) {
                uassert(7856604,
                        "Metadata search sort value must be bson object.",
                        tag == sbe::value::TypeTags::bsonObject);
                out.metadata().setSearchSortValues(
                    BSONObj{sbe::value::bitcastTo<const char*>(val)});
            }
        }
        if (sortKey) {
            auto [tag, val] = sortKey->getViewOfValue();
            if (tag != sbe::value::TypeTags::Nothing) {
                out.metadata().setSortKey(convertToValue(tag, val), isSingleSortKey);
            }
        }
        if (metadataSearchSequenceToken) {
            auto [tag, val] = metadataSearchSequenceToken->getViewOfValue();
            if (tag != sbe::value::TypeTags::Nothing) {
                uassert(8104600,
                        "Metadata search sequence token must be string",
                        tag == sbe::value::TypeTags::bsonString);
                out.metadata().setSearchSequenceToken(
                    Value(sbe::value::getStringOrSymbolView(tag, val)));
            }
        }
        return out.freeze();
    }
    return doc;
}

template <typename ObjectType, typename BSONTraits>
sbe::PlanState fetchNextImpl(sbe::PlanStage* root,
                             sbe::value::SlotAccessor* resultSlot,
                             sbe::value::SlotAccessor* recordIdSlot,
                             ObjectType* out,
                             RecordId* dlOut,
                             bool returnOwnedBson,
                             const PlanExecutorSBE::MetaDataAccessor* metadata) {
    constexpr bool isDocument = std::is_same_v<ObjectType, Document>;
    constexpr bool isBson = std::is_same_v<ObjectType, BSONObj>;
    static_assert(isDocument || isBson);

    invariant(out);
    auto state = root->getNext();

    if (state == sbe::PlanState::IS_EOF) {
        tassert(5609900,
                "Root stage returned EOF but root stage's CommonStats 'isEOF' field is false",
                root->getCommonStats()->isEOF);
        return state;
    }

    invariant(state == sbe::PlanState::ADVANCED);

    if (resultSlot) {
        auto [tag, val] = resultSlot->getViewOfValue();
        if (tag == sbe::value::TypeTags::Object) {
            if constexpr (isBson) {
                BSONObjBuilder bb;
                sbe::bson::convertToBsonObj(bb, sbe::value::getObjectView(val));
                *out = bb.obj<BSONTraits>();
            } else {
                *out = convertToDocument(*sbe::value::getObjectView(val));
            }
        } else if (tag == sbe::value::TypeTags::bsonObject) {
            BSONObj result;
            if (returnOwnedBson) {
                if (auto bsonResultAccessor = resultSlot->as<sbe::value::BSONObjValueAccessor>()) {
                    result = bsonResultAccessor->getOwnedBSONObj();
                } else {
                    auto [ownedTag, ownedVal] = sbe::value::copyValue(tag, val);
                    auto sharedBuf =
                        SharedBuffer(UniqueBuffer::reclaim(sbe::value::bitcastTo<char*>(ownedVal)));
                    result = BSONObj{std::move(sharedBuf)};
                }
            } else {
                result = BSONObj(sbe::value::bitcastTo<const char*>(val), BSONTraits{});
            }

            if constexpr (isBson) {
                *out = std::move(result);
            } else {
                *out = Document{result};
            }
        } else {
            // The query is supposed to return an object.
            MONGO_UNREACHABLE;
        }
        if (metadata) {
            if constexpr (isDocument) {
                *out = metadata->appendToDocument(std::move(*out));
            } else {
                *out = metadata->appendToBson<BSONTraits>(std::move(*out));
            }
        }
    }

    if (dlOut) {
        invariant(recordIdSlot);
        auto [tag, val] = recordIdSlot->getViewOfValue();
        if (tag == sbe::value::TypeTags::RecordId) {
            *dlOut = *sbe::value::getRecordIdView(val);
        }
    }
    return state;
}

template sbe::PlanState fetchNextImpl<BSONObj, BSONObj::DefaultSizeTrait>(
    sbe::PlanStage* root,
    sbe::value::SlotAccessor* resultSlot,
    sbe::value::SlotAccessor* recordIdSlot,
    BSONObj* out,
    RecordId* dlOut,
    bool returnOwnedBson,
    const PlanExecutorSBE::MetaDataAccessor* metadata);

template sbe::PlanState fetchNextImpl<BSONObj, BSONObj::LargeSizeTrait>(
    sbe::PlanStage* root,
    sbe::value::SlotAccessor* resultSlot,
    sbe::value::SlotAccessor* recordIdSlot,
    BSONObj* out,
    RecordId* dlOut,
    bool returnOwnedBson,
    const PlanExecutorSBE::MetaDataAccessor* metadata);

template sbe::PlanState fetchNextImpl<Document>(sbe::PlanStage* root,
                                                sbe::value::SlotAccessor* resultSlot,
                                                sbe::value::SlotAccessor* recordIdSlot,
                                                Document* out,
                                                RecordId* dlOut,
                                                bool returnOwnedBson,
                                                const PlanExecutorSBE::MetaDataAccessor* metadata);

// NOTE: We intentionally do not expose overload for the 'Document' type. The only interface to get
// result from plan in 'Document' type is to call 'PlanExecutorSBE::getNextDocument()'.
template <typename BSONTraits>
sbe::PlanState fetchNext(sbe::PlanStage* root,
                         sbe::value::SlotAccessor* resultSlot,
                         sbe::value::SlotAccessor* recordIdSlot,
                         BSONObj* out,
                         RecordId* dlOut,
                         bool returnOwnedBson) {
    // Sending an empty MetaDataAccessor because we currently only deal with search related
    // metadata, and search query won't reach here.
    return fetchNextImpl<BSONObj, BSONTraits>(
        root, resultSlot, recordIdSlot, out, dlOut, returnOwnedBson, nullptr);
}

template sbe::PlanState fetchNext<BSONObj::DefaultSizeTrait>(sbe::PlanStage* root,
                                                             sbe::value::SlotAccessor* resultSlot,
                                                             sbe::value::SlotAccessor* recordIdSlot,
                                                             BSONObj* out,
                                                             RecordId* dlOut,
                                                             bool returnOwnedBson);

template sbe::PlanState fetchNext<BSONObj::LargeSizeTrait>(sbe::PlanStage* root,
                                                           sbe::value::SlotAccessor* resultSlot,
                                                           sbe::value::SlotAccessor* recordIdSlot,
                                                           BSONObj* out,
                                                           RecordId* dlOut,
                                                           bool returnOwnedBson);


}  // namespace mongo
