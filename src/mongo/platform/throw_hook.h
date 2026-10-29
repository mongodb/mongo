// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/util/modules.h"

#include <functional>
#include <typeinfo>

namespace mongo {

/**
 * Cast for use inside ThrowHook implementations. Takes the passed
 * instance pointer, with specified type_info, and attempts to cast
 * it to the type specified by the template. Returns nullptr if the
 * passed instance is not derived from the type specified in the
 * template. Essentially a runtime static_cast<> using the type_info
 * to calculate things like pointer offsets.
 *
 * The __do_catch call in the implementation is part of the Linux C++
 * ABI, but not part of the C++ standard.
 */
template <typename T>
T* catchCast(std::type_info* tinfo, void* obj) {
    return typeid(T).__do_catch(tinfo, &obj, 1) ? static_cast<T*>(obj) : nullptr;
}

using ThrowHook = std::function<void(std::type_info*, void*)>;

/**
 * Set a hook function which is called whenever an exception is thrown.
 * Used for collecting stack traces, which are lost by the time the
 * exception is caught.
 */
void setThrowHook(ThrowHook);
/**
 * Get the current hook function for exception throw.
 */
const ThrowHook& getThrowHook();

}  // namespace mongo
