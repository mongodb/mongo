// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/base/data_view.h"
#include "mongo/util/modules.h"

#include <cstring>

[[MONGO_MOD_PUBLIC]];

namespace mongo {

struct ZeroInitTag_t {
    constexpr explicit ZeroInitTag_t() = default;
};

constexpr inline ZeroInitTag_t kZeroInitTag;

template <typename Layout, typename ConstView, typename View>
class [[MONGO_MOD_OPEN]] EncodedValueStorage {
protected:
    EncodedValueStorage() {}

    // This explicit constructor is provided to allow for easy zeroing
    // during creation of a value.  You might prefer this over an
    // uninitialised value if the zeroed version provides a useful base
    // state.  Such cases might include a set of counters that begin at
    // zero, flags that start off false or a larger structure where some
    // significant portion of storage falls into those kind of use cases.
    // Use this where you might have used calloc(1, sizeof(type)) in C.
    //
    // The added value of providing it as a constructor lies in the ability
    // of subclasses to easily inherit a zeroed base state during
    // initialization.
    explicit EncodedValueStorage(ZeroInitTag_t) {
        std::memset(_data, 0, sizeof(_data));
    }

public:
    View view() {
        return _data;
    }

    ConstView constView() const {
        return _data;
    }

    operator View() {
        return view();
    }

    operator ConstView() const {
        return constView();
    }

private:
    char _data[sizeof(Layout)];
};

}  // namespace mongo
