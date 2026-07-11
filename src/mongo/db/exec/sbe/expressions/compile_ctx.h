// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/db/exec/sbe/expressions/runtime_environment.h"
#include "mongo/db/exec/sbe/values/row.h"
#include "mongo/db/exec/sbe/values/slot.h"
#include "mongo/db/exec/sbe/values/value.h"
#include "mongo/db/exec/sbe/vm/vm_types.h"
#include "mongo/db/pipeline/search/search_helper.h"
#include "mongo/stdx/unordered_map.h"
#include "mongo/util/modules.h"

#include <memory>
#include <utility>
#include <vector>

namespace mongo {
class MultipleCollectionAccessor;
}

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
    const MultipleCollectionAccessor* mca{nullptr};

private:
    // Any data that a PlanStage needs from the RuntimeEnvironment should not be accessed directly
    // but insteady by looking up the corresponding slots. These slots are set up during the process
    // of building PlanStages, so the PlanStages themselves should never need to add new slots to
    // the RuntimeEnvironment.
    std::unique_ptr<RuntimeEnvironment> _env;
};
}  // namespace mongo::sbe
