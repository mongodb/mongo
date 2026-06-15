/**
 *    Copyright (C) 2026-present MongoDB, Inc.
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

#include "mongo/base/status.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/functional.h"

#include <type_traits>
#include <utility>

namespace mongo {

/**
 * Drop-in `data:` storage for an enum-typed query knob's IDL `cpp_class`, wrapping T (a
 * synchronized_value) with an on-update hook. Enum knobs lack the atomic path's
 * setOnUpdate, so this supplies the equivalent seam.
 *
 * The hook fires after the value is committed. A non-OK Status from the hook is
 * uasserted, matching the behaviour of scalar server parameter on_update callbacks.
 */
template <typename T>
class WithOnUpdateHook : public T {
public:
    using value_t = std::remove_cvref_t<decltype(std::declval<const T&>().get())>;
    using OnUpdateFn = unique_function<Status(const value_t&)>;

    template <typename... Args>
    WithOnUpdateHook(Args&&... args) : T(std::forward<Args>(args)...) {}

    WithOnUpdateHook& operator=(const value_t& other) {
        static_cast<T&>(*this) = other;
        if (_onUpdate) {
            uassertStatusOK(_onUpdate(other));
        }
        return *this;
    }
    WithOnUpdateHook& operator=(value_t&& other) {
        value_t copy = other;
        static_cast<T&>(*this) = std::move(other);
        if (_onUpdate) {
            uassertStatusOK(_onUpdate(copy));
        }
        return *this;
    }

    /**
     * Installs the on-update hook, not thread safe.
     */
    void setOnUpdate(OnUpdateFn callback) {
        _onUpdate = std::move(callback);
    }

private:
    OnUpdateFn _onUpdate = nullptr;
};

}  // namespace mongo
