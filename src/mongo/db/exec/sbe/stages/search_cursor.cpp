/**
 *    Copyright (C) 2023-present MongoDB, Inc.
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

#include "mongo/db/exec/sbe/stages/search_cursor.h"
#include "mongo/base/string_data.h"
#include "mongo/db/exec/docval_to_sbeval.h"
#include "mongo/db/exec/sbe/expressions/compile_ctx.h"
#include "mongo/db/exec/sbe/size_estimator.h"
#include "mongo/db/exec/sbe/values/value.h"
#include "mongo/util/str.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kQuery

namespace mongo::sbe {

SearchCursorStage::SearchCursorStage(NamespaceString nss,
                                     boost::optional<UUID> collUuid,
                                     boost::optional<value::SlotId> resultSlot,
                                     std::vector<std::string> metadataNames,
                                     value::SlotVector metadataSlots,
                                     std::vector<std::string> fieldNames,
                                     value::SlotVector fieldSlots,
                                     value::SlotId cursorIdSlot,
                                     value::SlotId firstBatchSlot,
                                     value::SlotId searchQuerySlot,
                                     boost::optional<value::SlotId> sortSpecSlot,
                                     value::SlotId limitSlot,
                                     value::SlotId protocolVersionSlot,
                                     boost::optional<ExplainOptions::Verbosity> explain,
                                     PlanYieldPolicy* yieldPolicy,
                                     PlanNodeId planNodeId)
    : PlanStage("search_cursor"_sd, yieldPolicy, planNodeId, false),
      _namespace(nss),
      _collUuid(collUuid),
      _resultSlot(resultSlot),
      _metadataNames(std::move(metadataNames)),
      _metadataSlots(std::move(metadataSlots)),
      _fieldNames(std::move(fieldNames)),
      _fieldSlots(std::move(fieldSlots)),
      _cursorIdSlot(cursorIdSlot),
      _firstBatchSlot(firstBatchSlot),
      _searchQuerySlot(searchQuerySlot),
      _sortSpecSlot(sortSpecSlot),
      _limitSlot(limitSlot),
      _protocolVersionSlot(protocolVersionSlot),
      _explain(explain) {
    _docsReturnedStats = getCommonStats();
}

std::unique_ptr<PlanStage> SearchCursorStage::clone() const {
    return std::make_unique<SearchCursorStage>(_namespace,
                                               _collUuid,
                                               _resultSlot,
                                               _metadataNames.getUnderlyingVector(),
                                               _metadataSlots,
                                               _fieldNames.getUnderlyingVector(),
                                               _fieldSlots,
                                               _cursorIdSlot,
                                               _firstBatchSlot,
                                               _searchQuerySlot,
                                               _sortSpecSlot,
                                               _limitSlot,
                                               _protocolVersionSlot,
                                               _explain,
                                               _yieldPolicy,
                                               _commonStats.nodeId);
}

namespace {
// Initialize the slot accessors and name to accessor mapping given the slots and names vector.
void initializeAccessorsVector(absl::InlinedVector<value::OwnedValueAccessor, 3>& accessors,
                               value::SlotAccessorMap& accessorsMap,
                               const IndexedStringVector& names,
                               const value::SlotVector& slotVec) {
    accessors.resize(names.size());
    for (size_t idx = 0; idx < names.size(); ++idx) {
        auto [it, inserted] = accessorsMap.emplace(slotVec[idx], &accessors[idx]);
        uassert(7816101, str::stream() << "duplicate slot: " << slotVec[idx], inserted);
    }
}

// Help debugPrinter to print an optional slot.
void addDebugOptionalSlotIdentifier(std::vector<DebugPrinter::Block>& ret,
                                    const boost::optional<value::SlotId>& slot) {
    if (slot) {
        DebugPrinter::addIdentifier(ret, slot.value());
    } else {
        DebugPrinter::addIdentifier(ret, DebugPrinter::kNoneKeyword);
    }
}

// Help debugPrinter to print a vector of slots.
void addDebugSlotVector(std::vector<DebugPrinter::Block>& ret, const value::SlotVector& slotVec) {
    ret.emplace_back(DebugPrinter::Block("[`"));
    for (size_t idx = 0; idx < slotVec.size(); ++idx) {
        if (idx) {
            ret.emplace_back(DebugPrinter::Block("`,"));
        }

        DebugPrinter::addIdentifier(ret, slotVec[idx]);
    }
    ret.emplace_back(DebugPrinter::Block("`]"));
}
}  // namespace

void SearchCursorStage::prepare(CompileCtx& ctx) {
    // Prepare accessors for the outputs of this stage.
    initializeAccessorsVector(
        _metadataAccessors, _metadataAccessorsMap, _metadataNames, _metadataSlots);
    initializeAccessorsVector(_fieldAccessors, _fieldAccessorsMap, _fieldNames, _fieldSlots);

    _limitAccessor = ctx.getAccessor(_limitSlot);
    _protocolVersionAccessor = ctx.getAccessor(_protocolVersionSlot);
    _cursorIdAccessor = ctx.getAccessor(_cursorIdSlot);
    _firstBatchAccessor = ctx.getAccessor(_firstBatchSlot);
    _searchQueryAccessor = ctx.getAccessor(_searchQuerySlot);
    if (_sortSpecSlot) {
        _sortSpecAccessor = ctx.getAccessor(*_sortSpecSlot);
    }
}

value::SlotAccessor* SearchCursorStage::getAccessor(CompileCtx& ctx, value::SlotId slot) {
    // Allow access to the outputs for this stage.
    if (_resultSlot && *_resultSlot == slot) {
        return &_resultAccessor;
    }

    if (auto it = _metadataAccessorsMap.find(slot); it != _metadataAccessorsMap.end()) {
        return it->second;
    }

    if (auto it = _fieldAccessorsMap.find(slot); it != _fieldAccessorsMap.end()) {
        return it->second;
    }
    return ctx.getAccessor(slot);
}

void SearchCursorStage::open(bool reOpen) {
    tassert(7816108, "SearchCursorStage can't be reOpened!", !reOpen);
    auto optTimer(getOptTimer(_opCtx));

    _commonStats.opens++;
    auto [limitTag, limitVal] = _limitAccessor->getViewOfValue();
    if (limitTag != value::TypeTags::Nothing) {
        tassert(7816106, "limit should be an integer", limitTag == value::TypeTags::NumberInt64);
        _limit = value::bitcastTo<uint64_t>(limitVal);
    }

    boost::optional<int> protocolVersion;
    auto [protocolVersionTag, protocolVersionVal] = _protocolVersionAccessor->getViewOfValue();
    if (protocolVersionTag != value::TypeTags::Nothing) {
        tassert(7816105,
                "protocolVersion should be an integer",
                protocolVersionTag == value::TypeTags::NumberInt32);
        protocolVersion = value::bitcastTo<int>(protocolVersionVal);
    }

    std::vector<BSONObj> batchVec;
    auto [batchTag, batchVal] = _firstBatchAccessor->getViewOfValue();
    tassert(7816109, "First batch should be a BSONArray", batchTag == value::TypeTags::bsonArray);
    value::ArrayEnumerator enumerator{batchTag, batchVal};
    while (!enumerator.atEnd()) {
        auto [tag, value] = enumerator.getViewOfValue();
        auto be = value::bitcastTo<const char*>(value);
        batchVec.push_back(BSONObj(be).getOwned());
        enumerator.advance();
    }

    auto [cursorIdTag, cursorIdVal] = _cursorIdAccessor->getViewOfValue();
    tassert(7816104,
            "Cursor ID should be an integer",
            cursorIdTag == value::TypeTags::NumberInt32 ||
                cursorIdTag == value::TypeTags::NumberInt64);
    auto cursorId = value::bitcastTo<CursorId>(cursorIdVal);

    auto [queryTag, queryVal] = _searchQueryAccessor->getViewOfValue();
    _searchQuery = BSONObj(value::bitcastTo<char*>(queryVal));

    // Establish the cursor that comes from cursorIdSlot.
    tassert(7816103, "Cursor should not be established yet", !_cursor);
    CursorResponse resp = CursorResponse(_namespace, cursorId, std::move(batchVec));
    _cursor.emplace(getSearchHelpers(_opCtx->getServiceContext())
                        ->establishSearchCursor(
                            _opCtx,
                            _namespace,
                            _collUuid,
                            _explain,
                            _searchQuery,
                            std::move(resp),
                            _limit != 0 ? boost::optional<long long>(_limit) : boost::none,
                            [this] { return calcDocsNeeded(); },
                            protocolVersion)
                        .value());
    tassert(7816111, "Establishing the cursor should yield a non-null value.", _cursor.has_value());
    _isStoredSource = _searchQuery.getBoolField(kReturnStoredSourceArg);
}

bool SearchCursorStage::shouldReturnEOF() {
    if (MONGO_unlikely(searchReturnEofImmediately.shouldFail())) {
        return true;
    }

    if (_isStoredSource && _limit != 0 && _commonStats.advances >= _limit) {
        return true;
    }

    // Return EOF if pExpCtx->uuid is unset here; the collection we are searching over has not been
    // created yet.
    if (!_collUuid) {
        return true;
    }

    if (_explain) {
        // TODO SERVER-79267: Explain response for search cursor stage
        return true;
    }

    return false;
}

PlanState SearchCursorStage::getNext() {
    auto optTimer(getOptTimer(_opCtx));
    if (shouldReturnEOF()) {
        return trackPlanState(PlanState::IS_EOF);
    }

    _opCtx->checkForInterrupt();

    // Get BSONObj response from Mongot
    _response = _cursor->getNext(_opCtx);
    auto& opDebug = CurOp::get(_opCtx)->debug();

    // TODO: SERVER-80114 should we move this to SearchStats?
    if (opDebug.msWaitingForMongot) {
        *opDebug.msWaitingForMongot += durationCount<Milliseconds>(_cursor->resetWaitingTime());
    } else {
        opDebug.msWaitingForMongot = durationCount<Milliseconds>(_cursor->resetWaitingTime());
    }
    opDebug.mongotBatchNum = _cursor->getBatchNum();
    opDebug.mongotCursorId = _cursor->getCursorId();

    // If there's no response, return EOF
    if (!_response) {
        return trackPlanState(PlanState::IS_EOF);
    }

    // Put results/values in slots and advance plan state
    for (auto& accessor : _metadataAccessors) {
        accessor.reset();
    }

    for (auto& accessor : _fieldAccessors) {
        accessor.reset();
    }

    for (auto& elem : *_response) {

        auto elemName = elem.fieldNameStringData();
        if (size_t pos = _metadataNames.findPos(elemName); pos != IndexedStringVector::npos) {
            auto [tag, val] = bson::convertFrom<true>(elem);
            _metadataAccessors[pos].reset(false, tag, val);
        }
        if (!_isStoredSource) {
            if (size_t pos = _fieldNames.findPos(elemName); pos != IndexedStringVector::npos) {
                auto [tag, val] = bson::convertFrom<true>(elem);
                _fieldAccessors[pos].reset(false, tag, val);
            }
        }
    }

    if (_resultSlot || _isStoredSource) {
        // Remove all metadata fields from response.
        _resultObj = Document::fromBsonWithMetaData(_response.value()).toBson();
        if (_isStoredSource) {
            uassert(7856301,
                    "StoredSource field must exist in mongot response.",
                    _resultObj->hasField("storedSource"));
            _resultObj = _resultObj->getObjectField("storedSource").getOwned();

            for (auto& elem : *_resultObj) {
                auto elemName = elem.fieldNameStringData();
                if (size_t pos = _fieldNames.findPos(elemName); pos != IndexedStringVector::npos) {
                    auto [tag, val] = bson::convertFrom<true>(elem);
                    _fieldAccessors[pos].reset(false, tag, val);
                }
            }
        }
        if (_resultSlot) {
            _resultAccessor.reset(false,
                                  value::TypeTags::bsonObject,
                                  value::bitcastFrom<const char*>(_resultObj->objdata()));
        }
    }
    return trackPlanState(PlanState::ADVANCED);
}

boost::optional<long long> SearchCursorStage::calcDocsNeeded() {
    if (_limit == 0) {
        return boost::none;
    }
    // The return value will start at _limit and will decrease by one for each document that gets
    // returned. If a document gets filtered out, _docsReturnedStats->advances will not change and
    // so docsNeeded will stay the same. In the stored source case, _docsReturnedStats ptr points to
    // current stage, otherwise _docsReturnedStats points to the idx scan stage.
    // TODO: SERVER-80648 to have a better way to track count of idx scan stage.
    return _limit - _docsReturnedStats->advances;
}

void SearchCursorStage::close() {
    auto optTimer(getOptTimer(_opCtx));

    trackClose();
    // Reset all the extraneous members that aren't needed after getNext() is done
    _cursor.reset();
}

std::unique_ptr<PlanStageStats> SearchCursorStage::getStats(bool includeDebugInfo) const {
    auto ret = std::make_unique<PlanStageStats>(_commonStats);

    // Add stats information for all the slots as well as specific stats
    ret->specific = std::make_unique<SearchStats>(_specificStats);

    if (includeDebugInfo) {
        BSONObjBuilder bob;
        if (_resultSlot) {
            bob.appendNumber("resultSlot", static_cast<long long>(*_resultSlot));
        }

        bob.append("metadataNames", _metadataNames.getUnderlyingVector());
        bob.append("metadataSlots", _metadataSlots.begin(), _metadataSlots.end());

        bob.append("fieldNames", _fieldNames.getUnderlyingVector());
        bob.append("fieldSlots", _fieldSlots.begin(), _fieldSlots.end());

        bob.appendNumber("cursorIdSlot", static_cast<long long>(_cursorIdSlot));
        bob.appendNumber("firstBatchSlot", static_cast<long long>(_firstBatchSlot));
        bob.appendNumber("limitSlot", static_cast<long long>(_limitSlot));

        ret->debugInfo = bob.obj();
    }

    return ret;
}

const SpecificStats* SearchCursorStage::getSpecificStats() const {
    return &_specificStats;
}

std::vector<DebugPrinter::Block> SearchCursorStage::debugPrint() const {
    auto ret = PlanStage::debugPrint();

    addDebugOptionalSlotIdentifier(ret, _resultSlot);

    addDebugSlotVector(ret, _metadataSlots);
    addDebugSlotVector(ret, _fieldSlots);

    DebugPrinter::addIdentifier(ret, _cursorIdSlot);
    DebugPrinter::addIdentifier(ret, _firstBatchSlot);
    DebugPrinter::addIdentifier(ret, _limitSlot);

    return ret;
}

size_t SearchCursorStage::estimateCompileTimeSize() const {
    size_t size = sizeof(*this);
    size += size_estimator::estimate(_children);
    size += size_estimator::estimate(_metadataSlots);
    size += size_estimator::estimate(_fieldSlots);
    size += size_estimator::estimate(_specificStats);
    return size;
}

}  // namespace mongo::sbe
