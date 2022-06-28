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

#include "mongo/db/exec/sbe/size_estimator.h"
#include "mongo/db/exec/sbe/stages/traverse.h"

namespace mongo::sbe {
TraverseStage::TraverseStage(std::unique_ptr<PlanStage> outer,
                             std::unique_ptr<PlanStage> inner,
                             value::SlotId inField,
                             value::SlotId outField,
                             value::SlotId outFieldInner,
                             value::SlotVector outerCorrelated,
                             std::unique_ptr<EExpression> foldExpr,
                             std::unique_ptr<EExpression> finalExpr,
                             PlanNodeId planNodeId,
                             boost::optional<size_t> nestedArraysDepth,
                             bool participateInTrialRunTracking)
    : PlanStage("traverse"_sd, planNodeId, participateInTrialRunTracking),
      _inField(inField),
      _outField(outField),
      _outFieldInner(outFieldInner),
      _correlatedSlots(std::move(outerCorrelated)),
      _fold(std::move(foldExpr)),
      _final(std::move(finalExpr)),
      _nestedArraysDepth(nestedArraysDepth) {
    _children.emplace_back(std::move(outer));
    _children.emplace_back(std::move(inner));

    if (_inField == _outField && (_fold || _final)) {
        uasserted(4822808, "in and out field must not match when folding");
    }
}

std::unique_ptr<PlanStage> TraverseStage::clone() const {
    return std::make_unique<TraverseStage>(_children[0]->clone(),
                                           _children[1]->clone(),
                                           _inField,
                                           _outField,
                                           _outFieldInner,
                                           _correlatedSlots,
                                           _fold ? _fold->clone() : nullptr,
                                           _final ? _final->clone() : nullptr,
                                           _commonStats.nodeId,
                                           _nestedArraysDepth,
                                           _participateInTrialRunTracking);
}

void TraverseStage::prepare(CompileCtx& ctx) {
    // Prepare the outer side as usual.
    _children[0]->prepare(ctx);

    // Get the inField (incoming) accessor.
    _inFieldAccessor = _children[0]->getAccessor(ctx, _inField);

    // Prepare the accessor for the correlated parameter.
    ctx.pushCorrelated(_inField, &_correlatedAccessor);
    for (auto slot : _correlatedSlots) {
        ctx.pushCorrelated(slot, _children[0]->getAccessor(ctx, slot));
    }
    // Prepare the inner side.
    _children[1]->prepare(ctx);

    // Get the output from the inner side.
    _outFieldInputAccessor = _children[1]->getAccessor(ctx, _outFieldInner);

    if (_fold) {
        ctx.root = this;
        _foldCode = _fold->compile(ctx);
    }

    if (_final) {
        ctx.root = this;
        _finalCode = _final->compile(ctx);
    }

    // Restore correlated parameters.
    for (size_t idx = 0; idx < _correlatedSlots.size(); ++idx) {
        ctx.popCorrelated();
    }
    ctx.popCorrelated();

    _compiled = true;
}

value::SlotAccessor* TraverseStage::getAccessor(CompileCtx& ctx, value::SlotId slot) {
    if (_outField == slot) {
        return &_outFieldOutputAccessor;
    }

    if (_compiled) {
        // After the compilation pass to the 'outer' child.
        return _children[0]->getAccessor(ctx, slot);
    } else {
        // If internal expressions (fold, final) are not compiled yet then they refer to the 'inner'
        // child.
        return _children[1]->getAccessor(ctx, slot);
    }
}

void TraverseStage::open(bool reOpen) {
    auto optTimer(getOptTimer(_opCtx));

    _commonStats.opens++;
    _children[0]->open(reOpen);
    // Do not open the inner child as we do not have values of correlated parameters yet.
    // The values are available only after we call getNext on the outer side.
}

void TraverseStage::openInner(value::TypeTags tag, value::Value val) {
    // Set the correlated value.
    _correlatedAccessor.reset(tag, val);

    // And (re)open the inner side as it can see the correlated value now.
    _children[1]->open(_reOpenInner);
    _reOpenInner = true;
}

PlanState TraverseStage::getNext() {
    auto optTimer(getOptTimer(_opCtx));

    auto state = getNextOuterSide();
    if (state != PlanState::ADVANCED) {
        return trackPlanState(state);
    }
    [[maybe_unused]] auto earlyExit = traverse(_inFieldAccessor, &_outFieldOutputAccessor, 0);

    return trackPlanState(PlanState::ADVANCED);
}

bool TraverseStage::traverse(value::SlotAccessor* inFieldAccessor,
                             value::OwnedValueAccessor* outFieldOutputAccessor,
                             size_t level) {
    auto earlyExit = false;
    // Get the value.
    auto [tag, val] = inFieldAccessor->getViewOfValue();

    if (value::isArray(tag)) {
        // If it is an array then we have to traverse it.
        auto& inArrayAccessor = _inArrayAccessors.emplace_back(value::ArrayAccessor{});
        inArrayAccessor.reset(inFieldAccessor);
        value::Array* arrOut{nullptr};

        if (!_foldCode) {
            // Create a fresh new output array.
            // TODO if _inField == _outField then we can do implace update of the input array.
            auto [tag, val] = value::makeNewArray();
            arrOut = value::getArrayView(val);
            outFieldOutputAccessor->reset(true, tag, val);
        } else {
            outFieldOutputAccessor->reset(false, value::TypeTags::Nothing, 0);
        }

        if (level == 0 && inArrayAccessor.atEnd()) {
            // If we are "traversing" an empty array then the inner side of the traverse stage is
            // not entered as we do not have any value to process there. However, the inner side
            // holds now "stale" state from the previous traversal and we must not access it if the
            // query yields.
            _children[1]->disableSlotAccess(true);
        }

        // Loop over all elements of array.
        bool firstValue = true;
        for (; !inArrayAccessor.atEnd(); inArrayAccessor.advance()) {
            auto [tag, val] = inArrayAccessor.getViewOfValue();

            if (value::isArray(tag) && (!_nestedArraysDepth || level + 1 < *_nestedArraysDepth)) {

                // If the current array element is an array itself, traverse it recursively.
                value::OwnedValueAccessor outArrayAccessor;
                earlyExit = traverse(&inArrayAccessor, &outArrayAccessor, level + 1);
                auto [tag, val] = outArrayAccessor.copyOrMoveValue();

                if (!_foldCode) {
                    arrOut->push_back(tag, val);
                } else {
                    outFieldOutputAccessor->reset(true, tag, val);
                    if (earlyExit) {
                        break;
                    }
                }
            } else {
                // Otherwise, execute inner side once for every element of the array.
                openInner(tag, val);
                auto state = _children[1]->getNext();

                if (state == PlanState::ADVANCED) {
                    if (!_foldCode) {
                        // We have to copy (or move optimization) the value to the array
                        // as by definition all composite values (arrays, objects) own their
                        // constituents.
                        auto [tag, val] = _outFieldInputAccessor->copyOrMoveValue();
                        arrOut->push_back(tag, val);
                    } else {
                        if (firstValue) {
                            auto [tag, val] = _outFieldInputAccessor->copyOrMoveValue();
                            outFieldOutputAccessor->reset(true, tag, val);
                            firstValue = false;
                        } else {
                            // Fold
                            auto [owned, tag, val] = _bytecode.run(_foldCode.get());
                            if (!owned) {
                                auto [copyTag, copyVal] = value::copyValue(tag, val);
                                tag = copyTag;
                                val = copyVal;
                            }
                            outFieldOutputAccessor->reset(true, tag, val);
                        }
                    }

                    // Check early out condition.
                    if (_finalCode) {
                        if (_bytecode.runPredicate(_finalCode.get())) {
                            earlyExit = true;
                            break;
                        }
                    }
                }
            }
        }

        _inArrayAccessors.pop_back();
    } else {
        // For non-arrays we simply execute the inner side once.
        outFieldOutputAccessor->reset();
        openInner(tag, val);
        auto state = _children[1]->getNext();

        if (state == PlanState::ADVANCED) {
            auto [tag, val] = _outFieldInputAccessor->getViewOfValue();
            // We don't have to copy the value.
            outFieldOutputAccessor->reset(false, tag, val);
        }
    }

    return earlyExit;
}

void TraverseStage::close() {
    auto optTimer(getOptTimer(_opCtx));

    trackClose();

    if (_reOpenInner) {
        _children[1]->close();

        _reOpenInner = false;
    }

    _children[0]->close();
}

void TraverseStage::doSaveState(bool relinquishCursor) {
    if (_isReadingLeftSide) {
        // If we yield while reading the left side, there is no need to prepareForYielding() data
        // held in the right side, since we will have to re-open it anyway.
        const bool recursive = true;
        _children[1]->disableSlotAccess(recursive);

        // As part of reading the left side we're about to reset the out field accessor anyway.
        // No point in keeping its data around.
        _outFieldOutputAccessor.reset();
    }

    if (!slotsAccessible() || !relinquishCursor) {
        return;
    }

    prepareForYielding(_outFieldOutputAccessor);
}

void TraverseStage::doRestoreState(bool relinquishCursor) {
    if (!slotsAccessible()) {
        return;
    }

    for (auto& accessor : _inArrayAccessors) {
        accessor.refresh();
    }
}

std::unique_ptr<PlanStageStats> TraverseStage::getStats(bool includeDebugInfo) const {
    auto ret = std::make_unique<PlanStageStats>(_commonStats);
    ret->specific = std::make_unique<TraverseStats>(_specificStats);

    if (includeDebugInfo) {
        DebugPrinter printer;
        BSONObjBuilder bob;
        bob.appendNumber("innerOpens", static_cast<long long>(_specificStats.innerOpens));
        bob.appendNumber("innerCloses", static_cast<long long>(_specificStats.innerCloses));
        bob.appendNumber("inputSlot", static_cast<long long>(_inField));
        bob.appendNumber("outputSlot", static_cast<long long>(_outField));
        bob.appendNumber("outputSlotInner", static_cast<long long>(_outFieldInner));
        bob.append("correlatedSlots", _correlatedSlots.begin(), _correlatedSlots.end());
        if (_nestedArraysDepth) {
            bob.appendNumber("nestedArraysDepth", static_cast<long long>(*_nestedArraysDepth));
        }
        if (_fold) {
            bob.append("fold", printer.print(_fold->debugPrint()));
        }
        if (_final) {
            bob.append("final", printer.print(_final->debugPrint()));
        }
        ret->debugInfo = bob.obj();
    }

    ret->children.emplace_back(_children[0]->getStats(includeDebugInfo));
    ret->children.emplace_back(_children[1]->getStats(includeDebugInfo));
    return ret;
}

const SpecificStats* TraverseStage::getSpecificStats() const {
    return &_specificStats;
}

std::vector<DebugPrinter::Block> TraverseStage::debugPrint() const {
    auto ret = PlanStage::debugPrint();

    DebugPrinter::addIdentifier(ret, _outField);
    DebugPrinter::addIdentifier(ret, _outFieldInner);
    DebugPrinter::addIdentifier(ret, _inField);

    if (_correlatedSlots.size()) {
        ret.emplace_back("[`");
        for (size_t idx = 0; idx < _correlatedSlots.size(); ++idx) {
            if (idx) {
                ret.emplace_back(DebugPrinter::Block("`,"));
            }
            DebugPrinter::addIdentifier(ret, _correlatedSlots[idx]);
        }
        ret.emplace_back("`]");
    }

    ret.emplace_back("{`");
    if (_fold) {
        DebugPrinter::addBlocks(ret, _fold->debugPrint());
    }
    ret.emplace_back("`}");

    ret.emplace_back("{`");
    if (_final) {
        DebugPrinter::addBlocks(ret, _final->debugPrint());
    }
    ret.emplace_back("`}");

    if (_nestedArraysDepth) {
        ret.emplace_back(std::to_string(*_nestedArraysDepth));
    }

    DebugPrinter::addNewLine(ret);
    DebugPrinter::addIdentifier(ret, "from");
    ret.emplace_back(DebugPrinter::Block::cmdIncIndent);
    DebugPrinter::addBlocks(ret, _children[0]->debugPrint());
    ret.emplace_back(DebugPrinter::Block::cmdDecIndent);

    DebugPrinter::addIdentifier(ret, "in");
    ret.emplace_back(DebugPrinter::Block::cmdIncIndent);
    DebugPrinter::addBlocks(ret, _children[1]->debugPrint());
    ret.emplace_back(DebugPrinter::Block::cmdDecIndent);

    return ret;
}

size_t TraverseStage::estimateCompileTimeSize() const {
    size_t size = sizeof(*this);
    size += size_estimator::estimate(_children);
    size += size_estimator::estimate(_correlatedSlots);
    size += _fold ? _fold->estimateSize() : 0;
    size += _final ? _final->estimateSize() : 0;
    size += size_estimator::estimate(_specificStats);
    return size;
}
}  // namespace mongo::sbe
