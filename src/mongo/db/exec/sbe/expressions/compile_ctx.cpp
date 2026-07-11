// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/exec/sbe/expressions/compile_ctx.h"


namespace mongo::sbe {

value::SlotAccessor* CompileCtx::getAccessor(value::SlotId slot) {
    for (auto it = correlated.rbegin(); it != correlated.rend(); ++it) {
        if (it->first == slot) {
            return it->second;
        }
    }

    return _env->getAccessor(slot);
}

std::shared_ptr<SpoolBuffer> CompileCtx::getSpoolBuffer(SpoolId spool) {
    if (spoolBuffers.find(spool) == spoolBuffers.end()) {
        spoolBuffers.emplace(spool, std::make_shared<SpoolBuffer>());
    }
    return spoolBuffers[spool];
}

void CompileCtx::pushCorrelated(value::SlotId slot, value::SlotAccessor* accessor) {
    correlated.emplace_back(slot, accessor);
}

void CompileCtx::popCorrelated() {
    correlated.pop_back();
}

CompileCtx CompileCtx::makeCopyForParallelUse() {
    return {_env->makeCopyForParallelUse()};
}

CompileCtx CompileCtx::makeCopy() const {
    return {_env->makeCopy()};
}
}  // namespace mongo::sbe
