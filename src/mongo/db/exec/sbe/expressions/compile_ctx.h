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

#include "mongo/db/exec/sbe/expressions/runtime_environment.h"
#include "mongo/db/exec/sbe/values/row.h"
#include "mongo/db/exec/sbe/values/slot.h"
#include "mongo/db/exec/sbe/values/value.h"
#include "mongo/db/exec/sbe/vm/vm_types.h"
#include "mongo/db/pipeline/search/search_helper.h"
#include "mongo/stdx/unordered_map.h"

#include <memory>
#include <utility>
#include <vector>

namespace mongo::sbe {

using SpoolBuffer = std::vector<value::MaterializedRow>;
class PlanStage;

struct CompileCtx {
    CompileCtx(std::unique_ptr<RuntimeEnvironment> env) : _env{std::move(env)} {}

    value::SlotAccessor* getAccessor(value::SlotId slot);

    RuntimeEnvironment::Accessor* getRuntimeEnvAccessor(value::SlotId slotId) {
        return _env->getAccessor(slotId);
    }

    std::shared_ptr<SpoolBuffer> getSpoolBuffer(SpoolId spool);

    void pushCorrelated(value::SlotId slot, value::SlotAccessor* accessor);
    void popCorrelated();

    /**
     * Make a copy of this CompileCtx. The underlying RuntimeEnvironment will also be copied.
     *
     * To create a copy of the underlying runtime environment for a parallel execution plan, please
     * use makeCopyForParallelUse() method. This will result in the environment in this CompileCtx
     * being converted to a parallel environment, as well as the newly created copy.
     */
    CompileCtx makeCopyForParallelUse();
    CompileCtx makeCopy() const;

    vm::LabelId newLabelId() {
        return ++lastLabelId;
    }

    /**
     * Root plan stage is used to resolve slot accessors introduced by PlanStage (optional).
     * - if specified, the root plan stage will be used to resolve the slot accessor.
     * - otherwise, if null, default context accessor resolution rules will be used.
     */
    PlanStage* root{nullptr};

    value::SlotAccessor* accumulator{nullptr};
    std::vector<std::pair<value::SlotId, value::SlotAccessor*>> correlated;
    stdx::unordered_map<SpoolId, std::shared_ptr<SpoolBuffer>> spoolBuffers;
    bool aggExpression{false};
    vm::LabelId lastLabelId{0};
    RemoteCursorMap* remoteCursors{nullptr};

private:
    // Any data that a PlanStage needs from the RuntimeEnvironment should not be accessed directly
    // but insteady by looking up the corresponding slots. These slots are set up during the process
    // of building PlanStages, so the PlanStages themselves should never need to add new slots to
    // the RuntimeEnvironment.
    std::unique_ptr<RuntimeEnvironment> _env;
};
}  // namespace mongo::sbe
