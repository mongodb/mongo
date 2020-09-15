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

#pragma once

#include "mongo/db/exec/sbe/stages/stages.h"
#include "mongo/db/fts/fts_matcher.h"

namespace mongo::sbe {

/**
 * Special PlanStage for evaluating an FTSMatcher. Reads a BSON object from 'inputSlot' and passes
 * it to the FTSMatcher. Fills out 'outputSlot' with the resulting boolean. If 'inputSlot' contains
 * a value of any type other than 'bsonObject', throws a UserException.
 *
 * TODO: Can this be expressed via string manipulation EExpressions? That would eliminate the need
 * for this special stage.
 */
class TextMatchStage final : public PlanStage {
public:
    TextMatchStage(std::unique_ptr<PlanStage> inputStage,
                   const fts::FTSQueryImpl& ftsQuery,
                   const fts::FTSSpec& ftsSpec,
                   value::SlotId inputSlot,
                   value::SlotId outputSlot)
        : PlanStage("textmatch"),
          _ftsMatcher(ftsQuery, ftsSpec),
          _inputSlot(inputSlot),
          _outputSlot(outputSlot) {
        _children.emplace_back(std::move(inputStage));
    }

    std::unique_ptr<PlanStage> clone() const final;

    void prepare(CompileCtx& ctx) final;

    value::SlotAccessor* getAccessor(CompileCtx& ctx, value::SlotId slot) final;

    void open(bool reOpen) final;

    PlanState getNext() final;

    void close() final;

    std::vector<DebugPrinter::Block> debugPrint() const final;

    std::unique_ptr<PlanStageStats> getStats() const final;

    const SpecificStats* getSpecificStats() const final {
        return nullptr;
    }

private:
    // Phrase and negated term matcher.
    const fts::FTSMatcher _ftsMatcher;

    const value::SlotId _inputSlot;
    const value::SlotId _outputSlot;

    value::SlotAccessor* _inValueAccessor{nullptr};
    value::ViewOfValueAccessor _outValueAccessor;
};

}  // namespace mongo::sbe
