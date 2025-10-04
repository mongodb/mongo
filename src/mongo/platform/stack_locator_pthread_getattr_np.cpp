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


#include "mongo/platform/stack_locator.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/scopeguard.h"

#include <pthread.h>

namespace mongo {

StackLocator::StackLocator(const void* capturedStackPointer)
    : _capturedStackPointer(capturedStackPointer) {
    pthread_t self = pthread_self();
    pthread_attr_t selfAttrs;
    invariant(pthread_attr_init(&selfAttrs) == 0);
    invariant(pthread_getattr_np(self, &selfAttrs) == 0);
    ON_BLOCK_EXIT([&] { pthread_attr_destroy(&selfAttrs); });

    void* base = nullptr;
    size_t size = 0;

    auto result = pthread_attr_getstack(&selfAttrs, &base, &size);

    invariant(result == 0);
    invariant(base != nullptr);
    invariant(size != 0);

    // TODO: Assumes a downward growing stack. Note here that
    // getstack returns the stack *base*, being the bottom of the
    // stack, so we need to add size to it.
    _end = base;
    _begin = static_cast<const char*>(_end) + size;
}

}  // namespace mongo
