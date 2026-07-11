// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/db/shard_role/lock_manager/lock_manager_defs.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/modules.h"

namespace mongo {

/**
 * Simple intrusive list implementation for the lock's granted and conflicting lists. Does not
 * own its contents, just uses the intrusive pointers on the LockRequest structure to link them
 * together. Therefore requests must outlive this list.
 *
 * Intentionally implemented as a POD in order to avoid constructor/destructor invocations.
 *
 * NOTE: This class should not be used for generic purposes and should not be used outside of
 * the Lock Manager library.
 */
class [[MONGO_MOD_PRIVATE]] LockRequestList {
public:
    void push_front(LockRequest* request) {
        // Sanity check that we do not reuse entries without cleaning them up
        invariant(request->next == nullptr);
        invariant(request->prev == nullptr);

        if (_front == nullptr) {
            _front = _back = request;
        } else {
            invariant(_front->prev == nullptr);
            request->next = _front;

            _front->prev = request;
            _front = request;
        }
    }

    void push_back(LockRequest* request) {
        // Sanity check that we do not reuse entries without cleaning them up
        invariant(request->next == nullptr);
        invariant(request->prev == nullptr);

        if (_front == nullptr) {
            _front = _back = request;
        } else {
            invariant(_back);
            invariant(_back->next == nullptr);
            request->prev = _back;

            _back->next = request;
            _back = request;
        }
    }

    void remove(LockRequest* request) {
        if (request->prev != nullptr) {
            invariant(request->prev->next == request);
            request->prev->next = request->next;
        } else {
            _front = request->next;
        }

        if (request->next != nullptr) {
            invariant(request->next->prev == request);
            request->next->prev = request->prev;
        } else {
            _back = request->prev;
        }

        request->prev = nullptr;
        request->next = nullptr;

        invariant((_front == nullptr) == (_back == nullptr),
                  str::stream() << "_front=" << (void*)_front << ", _back=" << (void*)_back);
    }

    void reset() {
        _front = _back = nullptr;
    }

    bool empty() const {
        return _front == nullptr;
    }

    // Pointers to the beginning and the end of the list
    LockRequest* _front;
    LockRequest* _back;
};

}  // namespace mongo
