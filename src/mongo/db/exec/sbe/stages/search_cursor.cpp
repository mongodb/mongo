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

std::unique_ptr<SearchCursorStage> SearchCursorStage::createForStoredSource(
    NamespaceString nss,
    boost::optional<UUID> collUuid,
    boost::optional<value::SlotId> resultSlot,
    std::vector<std::string> metadataNames,
    value::SlotVector metadataSlots,
    std::vector<std::string> fieldNames,
    value::SlotVector fieldSlots,
    size_t remoteCursorId,
    boost::optional<value::SlotId> sortSpecSlot,
    boost::optional<value::SlotId> limitSlot,
    boost::optional<value::SlotId> sortKeySlot,
    boost::optional<value::SlotId> collatorSlot,
    PlanYieldPolicy* yieldPolicy,
    PlanNodeId planNodeId) {
    return std::unique_ptr<SearchCursorStage>(new SearchCursorStage(nss,
                                                                    collUuid,
                                                                    boost::none /* idSlot */,
                                                                    resultSlot,
                                                                    metadataNames,
                                                                    metadataSlots,
                                                                    fieldNames,
                                                                    fieldSlots,
                                                                    remoteCursorId,
                                                                    true /* isStoredSource */,
                                                                    sortSpecSlot,
                                                                    limitSlot,
                                                                    sortKeySlot,
                                                                    collatorSlot,
                                                                    yieldPolicy,
                                                                    planNodeId));
}

std::unique_ptr<SearchCursorStage> SearchCursorStage::createForNonStoredSource(
    NamespaceString nss,
    boost::optional<UUID> collUuid,
    boost::optional<value::SlotId> idSlot,
    std::vector<std::string> metadataNames,
    value::SlotVector metadataSlots,
    size_t remoteCursorId,
    boost::optional<value::SlotId> sortSpecSlot,
    boost::optional<value::SlotId> limitSlot,
    boost::optional<value::SlotId> sortKeySlot,
    boost::optional<value::SlotId> collatorSlot,
    PlanYieldPolicy* yieldPolicy,
    PlanNodeId planNodeId) {
    return std::unique_ptr<SearchCursorStage>(
        new SearchCursorStage(nss,
                              collUuid,
                              idSlot,
                              boost::none /* resultSlot */,
                              metadataNames,
                              metadataSlots,
                              std::vector<std::string>() /* fieldNames */,
                              sbe::makeSV() /* fieldSlots */,
                              remoteCursorId,
                              false /* isStoredSource */,
                              sortSpecSlot,
                              limitSlot,
                              sortKeySlot,
                              collatorSlot,
                              yieldPolicy,
                              planNodeId));
}

std::unique_ptr<SearchCursorStage> SearchCursorStage::createForMetadata(
    NamespaceString nss,
    boost::optional<UUID> collUuid,
    boost::optional<value::SlotId> resultSlot,
    size_t remoteCursorId,
    PlanYieldPolicy* yieldPolicy,
    PlanNodeId planNodeId) {
    return std::unique_ptr<SearchCursorStage>(
        new SearchCursorStage(nss,
                              collUuid,
                              boost::none /* idSlot */,
                              resultSlot,
                              std::vector<std::string>() /* metadataNames */,
                              sbe::makeSV() /* metadataSlots */,
                              std::vector<std::string>() /* fieldNames */,
                              sbe::makeSV() /* fieldSlots */,
                              remoteCursorId,
                              false /* isStoredSource */,
                              boost::none /* sortSpecSlot */,
                              boost::none /* limitSlot */,
                              boost::none /* sortKeySlot */,
                              boost::none /* collatorSlot */,
                              yieldPolicy,
                              planNodeId));
}

SearchCursorStage::SearchCursorStage(NamespaceString nss,
                                     boost::optional<UUID> collUuid,
                                     boost::optional<value::SlotId> idSlot,
                                     boost::optional<value::SlotId> resultSlot,
                                     std::vector<std::string> metadataNames,
                                     value::SlotVector metadataSlots,
                                     std::vector<std::string> fieldNames,
                                     value::SlotVector fieldSlots,
                                     size_t remoteCursorId,
                                     bool isStoredSource,
                                     boost::optional<value::SlotId> sortSpecSlot,
                                     boost::optional<value::SlotId> limitSlot,
                                     boost::optional<value::SlotId> sortKeySlot,
                                     boost::optional<value::SlotId> collatorSlot,
                                     PlanYieldPolicy* yieldPolicy,
                                     PlanNodeId planNodeId)
    : PlanStage("search_cursor"_sd, yieldPolicy, planNodeId, false),
      _namespace(nss),
      _collUuid(collUuid),
      _idSlot(idSlot),
      _resultSlot(resultSlot),
      _metadataNames(std::move(metadataNames)),
      _metadataSlots(std::move(metadataSlots)),
      _fieldNames(std::move(fieldNames)),
      _fieldSlots(std::move(fieldSlots)),
      _remoteCursorId(remoteCursorId),
      _isStoredSource(isStoredSource),
      _sortSpecSlot(sortSpecSlot),
      _limitSlot(limitSlot),
      _sortKeySlot(sortKeySlot),
      _collatorSlot(collatorSlot) {}

std::unique_ptr<PlanStage> SearchCursorStage::clone() const {
    return std::unique_ptr<SearchCursorStage>(
        new SearchCursorStage(_namespace,
                              _collUuid,
                              _idSlot,
                              _resultSlot,
                              _metadataNames.getUnderlyingVector(),
                              _metadataSlots,
                              _fieldNames.getUnderlyingVector(),
                              _fieldSlots,
                              _remoteCursorId,
                              _isStoredSource,
                              _sortSpecSlot,
                              _limitSlot,
                              _sortKeySlot,
                              _collatorSlot,
                              _yieldPolicy,
                              _commonStats.nodeId));
}

namespace {
// Initialize the slot accessors and name to accessor mapping given the slots and names vector.
void initializeAccessorsVector(absl::InlinedVector<value::OwnedValueAccessor, 3>& accessors,
                               value::SlotAccessorMap& accessorsMap,
                               const StringListSet& names,
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

    if (_limitSlot) {
        _limitAccessor = ctx.getAccessor(*_limitSlot);
    }
    if (_sortSpecSlot) {
        _sortSpecAccessor = ctx.getAccessor(*_sortSpecSlot);
    }
    if (_collatorSlot) {
        _collatorAccessor = ctx.getAccessor(*_collatorSlot);
    }

    if (ctx.remoteCursors) {
        tassert(7816103,
                "RemoteCursors must be established",
                ctx.remoteCursors->count(_remoteCursorId));
        _cursor = ctx.remoteCursors->at(_remoteCursorId).get();
        _cursorId = _cursor->getCursorId();
    }
}

value::SlotAccessor* SearchCursorStage::getAccessor(CompileCtx& ctx, value::SlotId slot) {
    // Allow access to the outputs for this stage.
    if (_idSlot && *_idSlot == slot) {
        return &_idAccessor;
    }

    if (_resultSlot && *_resultSlot == slot) {
        return &_resultAccessor;
    }

    if (_sortKeySlot && *_sortKeySlot == slot) {
        return &_sortKeyAccessor;
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

    if (_limitAccessor) {
        auto [limitTag, limitVal] = _limitAccessor->getViewOfValue();
        if (limitTag != value::TypeTags::Nothing) {
            tassert(
                7816106, "limit should be an integer", limitTag == value::TypeTags::NumberInt64);
            _limit = value::bitcastTo<uint64_t>(limitVal);
        }
    }

    if (_cursor && _limit != 0) {
        _cursor->updateGetMoreFunc(
            search_helpers::buildSearchGetMoreFunc([this] { return calcDocsNeeded(); }));
    }

    if (_sortSpecAccessor) {
        auto [sortTag, sortVal] = _sortSpecAccessor->getViewOfValue();
        if (sortTag != value::TypeTags::Nothing) {
            tassert(
                7856005, "Incorrect search sort spec type", sortTag == value::TypeTags::sortSpec);
            auto sortSpec = value::bitcastTo<SortSpec*>(sortVal);
            const CollatorInterface* collatorView = nullptr;
            if (_collatorAccessor) {
                auto [collatorTag, collatorVal] = _collatorAccessor->getViewOfValue();
                uassert(7856006,
                        "collatorSlot must be of collator type",
                        collatorTag == value::TypeTags::collator ||
                            collatorTag == value::TypeTags::Nothing);
                if (collatorTag == value::TypeTags::collator) {
                    collatorView = value::getCollatorView(collatorVal);
                }
            }
            _sortKeyGen.emplace(sortSpec->getSortPattern(), collatorView);
        }
    }
}

bool SearchCursorStage::shouldReturnEOF() {
    if (MONGO_unlikely(searchReturnEofImmediately.shouldFail())) {
        return true;
    }

    if (_isStoredSource && _limit != 0 && _commonStats.advances >= _limit) {
        return true;
    }

    // Return EOF if uuid is unset here; the collection we are searching over has not been created
    // yet.
    if (!_collUuid || !_cursor) {
        return true;
    }
    return false;
}

PlanState SearchCursorStage::getNext() {
    auto optTimer(getOptTimer(_opCtx));
    checkForInterruptAndYield(_opCtx);

    if (shouldReturnEOF()) {
        return trackPlanState(PlanState::IS_EOF);
    }

    auto state = doGetNext();

    // Update search stats.
    _specificStats.msWaitingForMongot += durationCount<Milliseconds>(_cursor->resetWaitingTime());
    _specificStats.batchNum = _cursor->getBatchNum();
    // Update opDebug log as well so that these are logged in "Slow query" log for every getMore
    // command.
    auto& opDebug = CurOp::get(_opCtx)->debug();
    opDebug.mongotCursorId = _cursorId;
    opDebug.msWaitingForMongot = _specificStats.msWaitingForMongot;
    opDebug.mongotBatchNum = _specificStats.batchNum;

    return trackPlanState(state);
}

PlanState SearchCursorStage::doGetNext() {
    for (;;) {
        // Get BSONObj response from Mongot
        _response = _cursor->getNext(_opCtx);

        // If there's no response, return EOF
        if (!_response) {
            return PlanState::IS_EOF;
        }

        // Put results/values in slots and advance plan state
        for (auto& accessor : _metadataAccessors) {
            accessor.reset();
        }

        for (auto& accessor : _fieldAccessors) {
            accessor.reset();
        }

        _idAccessor.reset();

        for (auto& elem : *_response) {

            auto elemName = elem.fieldNameStringData();
            if (size_t pos = _metadataNames.findPos(elemName); pos != StringListSet::npos) {
                auto [tag, val] = bson::convertFrom<true>(elem);
                _metadataAccessors[pos].reset(false, tag, val);
            }
            if (_idSlot && elemName == "_id") {
                auto [tag, val] = bson::convertFrom<true>(elem);
                _idAccessor.reset(false, tag, val);
            }
        }

        if (!_isStoredSource && _idSlot &&
            _idAccessor.getViewOfValue().first == value::TypeTags::Nothing) {
            // For non-storedSource case, document without _id field is not valid.
            continue;
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
                    if (size_t pos = _fieldNames.findPos(elemName); pos != StringListSet::npos) {
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

        if (_sortKeySlot) {
            _sortKeyAccessor.reset();
            if (_sortSpecSlot && _sortKeyGen) {
                auto sortKey = _sortKeyGen->computeSortKeyFromDocument(Document(*_response));
                auto [tag, val] = value::makeValue(sortKey);
                _sortKeyAccessor.reset(tag, val);
            } else if (_response->hasField(Document::metaFieldSearchScore)) {
                // If this stage is getting metadata documents from mongot, those don't include
                // searchScore.
                _sortKeyAccessor.reset(
                    value::TypeTags::NumberDouble,
                    value::bitcastFrom<double>(
                        _response->getField(Document::metaFieldSearchScore).number()));
            }
        }

        return PlanState::ADVANCED;
    }
}

boost::optional<long long> SearchCursorStage::calcDocsNeeded() {
    if (_limit == 0) {
        return boost::none;
    }
    // For the stored source case, we know exact how many documents we returned so we can calculate
    // the correct number we still need; for non stored source case, we don't know the correct
    // number because the _id returned may not be valid.
    return _isStoredSource ? _limit - _commonStats.advances : _limit;
}

void SearchCursorStage::close() {
    auto optTimer(getOptTimer(_opCtx));
    trackClose();
    _cursor = nullptr;
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
        if (_idSlot) {
            bob.appendNumber("idSlot", static_cast<long long>(*_idSlot));
        }

        bob.append("metadataNames", _metadataNames.getUnderlyingVector());
        bob.append("metadataSlots", _metadataSlots.begin(), _metadataSlots.end());

        bob.append("fieldNames", _fieldNames.getUnderlyingVector());
        bob.append("fieldSlots", _fieldSlots.begin(), _fieldSlots.end());

        bob.appendNumber("remoteCursorId", static_cast<long long>(_remoteCursorId));
        bob.appendBool("isStoredSource", _isStoredSource);

        if (_sortSpecSlot) {
            bob.appendNumber("sortSpecSlot", static_cast<long long>(*_sortSpecSlot));
        }
        if (_limitSlot) {
            bob.appendNumber("limitSlot", static_cast<long long>(*_limitSlot));
        }
        if (_sortKeySlot) {
            bob.appendNumber("sortKeySlot", static_cast<long long>(*_sortKeySlot));
        }
        if (_collatorSlot) {
            bob.appendNumber("collatorSlot", static_cast<long long>(*_collatorSlot));
        }

        // Specific stats.
        bob.appendBool("msWaitingForMongot", _specificStats.msWaitingForMongot);
        bob.appendNumber("batchNum", _specificStats.batchNum);

        ret->debugInfo = bob.obj();
    }

    return ret;
}

const SpecificStats* SearchCursorStage::getSpecificStats() const {
    return &_specificStats;
}

std::vector<DebugPrinter::Block> SearchCursorStage::debugPrint() const {
    auto ret = PlanStage::debugPrint();

    addDebugOptionalSlotIdentifier(ret, _idSlot);
    addDebugOptionalSlotIdentifier(ret, _resultSlot);

    addDebugSlotVector(ret, _metadataSlots);
    addDebugSlotVector(ret, _fieldSlots);

    ret.emplace_back(std::to_string(_remoteCursorId));
    ret.emplace_back(_isStoredSource ? "true" : "false");
    addDebugOptionalSlotIdentifier(ret, _sortSpecSlot);
    addDebugOptionalSlotIdentifier(ret, _limitSlot);
    addDebugOptionalSlotIdentifier(ret, _sortKeySlot);
    addDebugOptionalSlotIdentifier(ret, _collatorSlot);

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
