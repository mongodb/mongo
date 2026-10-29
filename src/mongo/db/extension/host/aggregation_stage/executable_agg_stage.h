// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0
#pragma once

#include "mongo/db/exec/agg/stage.h"
#include "mongo/util/modules.h"

#include <memory>
#include <string_view>

namespace mongo::extension::host {

/**
 * Host-defined ExecAggStage node.
 *
 * Wraps an exec::agg::Stage (an execution stage) such that a host-defined execution stage can
 * forward the results from its execution stage to the extension-defined transform stage.
 */
class ExecAggStage {
public:
    ~ExecAggStage() = default;

    /**
     * Returns the next result from the underlying execution stage.
     */
    exec::agg::GetNextResult getNext() {
        return _execAggStage->getNext();
    }

    std::string_view getName() const {
        return _stageName;
    }

    static inline std::unique_ptr<ExecAggStage> make(exec::agg::Stage* execAggStage) {
        return std::unique_ptr<ExecAggStage>(new ExecAggStage(execAggStage));
    }

protected:
    ExecAggStage(absl::Nonnull<exec::agg::Stage*> execAggStage)
        : _execAggStage(execAggStage), _stageName(execAggStage->getCommonStats().stageTypeStr) {}

private:
    exec::agg::Stage* const _execAggStage;
    const std::string _stageName;
};

};  // namespace mongo::extension::host
