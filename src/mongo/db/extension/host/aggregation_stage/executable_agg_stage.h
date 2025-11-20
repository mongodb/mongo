/**
 *    Copyright (C) 2025-present MongoDB, Inc.
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

#include "mongo/db/exec/agg/stage.h"
#include "mongo/util/modules.h"

#include <memory>

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
