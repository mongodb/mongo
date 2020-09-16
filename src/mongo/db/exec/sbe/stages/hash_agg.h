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

#pragma once

#include <unordered_map>

#include "mongo/db/exec/sbe/expressions/expression.h"
#include "mongo/db/exec/sbe/stages/stages.h"
#include "mongo/db/exec/sbe/vm/vm.h"
#include "mongo/stdx/unordered_map.h"

namespace mongo {
namespace sbe {
class HashAggStage final : public PlanStage {
public:
    HashAggStage(std::unique_ptr<PlanStage> input,
                 value::SlotVector gbs,
                 value::SlotMap<std::unique_ptr<EExpression>> aggs,
                 PlanNodeId planNodeId);

    std::unique_ptr<PlanStage> clone() const final;

    void prepare(CompileCtx& ctx) final;
    value::SlotAccessor* getAccessor(CompileCtx& ctx, value::SlotId slot) final;
    void open(bool reOpen) final;
    PlanState getNext() final;
    void close() final;

    std::unique_ptr<PlanStageStats> getStats() const final;
    const SpecificStats* getSpecificStats() const final;
    std::vector<DebugPrinter::Block> debugPrint() const final;

private:
    using TableType = stdx::
        unordered_map<value::MaterializedRow, value::MaterializedRow, value::MaterializedRowHasher>;

    using HashKeyAccessor = value::MaterializedRowKeyAccessor<TableType::iterator>;
    using HashAggAccessor = value::MaterializedRowValueAccessor<TableType::iterator>;

    const value::SlotVector _gbs;
    const value::SlotMap<std::unique_ptr<EExpression>> _aggs;

    value::SlotAccessorMap _outAccessors;
    std::vector<value::SlotAccessor*> _inKeyAccessors;
    std::vector<std::unique_ptr<HashKeyAccessor>> _outKeyAccessors;

    std::vector<std::unique_ptr<HashAggAccessor>> _outAggAccessors;
    std::vector<std::unique_ptr<vm::CodeFragment>> _aggCodes;

    TableType _ht;
    TableType::iterator _htIt;

    vm::ByteCode _bytecode;

    bool _compiled{false};
};
}  // namespace sbe
}  // namespace mongo
