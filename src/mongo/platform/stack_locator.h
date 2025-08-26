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

#include "mongo/platform/compiler.h"
#include "mongo/util/modules.h"

#include <cstddef>

#include <boost/optional.hpp>
#include <boost/optional/optional.hpp>

#ifdef __has_builtin
#if __has_builtin(__builtin_frame_address)
#define MONGO_STACK_LOCATOR_HAS_BUILTIN_FRAME_ADDRESS
#endif
#endif

namespace MONGO_MOD_PUB mongo {

/**
 *  Provides access to the current stack bounds and remaining
 *  available stack space.
 *
 *  To use one, create it on the stack, like this:
 *
 *  // Construct a new locator
 *  const StackLocator locator;
 *
 *  // Get the start of the stack
 *  auto b = locator.begin();
 *
 *  // Get the end of the stack
 *  auto e = locator.end();
 *
 *  // Get the remaining space after 'locator' on the stack.
 *  auto avail = locator.available();
 */
class StackLocator {
public:
    MONGO_COMPILER_IF_MSVC(_Pragma("warning(push)"))
    // returning address of local variable or temporary
    MONGO_COMPILER_IF_MSVC(_Pragma("warning(disable:4172)"))
    MONGO_COMPILER_ALWAYS_INLINE static const void* getFramePointer() {
#ifdef MONGO_STACK_LOCATOR_HAS_BUILTIN_FRAME_ADDRESS
        return __builtin_frame_address(0);
#else
        int x;
        return &x;
#endif  // MONGO_STACK_LOCATOR_HAS_BUILTIN_FRAME_ADDRESS
    }
    MONGO_COMPILER_IF_MSVC(_Pragma("warning(pop)"))

    explicit StackLocator(const void* capturedStackPointer = getFramePointer());

    /**
     *  Returns the address of the beginning of the stack, or nullptr
     *  if this cannot be done. Beginning here means those addresses
     *  that represent values of automatic duration found earlier in
     *  the call chain. Returns nullptr if the beginning of the stack
     *  could not be found.
     */
    const void* begin() const {
        return _begin;
    }

    /**
     *  Returns the address of the end of the stack, or nullptr if
     *  this cannot be done. End here means those addresses that
     *  represent values of automatic duration allocated deeper in the
     *  call chain. Returns nullptr if the end of the stack could not
     *  be found.
     */
    const void* end() const {
        return _end;
    }

    /**
     *  Returns the apparent size of the stack. Returns a disengaged
     *  optional if the size of the stack could not be determined.
     */
    boost::optional<size_t> size() const;

    /**
     *  Returns the remaining stack available after the location of
     *  this StackLocator. Obviously, the StackLocator must have been
     *  constructed on the stack. Calling 'available' on a heap
     *  allocated StackAllocator will have undefined behavior. Returns
     *  a disengaged optional if the remaining stack cannot be
     *  determined.
     */
    boost::optional<std::size_t> available() const;

private:
    const void* _begin = nullptr;
    const void* _end = nullptr;
    const void* _capturedStackPointer = nullptr;
};

}  // namespace MONGO_MOD_PUB mongo
