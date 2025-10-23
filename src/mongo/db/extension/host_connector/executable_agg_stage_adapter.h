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

#include "mongo/db/extension/host/aggregation_stage/executable_agg_stage.h"
#include "mongo/db/extension/public/api.h"
#include "mongo/util/modules.h"

#include <memory>

namespace mongo::extension::host_connector {

/**
 * Boundary object representation of a ::MongoExtensionExecAggStage.
 *
 * This class abstracts the C++ implementation of the extension and provides the interface at the
 * API boundary which will be called upon by the host. The static VTABLE member points to static
 * methods which ensure the correct conversion from C++ context to the C API context.
 *
 * This abstraction is required to ensure we maintain the public
 * ::MongoExtensionExecAggStage interface and layout as dictated by the public API.
 * Any polymorphic behavior must be deferred to and implemented by the ExecAggStage.
 */
class HostExecAggStageAdapter final : public ::MongoExtensionExecAggStage {
public:
    HostExecAggStageAdapter(std::unique_ptr<host::ExecAggStage> execAggStage)
        : ::MongoExtensionExecAggStage(&VTABLE), _execAggStage(std::move(execAggStage)) {}

    ~HostExecAggStageAdapter() = default;

    /**
     * Specifies whether the provided exec agg stage was allocated by the host.
     *
     * Since ExtensionExecAggStage and HostExecAggStageAdapter implement the same
     * vtable, this function is necessary for differentiating between host- and extension-allocated
     * exec agg stages.
     *
     * Use this function to check if an exec agg stage is host-allocated before casting a
     * MongoExtensionExecAggStage to a HostExecAggStageAdapter.
     */
    static inline bool isHostAllocated(::MongoExtensionExecAggStage& execAggStage) {
        return execAggStage.vtable == &VTABLE;
    }

private:
    const host::ExecAggStage& getImpl() const noexcept {
        return *_execAggStage;
    }

    host::ExecAggStage& getImpl() noexcept {
        return *_execAggStage;
    }

    static void _hostDestroy(::MongoExtensionExecAggStage* execAggStage) noexcept {
        delete static_cast<HostExecAggStageAdapter*>(execAggStage);
    }

    /**
     * Transforms a exec::agg::GetNextResult into a ::MongoExtensionGetNextResult. Ownership of the
     * result in the ::MongoExtensionGetNextResult is transferred to the caller.
     */
    static ::MongoExtensionStatus* _hostGetNext(::MongoExtensionExecAggStage* execAggStage,
                                                ::MongoExtensionGetNextResult* apiResult) noexcept;

    static constexpr ::MongoExtensionExecAggStageVTable VTABLE{.destroy = &_hostDestroy,
                                                               .get_next = &_hostGetNext};

    std::unique_ptr<host::ExecAggStage> _execAggStage;
};
};  // namespace mongo::extension::host_connector
