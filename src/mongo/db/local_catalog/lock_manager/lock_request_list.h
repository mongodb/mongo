/**
 *    Copyright (C) 2018-present MongoDB, Inc.
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

#include "mongo/db/local_catalog/lock_manager/lock_manager_defs.h"
#include "mongo/util/assert_util.h"

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
class LockRequestList {
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
