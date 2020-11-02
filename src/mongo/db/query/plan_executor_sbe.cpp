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

namespace mongo {
PlanExecutorSBE::PlanExecutorSBE(OperationContext* opCtx,
                                 std::unique_ptr<CanonicalQuery> cq,
                                 sbe::CandidatePlans candidates,
                                 const CollectionPtr& collection,
                                 bool returnOwnedBson,
                                 NamespaceString nss,
                                 bool isOpen,
                                 std::unique_ptr<PlanYieldPolicySBE> yieldPolicy)
    : _state{isOpen ? State::kOpened : State::kClosed},
      _opCtx(opCtx),
      _nss(std::move(nss)),
      _mustReturnOwnedBson(returnOwnedBson),
      _env{candidates.winner().data.env},
      _ctx{std::move(candidates.winner().data.ctx)},
      _root{std::move(candidates.winner().root)},
      _cq{std::move(cq)},
      _yieldPolicy(std::move(yieldPolicy)) {
    invariant(!_nss.isEmpty());
    invariant(_root);

    auto winner = std::move(candidates.plans[candidates.winnerIdx]);
    _solution = std::move(winner.solution);

    if (auto slot = winner.data.outputs.getIfExists(stage_builder::PlanStageSlots::kResult); slot) {
        _result = _root->getAccessor(_ctx, *slot);
        uassert(4822865, "Query does not have result slot.", _result);
    }

    if (auto slot = winner.data.outputs.getIfExists(stage_builder::PlanStageSlots::kRecordId);
        slot) {
        _resultRecordId = _root->getAccessor(_ctx, *slot);
        uassert(4822866, "Query does not have recordId slot.", _resultRecordId);
    }

    if (auto slot = winner.data.outputs.getIfExists(stage_builder::PlanStageSlots::kOplogTs);
        slot) {
        _oplogTs = _root->getAccessor(_ctx, *slot);
        uassert(4822867, "Query does not have oplogTs slot.", _oplogTs);
    }

    if (winner.data.shouldUseTailableScan) {
        _resumeRecordIdSlot = _env->getSlot("resumeRecordId"_sd);
    }

    _shouldTrackLatestOplogTimestamp = winner.data.shouldTrackLatestOplogTimestamp;
    _shouldTrackResumeToken = winner.data.shouldTrackResumeToken;

    if (!winner.results.empty()) {
        _stash = std::move(winner.results);
    }

    // Callers are allowed to disable yielding for this plan by passing a null yield policy.
    if (_yieldPolicy) {
        // Clear any formerly registered plans and register '_root' to yield. This is needed because
        // multiple candidate plans may have been registered during runtime planning, before the
        // PlanExecutor was created. All but one candidate plan ('_root') have since been discarded.
        _yieldPolicy->clearRegisteredPlans();
        _yieldPolicy->registerPlan(_root.get());
    }

    const auto isMultiPlan = candidates.plans.size() > 0;
    if (!_cq->getExpCtx()->explain) {
        // If we're not in explain mode, there is no need to keep rejected candidate plans around.
        candidates.plans.clear();
    } else {
        // Keep only rejected candidate plans.
        candidates.plans.erase(candidates.plans.begin() + candidates.winnerIdx);
    }
    _planExplainer = plan_explainer_factory::make(
        _root.get(), _solution.get(), std::move(candidates.plans), isMultiPlan);
}

void PlanExecutorSBE::saveState() {
    _root->saveState();
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

void PlanExecutorSBE::enqueue(const BSONObj& obj) {
    invariant(_state == State::kOpened);
    invariant(!_isDisposed);
    _stash.push({obj.getOwned(), boost::none});
}

PlanExecutor::ExecState PlanExecutorSBE::getNextDocument(Document* objOut, RecordId* dlOut) {
    invariant(!_isDisposed);

    BSONObj obj;
    auto result = getNext(&obj, dlOut);
    if (result == PlanExecutor::ExecState::ADVANCED) {
        *objOut = Document{std::move(obj)};
    }
    return result;
}

PlanExecutor::ExecState PlanExecutorSBE::getNext(BSONObj* out, RecordId* dlOut) {
    invariant(!_isDisposed);

    if (!_stash.empty()) {
        auto&& [doc, recordId] = _stash.front();
        *out = std::move(doc);
        if (dlOut && recordId) {
            *dlOut = *recordId;
        }
        _stash.pop();
        return PlanExecutor::ExecState::ADVANCED;
    } else if (_root->getCommonStats()->isEOF) {
        // If we had stashed elements and consumed them all, but the PlanStage has also
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
                invariant(_resultRecordId);

                auto [tag, val] = _resultRecordId->getViewOfValue();
                uassert(4946306,
                        "Collection scan was asked to track resume token, but found a result "
                        "without a valid RecordId",
                        tag == sbe::value::TypeTags::RecordId ||
                            tag == sbe::value::TypeTags::Nothing);
                _env->resetSlot(*_resumeRecordIdSlot, tag, val, false);
            }

            _state = State::kOpened;
            _root->open(false);
        }

        invariant(_state == State::kOpened);

        auto result =
            fetchNext(_root.get(), _result, _resultRecordId, out, dlOut, _mustReturnOwnedBson);
        if (result == sbe::PlanState::IS_EOF) {
            _root->close();
            _state = State::kClosed;

            if (!insert_listener::shouldWaitForInserts(_opCtx, _cq.get(), _yieldPolicy.get())) {
                return PlanExecutor::ExecState::IS_EOF;
            }

            insert_listener::waitForInserts(_opCtx, _yieldPolicy.get(), &cappedInsertNotifierData);
            // There may be more results, keep going.
            continue;
        }

        invariant(result == sbe::PlanState::ADVANCED);
        return PlanExecutor::ExecState::ADVANCED;
    }
}

Timestamp PlanExecutorSBE::getLatestOplogTimestamp() const {
    if (_shouldTrackLatestOplogTimestamp) {
        invariant(_oplogTs);

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
    if (_shouldTrackResumeToken) {
        invariant(_resultRecordId);

        auto [tag, val] = _resultRecordId->getViewOfValue();
        if (tag != sbe::value::TypeTags::Nothing) {
            const auto msgTag = tag;
            uassert(4822869,
                    str::stream() << "Collection scan was asked to track resume token, "
                                     "but found a result without a valid RecordId: "
                                  << msgTag,
                    tag == sbe::value::TypeTags::RecordId);
            return BSON("$recordId" << sbe::value::bitcastTo<int64_t>(val));
        }
    }
    return {};
}

sbe::PlanState fetchNext(sbe::PlanStage* root,
                         sbe::value::SlotAccessor* resultSlot,
                         sbe::value::SlotAccessor* recordIdSlot,
                         BSONObj* out,
                         RecordId* dlOut,
                         bool returnOwnedBson) {
    invariant(out);

    auto state = root->getNext();
    if (state == sbe::PlanState::IS_EOF) {
        return state;
    }
    invariant(state == sbe::PlanState::ADVANCED);

    if (resultSlot) {
        auto [tag, val] = resultSlot->getViewOfValue();
        if (tag == sbe::value::TypeTags::Object) {
            BSONObjBuilder bb;
            sbe::bson::convertToBsonObj(bb, sbe::value::getObjectView(val));
            *out = bb.obj();
        } else if (tag == sbe::value::TypeTags::bsonObject) {
            if (returnOwnedBson) {
                auto [ownedTag, ownedVal] = resultSlot->copyOrMoveValue();
                auto sharedBuf =
                    SharedBuffer(UniqueBuffer::reclaim(sbe::value::bitcastTo<char*>(ownedVal)));
                *out = BSONObj(std::move(sharedBuf));
            } else {
                *out = BSONObj(sbe::value::bitcastTo<const char*>(val));
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
            *dlOut = RecordId{sbe::value::bitcastTo<int64_t>(val)};
        }
    }
    return state;
}

}  // namespace mongo
