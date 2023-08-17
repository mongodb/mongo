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

#pragma once

#include <vector>

#include "mongo/db/exec/sbe/expressions/expression.h"
#include "mongo/db/exec/sbe/values/slot.h"
#include "mongo/db/query/sbe_stage_builder_abt_holder_def.h"

#include "mongo/db/exec/sbe/abt/abt_lower_defs.h"
#include "mongo/db/exec/sbe/abt/named_slots.h"
#include "mongo/db/query/optimizer/node.h"  // IWYU pragma: keep
#include "mongo/db/query/optimizer/node_defs.h"
#include "mongo/db/query/optimizer/syntax/expr.h"
#include "mongo/db/query/optimizer/syntax/syntax.h"
#include "mongo/db/query/optimizer/utils/utils.h"

namespace mongo::stage_builder {

struct StageBuilderState;

/**
 * The SbExpr class is used to represent expressions in the SBE stage builder. "SbExpr" is short
 * for "stage builder expression".
 */
class SbExpr {
public:
    SbExpr() : _storage{false} {}

    SbExpr(SbExpr&& e) : _storage(std::move(e._storage)) {
        e.reset();
    }

    SbExpr(std::unique_ptr<sbe::EExpression>&& e) : _storage(std::move(e)) {}

    SbExpr(sbe::value::SlotId s) : _storage(s) {}

    SbExpr(const abt::HolderPtr& a);

    SbExpr(abt::HolderPtr&& a) : _storage(std::move(a)) {}

    SbExpr& operator=(SbExpr&& e) {
        if (this == &e) {
            return *this;
        }

        _storage = std::move(e._storage);
        e.reset();
        return *this;
    }

    SbExpr& operator=(std::unique_ptr<sbe::EExpression>&& e) {
        _storage = std::move(e);
        e.reset();
        return *this;
    }

    SbExpr& operator=(sbe::value::SlotId s) {
        _storage = s;
        return *this;
    }

    SbExpr& operator=(abt::HolderPtr&& a) {
        _storage = std::move(a);
        return *this;
    }

    boost::optional<sbe::value::SlotId> getSlot() const {
        return hasSlot() ? boost::make_optional(stdx::get<sbe::value::SlotId>(_storage))
                         : boost::none;
    }

    bool hasSlot() const {
        return stdx::holds_alternative<sbe::value::SlotId>(_storage);
    }

    bool hasExpr() const {
        return stdx::holds_alternative<std::unique_ptr<sbe::EExpression>>(_storage);
    }

    bool hasABT() const {
        return stdx::holds_alternative<abt::HolderPtr>(_storage);
    }

    SbExpr clone() const {
        if (hasSlot()) {
            return stdx::get<sbe::value::SlotId>(_storage);
        }

        if (hasABT()) {
            return stdx::get<abt::HolderPtr>(_storage);
        }

        if (stdx::holds_alternative<bool>(_storage)) {
            return SbExpr{};
        }

        const auto& expr = stdx::get<std::unique_ptr<sbe::EExpression>>(_storage);
        if (expr) {
            return expr->clone();
        }

        return {};
    }

    bool isNull() const {
        return stdx::holds_alternative<bool>(_storage);
    }

    explicit operator bool() const {
        return !isNull();
    }

    void reset() {
        _storage = false;
    }

    std::unique_ptr<sbe::EExpression> getExpr(optimizer::SlotVarMap& varMap,
                                              StageBuilderState& state) const;

    std::unique_ptr<sbe::EExpression> getExpr(StageBuilderState& state) const;

    /**
     * Extract the expression on top of the stack as an SBE EExpression node. If the expression is
     * stored as an ABT node, it is lowered into an SBE expression, using the provided map to
     * convert variable names into slot ids.
     */
    std::unique_ptr<sbe::EExpression> extractExpr(optimizer::SlotVarMap& varMap,
                                                  StageBuilderState& state);

    /**
     * Helper function that obtains data needed for SbExpr::extractExpr from StageBuilderState
     */
    std::unique_ptr<sbe::EExpression> extractExpr(StageBuilderState& state);

    /**
     * Extract the expression on top of the stack as an ABT node. If the expression is stored as a
     * slot id, the mapping between the generated ABT node and the slot id is recorded in the map.
     * Throws an exception if the expression is stored as an SBE EExpression.
     */
    abt::HolderPtr extractABT(optimizer::SlotVarMap& varMap);

private:
    // The bool type as the first option is used to represent the empty storage.
    stdx::variant<bool, std::unique_ptr<sbe::EExpression>, sbe::value::SlotId, abt::HolderPtr>
        _storage;
};

using EvalExpr = SbExpr;

}  // namespace mongo::stage_builder
