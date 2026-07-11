// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

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
