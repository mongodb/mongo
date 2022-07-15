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


#include "mongo/platform/basic.h"

#include "mongo/db/query/plan_executor_sbe.h"

#include "mongo/db/db_raii.h"
#include "mongo/db/exec/sbe/expressions/expression.h"
#include "mongo/db/exec/sbe/values/bson.h"
#include "mongo/db/query/plan_explainer_factory.h"
#include "mongo/db/query/plan_insert_listener.h"
#include "mongo/db/query/sbe_stage_builder.h"
#include "mongo/logv2/log.h"
#include "mongo/s/resharding/resume_token_gen.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kQuery


namespace mongo {
// This failpoint is defined by the classic executor but is also accessed here.
extern FailPoint planExecutorHangBeforeShouldWaitForInserts;

PlanExecutorSBE::PlanExecutorSBE(OperationContext* opCtx,
                                 std::unique_ptr<CanonicalQuery> cq,
                                 std::unique_ptr<optimizer::AbstractABTPrinter> optimizerData,
                                 sbe::CandidatePlans candidates,
                                 bool returnOwnedBson,
                                 NamespaceString nss,
                                 bool isOpen,
                                 std::unique_ptr<PlanYieldPolicySBE> yieldPolicy)
    : _state{isOpen ? State::kOpened : State::kClosed},
      _opCtx(opCtx),
      _nss(std::move(nss)),
      _mustReturnOwnedBson(returnOwnedBson),
      _root{std::move(candidates.winner().root)},
      _rootData{std::move(candidates.winner().data)},
      _solution{std::move(candidates.winner().solution)},
      _stash{std::move(candidates.winner().results)},
      _cq{std::move(cq)},
      _yieldPolicy(std::move(yieldPolicy)) {
    invariant(!_nss.isEmpty());
    invariant(_root);

    if (auto slot = _rootData.outputs.getIfExists(stage_builder::PlanStageSlots::kResult); slot) {
        _result = _root->getAccessor(_rootData.ctx, *slot);
        uassert(4822865, "Query does not have result slot.", _result);
    }

    if (auto slot = _rootData.outputs.getIfExists(stage_builder::PlanStageSlots::kRecordId); slot) {
        _resultRecordId = _root->getAccessor(_rootData.ctx, *slot);
        uassert(4822866, "Query does not have recordId slot.", _resultRecordId);
    }

    if (_rootData.shouldTrackLatestOplogTimestamp) {
        _oplogTs = _rootData.env->getAccessor(_rootData.env->getSlot("oplogTs"_sd));
    }

    if (_rootData.shouldUseTailableScan) {
        _resumeRecordIdSlot = _rootData.env->getSlot("resumeRecordId"_sd);
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

    const auto isMultiPlan = candidates.plans.size() > 1;

    if (!_cq || !_cq->getExpCtx()->explain) {
        // If we're not in explain mode, there is no need to keep rejected candidate plans around.
        candidates.plans.clear();
    } else {
        // Keep only rejected candidate plans.
        candidates.plans.erase(candidates.plans.begin() + candidates.winnerIdx);
    }

    if (_solution) {
        _secondaryNssVector = _solution->getAllSecondaryNamespaces(_nss);
    }

    _planExplainer = plan_explainer_factory::make(_root.get(),
                                                  &_rootData,
                                                  _solution.get(),
                                                  std::move(optimizerData),
                                                  std::move(candidates.plans),
                                                  isMultiPlan,
                                                  _rootData.debugInfo);
}

void PlanExecutorSBE::saveState() {
    if (_isSaveRecoveryUnitAcrossCommandsEnabled) {
        _root->saveState(false /* NOT relinquishing cursor */);

        // Put the RU into 'kCommit' mode so that subsequent calls to abandonSnapshot() keep
        // cursors positioned. This ensures that no pointers into memory owned by the storage
        // engine held by the SBE PlanStage tree become invalid while the executor is in a saved
        // state.
        _opCtx->recoveryUnit()->setAbandonSnapshotMode(RecoveryUnit::AbandonSnapshotMode::kCommit);
        _opCtx->recoveryUnit()->abandonSnapshot();
    } else {
        _root->saveState(true /* relinquish cursor */);
    }

    _yieldPolicy->setYieldable(nullptr);
    _lastGetNext = BSONObj();
}

void PlanExecutorSBE::restoreState(const RestoreContext& context) {
    _yieldPolicy->setYieldable(context.collection());

    if (_isSaveRecoveryUnitAcrossCommandsEnabled) {
        _root->restoreState(false /* NOT relinquishing cursor */);

        // Put the RU back into 'kAbort' mode. Since the executor is now in a restored state, calls
        // to doAbandonSnapshot() only happen if the query has failed and the executor will not be
        // used again. In this case, we do not rely on the guarantees provided by 'kCommit' mode.
        _opCtx->recoveryUnit()->setAbandonSnapshotMode(RecoveryUnit::AbandonSnapshotMode::kAbort);
    } else {
        _root->restoreState(true /* relinquish cursor */);
    }
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

PlanExecutor::ExecState PlanExecutorSBE::getNextDocument(Document* objOut, RecordId* dlOut) {
    invariant(!_isDisposed);

    checkFailPointPlanExecAlwaysFails();

    Document obj;
    auto result = getNextImpl(&obj, dlOut);
    if (result == PlanExecutor::ExecState::ADVANCED) {
        *objOut = std::move(obj);
    }
    return result;
}

PlanExecutor::ExecState PlanExecutorSBE::getNext(BSONObj* out, RecordId* dlOut) {
    invariant(!_isDisposed);

    checkFailPointPlanExecAlwaysFails();

    BSONObj obj;
    auto result = getNextImpl(&obj, dlOut);
    if (result == PlanExecutor::ExecState::ADVANCED) {
        *out = std::move(obj);
    }
    return result;
}

template <typename ObjectType>
sbe::PlanState fetchNextImpl(sbe::PlanStage* root,
                             sbe::value::SlotAccessor* resultSlot,
                             sbe::value::SlotAccessor* recordIdSlot,
                             ObjectType* out,
                             RecordId* dlOut,
                             bool returnOwnedBson);

template <typename ObjectType>
PlanExecutor::ExecState PlanExecutorSBE::getNextImpl(ObjectType* out, RecordId* dlOut) {
    constexpr bool isDocument = std::is_same_v<ObjectType, Document>;
    constexpr bool isBson = std::is_same_v<ObjectType, BSONObj>;
    static_assert(isDocument || isBson);

    invariant(!_isDisposed);

    checkFailPointPlanExecAlwaysFails();

    if (!_stash.empty()) {
        auto&& [doc, recordId] = _stash.front();
        if constexpr (isBson) {
            *out = std::move(doc);
        } else {
            *out = Document{std::move(doc)};
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
    //
    // Note that we need to hold a database intent lock before acquiring a notifier.
    boost::optional<AutoGetCollectionForReadMaybeLockFree> coll;
    insert_listener::CappedInsertNotifierData cappedInsertNotifierData;
    if (insert_listener::shouldListenForInserts(_opCtx, _cq.get())) {
        if (!_opCtx->lockState()->isCollectionLockedForMode(_nss, MODE_IS)) {
            coll.emplace(_opCtx, _nss);
        }

        cappedInsertNotifierData.notifier =
            insert_listener::getCappedInsertNotifier(_opCtx, _nss, _yieldPolicy.get());
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

        auto result =
            fetchNextImpl(_root.get(), _result, _resultRecordId, out, dlOut, _mustReturnOwnedBson);
        if (result == sbe::PlanState::IS_EOF) {
            _root->close();
            _state = State::kClosed;
            _lastGetNext = BSONObj();

            if (MONGO_unlikely(planExecutorHangBeforeShouldWaitForInserts.shouldFail(
                    [this](const BSONObj& data) {
                        if (data.hasField("namespace") &&
                            _nss != NamespaceString(data.getStringField("namespace"))) {
                            return false;
                        }
                        return true;
                    }))) {
                LOGV2(5567001,
                      "PlanExecutor - planExecutorHangBeforeShouldWaitForInserts fail point "
                      "enabled. Blocking until fail point is disabled");
                planExecutorHangBeforeShouldWaitForInserts.pauseWhileSet();
            }

            if (!insert_listener::shouldWaitForInserts(_opCtx, _cq.get(), _yieldPolicy.get())) {
                return PlanExecutor::ExecState::IS_EOF;
            }

            insert_listener::waitForInserts(_opCtx, _yieldPolicy.get(), &cappedInsertNotifierData);
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
    if (_rootData.shouldTrackLatestOplogTimestamp) {
        tassert(5567201,
                "The '_oplogTs' accessor should be populated when "
                "'shouldTrackLatestOplogTimestamp' is true",
                _oplogTs);

        auto [tag, val] = _oplogTs->getViewOfValue();
        if (tag != sbe::value::TypeTags::Nothing) {
            const auto msgTag = tag;
            uassert(4822868,
                    str::stream() << "Collection scan was asked to track latest operation time, "
                                     "but found a result without a valid 'ts' field: "
                                  << msgTag,
                    tag == sbe::value::TypeTags::Timestamp);
            return Timestamp{sbe::value::bitcastTo<uint64_t>(val)};
        }
    }
    return {};
}

BSONObj PlanExecutorSBE::getPostBatchResumeToken() const {
    if (_rootData.shouldTrackResumeToken) {
        invariant(_resultRecordId);

        auto [tag, val] = _resultRecordId->getViewOfValue();
        if (tag != sbe::value::TypeTags::Nothing) {
            const auto msgTag = tag;
            uassert(4822869,
                    str::stream() << "Collection scan was asked to track resume token, "
                                     "but found a result without a valid RecordId: "
                                  << msgTag,
                    tag == sbe::value::TypeTags::RecordId);
            BSONObjBuilder builder;
            sbe::value::getRecordIdView(val)->serializeToken("$recordId", &builder);
            return builder.obj();
        }
    }

    if (_rootData.shouldTrackLatestOplogTimestamp) {
        return ResumeTokenOplogTimestamp{getLatestOplogTimestamp()}.toBSON();
    }

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
        case sbe::value::TypeTags::ArraySet: {
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

template <typename ObjectType>
sbe::PlanState fetchNextImpl(sbe::PlanStage* root,
                             sbe::value::SlotAccessor* resultSlot,
                             sbe::value::SlotAccessor* recordIdSlot,
                             ObjectType* out,
                             RecordId* dlOut,
                             bool returnOwnedBson) {
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
                *out = bb.obj();
            } else {
                *out = convertToDocument(*sbe::value::getObjectView(val));
            }
        } else if (tag == sbe::value::TypeTags::bsonObject) {
            BSONObj result;
            if (returnOwnedBson) {
                auto [ownedTag, ownedVal] = resultSlot->copyOrMoveValue();
                auto sharedBuf =
                    SharedBuffer(UniqueBuffer::reclaim(sbe::value::bitcastTo<char*>(ownedVal)));
                result = BSONObj{std::move(sharedBuf)};
            } else {
                result = BSONObj{sbe::value::bitcastTo<const char*>(val)};
            }

            if constexpr (isBson) {
                *out = std::move(result);
            } else {
                *out = Document{std::move(result)};
            }
        } else {
            // The query is supposed to return an object.
            MONGO_UNREACHABLE;
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

template sbe::PlanState fetchNextImpl<BSONObj>(sbe::PlanStage* root,
                                               sbe::value::SlotAccessor* resultSlot,
                                               sbe::value::SlotAccessor* recordIdSlot,
                                               BSONObj* out,
                                               RecordId* dlOut,
                                               bool returnOwnedBson);

template sbe::PlanState fetchNextImpl<Document>(sbe::PlanStage* root,
                                                sbe::value::SlotAccessor* resultSlot,
                                                sbe::value::SlotAccessor* recordIdSlot,
                                                Document* out,
                                                RecordId* dlOut,
                                                bool returnOwnedBson);

// NOTE: We intentionally do not expose overload for the 'Document' type. The only interface to get
// result from plan in 'Document' type is to call 'PlanExecutorSBE::getNextDocument()'.
sbe::PlanState fetchNext(sbe::PlanStage* root,
                         sbe::value::SlotAccessor* resultSlot,
                         sbe::value::SlotAccessor* recordIdSlot,
                         BSONObj* out,
                         RecordId* dlOut,
                         bool returnOwnedBson) {
    return fetchNextImpl(root, resultSlot, recordIdSlot, out, dlOut, returnOwnedBson);
}
}  // namespace mongo
