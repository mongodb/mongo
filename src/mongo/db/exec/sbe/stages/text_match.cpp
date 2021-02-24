/**
 *    Copyright (C) 2020-present MongoDB, Inc.
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

#include "mongo/db/exec/sbe/stages/text_match.h"

#include "mongo/db/exec/sbe/expressions/expression.h"
#include "mongo/db/exec/sbe/values/bson.h"

namespace mongo::sbe {

std::unique_ptr<PlanStage> TextMatchStage::clone() const {
    return makeS<TextMatchStage>(_children[0]->clone(),
                                 _ftsMatcher.query(),
                                 _ftsMatcher.spec(),
                                 _inputSlot,
                                 _outputSlot,
                                 _commonStats.nodeId);
}

void TextMatchStage::prepare(CompileCtx& ctx) {
    _children[0]->prepare(ctx);
    _inValueAccessor = _children[0]->getAccessor(ctx, _inputSlot);
}

value::SlotAccessor* TextMatchStage::getAccessor(CompileCtx& ctx, value::SlotId slot) {
    if (slot == _outputSlot) {
        return &_outValueAccessor;
    }

    return _children[0]->getAccessor(ctx, slot);
}

void TextMatchStage::open(bool reOpen) {
    auto optTimer(getOptTimer(_opCtx));

    _commonStats.opens++;
    _children[0]->open(reOpen);
}

PlanState TextMatchStage::getNext() {
    auto optTimer(getOptTimer(_opCtx));

    auto state = _children[0]->getNext();

    if (state == PlanState::ADVANCED) {
        auto&& [typeTag, value] = _inValueAccessor->getViewOfValue();
        uassert(ErrorCodes::Error(4623400),
                "textmatch requires input to be an object",
                value::isObject(typeTag));
        BSONObj obj;
        if (typeTag == value::TypeTags::bsonObject) {
            obj = BSONObj{value::bitcastTo<const char*>(value)};
        } else {
            BSONObjBuilder builder;
            bson::convertToBsonObj(builder, value::getObjectView(value));
            obj = builder.obj();
        }
        const auto matchResult = _ftsMatcher.matches(obj);
        _outValueAccessor.reset(value::TypeTags::Boolean, value::bitcastFrom<bool>(matchResult));
    }

    return trackPlanState(state);
}

void TextMatchStage::close() {
    auto optTimer(getOptTimer(_opCtx));

    _commonStats.closes++;
    _children[0]->close();
}

std::vector<DebugPrinter::Block> TextMatchStage::debugPrint() const {
    // TODO: Add 'textmatch' to the parser so that the debug output can be parsed back to an
    // execution plan.
    auto ret = PlanStage::debugPrint();

    DebugPrinter::addIdentifier(ret, _inputSlot);
    DebugPrinter::addIdentifier(ret, _outputSlot);

    DebugPrinter::addNewLine(ret);
    DebugPrinter::addBlocks(ret, _children[0]->debugPrint());

    return ret;
}

std::unique_ptr<PlanStageStats> TextMatchStage::getStats(bool includeDebugInfo) const {
    auto ret = std::make_unique<PlanStageStats>(_commonStats);

    if (includeDebugInfo) {
        BSONObjBuilder bob;
        bob.appendIntOrLL("inputSlot", _inputSlot);
        bob.appendIntOrLL("outputSlot", _outputSlot);
        ret->debugInfo = bob.obj();
    }

    ret->children.emplace_back(_children[0]->getStats(includeDebugInfo));
    return ret;
}

}  // namespace mongo::sbe
