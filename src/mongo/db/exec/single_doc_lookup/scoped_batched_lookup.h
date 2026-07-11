// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/db/exec/single_doc_lookup/single_document_lookup_executor.h"

#include <utility>

namespace mongo::exec::agg {

/**
 * RAII batch scope over a SingleDocumentLookupExecutor. Construction only captures the executor and
 * on destruction it releases the resources.
 * Exception-safe by construction: the release runs even when the enrich body throws.
 */
class [[nodiscard]] ScopedBatchedLookup final {
public:
    explicit ScopedBatchedLookup(SingleDocumentLookupExecutor& executor) : _executor(&executor) {}

    ~ScopedBatchedLookup() {
        release();
    }

    ScopedBatchedLookup(ScopedBatchedLookup&& other) noexcept
        : _executor(std::exchange(other._executor, nullptr)) {}

    ScopedBatchedLookup& operator=(ScopedBatchedLookup&& other) noexcept {
        if (this != &other) {
            release();
            _executor = std::exchange(other._executor, nullptr);
        }
        return *this;
    }

    ScopedBatchedLookup(const ScopedBatchedLookup&) = delete;
    ScopedBatchedLookup& operator=(const ScopedBatchedLookup&) = delete;

private:
    void release() noexcept {
        if (_executor) {
            _executor->releaseResources();
            _executor = nullptr;
        }
    }

    SingleDocumentLookupExecutor* _executor;
};

}  // namespace mongo::exec::agg
