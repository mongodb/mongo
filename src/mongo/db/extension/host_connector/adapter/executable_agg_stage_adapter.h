// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0
#pragma once

#include "mongo/db/extension/host/aggregation_stage/executable_agg_stage.h"
#include "mongo/db/extension/public/api.h"
#include "mongo/db/extension/shared/extension_status.h"
#include "mongo/util/modules.h"

#include <memory>

namespace mongo::extension::host_connector {

/**
 * CachedGetNextResult is a helper class used to cache Host exec::agg::GetNextResult documents.
 * When converting a Host exec::agg::GetNextResult to the boundary type
 * ::MongoExtensionGetNextResult, we want to avoid having to make unnecessary copies of the result
 * BSON document. For this reason, the key function getAsExtensionNextResult only provides the BSON
 * result as a view on the original document.
 * Any populated ::MongoExtensionGetNextResult via the key function must not outlive the
 * CachedGetNextResult instance from which it was obtained. This is guaranteed by
 * HostExecAggStageAdapter when servicing calls to getNext(), where HostExecAggStageAdapter
 * guarantees to keep the previous CachedGetNextResult valid until the subsequent GetNext() call.
 */
class CachedGetNextResult {
public:
    explicit CachedGetNextResult() {}
    explicit CachedGetNextResult(exec::agg::GetNextResult&& hostResult)
        : _getNextResult(std::move(hostResult)) {}

    CachedGetNextResult(const CachedGetNextResult&) = delete;
    CachedGetNextResult& operator=(const CachedGetNextResult&) = delete;
    CachedGetNextResult(CachedGetNextResult&&) = default;
    CachedGetNextResult& operator=(CachedGetNextResult&&) = default;
    /**
     * Expresses a CachedGetNextResult as the boundary type ::MongoExtensionGetNextResult.
     * Callers of this function must guarantee to keep this CachedGetNextResult instance valid
     * for as long as the output value is needed. This is required, because this conversion function
     * only provides the BSON result as a view on the original document.
     * This was done intentionally to avoid making unnecessary copies of owned BSON.
     * This function is typically used by HostExecAggStage when servicing calls to getNext(), where
     * HostExecAggStageAdapter guarantees to keep the previous CachedGetNextResult valid until the
     * subsequent GetNext() call.
     */
    void getAsExtensionNextResult(::MongoExtensionGetNextResult& outputResult);

    exec::agg::GetNextResult::ReturnStatus getStatus() const {
        return _getNextResult.getStatus();
    }

private:
    exec::agg::GetNextResult _getNextResult{exec::agg::GetNextResult::makeEOF()};
    boost::optional<BSONObj> _resultDocument{boost::none};
    boost::optional<BSONObj> _resultMetadata{boost::none};
};

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
        : ::MongoExtensionExecAggStage(&VTABLE), _execAggStage(std::move(execAggStage)) {
        tassert(10957207,
                "The adapter's underlying host exec agg stage is invalid.",
                _execAggStage != nullptr);
    }

    ~HostExecAggStageAdapter() = default;

    // HostExecAggStageAdapter is non-copyable and non-moveable, as adapters should be heap
    // allocated, and managed via a unique_ptr or Handle.
    // This property guarantees that the adapter's underlying implementation pointer remains valid
    // for object's lifetime.
    HostExecAggStageAdapter(const HostExecAggStageAdapter&) = delete;
    HostExecAggStageAdapter& operator=(const HostExecAggStageAdapter&) = delete;
    HostExecAggStageAdapter(HostExecAggStageAdapter&&) = delete;
    HostExecAggStageAdapter& operator=(HostExecAggStageAdapter&&) = delete;

    /**
     * Specifies whether the provided exec agg stage was allocated by the host.
     *
     * Since ExtensionExecAggStageAdapter and HostExecAggStageAdapter implement the same
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
                                                ::MongoExtensionQueryExecutionContext* execCtxPtr,
                                                ::MongoExtensionGetNextResult* apiResult) noexcept;

    static MongoExtensionByteView _hostGetName(
        const ::MongoExtensionExecAggStage* execAggStage) noexcept;

    static MongoExtensionStatus* _hostCreateMetrics(
        const MongoExtensionExecAggStage* execAggStage,
        MongoExtensionOperationMetrics** metrics) noexcept {
        return wrapCXXAndConvertExceptionToStatus([]() {
            tasserted(11213501,
                      "_hostCreateMetrics should not be called. Ensure that execAggStage is "
                      "extension-allocated, not host-allocated.");
        });
    }

    static MongoExtensionStatus* _hostSetSource(::MongoExtensionExecAggStage* execAggStage,
                                                ::MongoExtensionExecAggStage* input) noexcept {
        return wrapCXXAndConvertExceptionToStatus([]() {
            tasserted(10957206,
                      "_hostSetSource should not be called. Ensure that execAggStage is "
                      "extension-allocated, not host-allocated.");
        });
    };

    /**
     * The following lifecycle functions are unreachable because HostExecAggStageAdapter only wraps
     * host stages to forward getNext() results to extension stages. These functions are currently
     * only implemented for SBE stages, but in the near term, we may implement this functionality in
     * agg::Stage. In the long term, HostExecAggStageAdapter will need to be adapted to accommodate
     * these changes.
     */
    static ::MongoExtensionStatus* _hostOpen(::MongoExtensionExecAggStage* execAggStage) noexcept {
        return wrapCXXAndConvertExceptionToStatus([]() {
            tasserted(11216700,
                      "_hostOpen should not be called. Ensure that execAggStage is "
                      "extension-allocated, not host-allocated.");
        });
    }

    static ::MongoExtensionStatus* _hostReopen(
        ::MongoExtensionExecAggStage* execAggStage) noexcept {
        return wrapCXXAndConvertExceptionToStatus([]() {
            tasserted(11216701,
                      "_hostReopen should not be called. Ensure that execAggStage is "
                      "extension-allocated, not host-allocated.");
        });
    }

    static ::MongoExtensionStatus* _hostClose(::MongoExtensionExecAggStage* execAggStage) noexcept {
        return wrapCXXAndConvertExceptionToStatus([]() {
            tasserted(11216702,
                      "_hostClose should not be called. Ensure that execAggStage is "
                      "extension-allocated, not host-allocated.");
        });
    }

    static ::MongoExtensionStatus* _hostExplain(const ::MongoExtensionExecAggStage* execAggStage,
                                                ::MongoExtensionQueryExecutionContext* execCtx,
                                                ::MongoExtensionExplainVerbosity verbosity,
                                                ::MongoExtensionByteBuf** output) noexcept {
        return wrapCXXAndConvertExceptionToStatus([]() {
            tasserted(12149000,
                      "_hostExplain should not be called. Ensure that execAggStage is "
                      "extension-allocated, not host-allocated.");
        });
    }

    static constexpr ::MongoExtensionExecAggStageVTable VTABLE = {.destroy = &_hostDestroy,
                                                                  .get_next = &_hostGetNext,
                                                                  .get_name = &_hostGetName,
                                                                  .create_metrics =
                                                                      &_hostCreateMetrics,
                                                                  .set_source = &_hostSetSource,
                                                                  .open = &_hostOpen,
                                                                  .reopen = &_hostReopen,
                                                                  .close = &_hostClose,
                                                                  .explain = &_hostExplain};

    std::unique_ptr<host::ExecAggStage> _execAggStage;
    CachedGetNextResult _lastGetNextResult;
};

};  // namespace mongo::extension::host_connector
