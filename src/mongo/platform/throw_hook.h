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
