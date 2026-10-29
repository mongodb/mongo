// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/base/data_type.h"
#include "mongo/config.h"  // IWYU pragma: keep
#include "mongo/util/modules.h"

#include <cstring>
#include <iosfwd>
#include <type_traits>

[[MONGO_MOD_PUBLIC]];

namespace mongo {

class ConstDataView {
public:
    typedef const char* bytes_type;

    ConstDataView(bytes_type bytes) : _bytes(bytes) {}

    bytes_type view(std::ptrdiff_t offset = 0) const {
        return _bytes + offset;
    }

    template <typename T>
    const ConstDataView& readInto(T* t, std::ptrdiff_t offset = 0) const {
        DataType::unsafeLoad(t, view(offset), nullptr);

        return *this;
    }

    template <typename T>
    T read(std::ptrdiff_t offset = 0) const {
        T t(DataType::defaultConstruct<T>());

        readInto(&t, offset);

        return t;
    }

private:
    bytes_type _bytes;
};

class DataView : public ConstDataView {
public:
    typedef char* bytes_type;

    DataView(bytes_type bytes) : ConstDataView(bytes) {}

    bytes_type view(std::ptrdiff_t offset = 0) const {
        // It is safe to cast away const here since the pointer stored in our base class was
        // originally non-const by way of our constructor.
        return const_cast<bytes_type>(ConstDataView::view(offset));
    }

    template <typename T>
    DataView& write(const T& value, std::ptrdiff_t offset = 0) {
        DataType::unsafeStore(value, view(offset), nullptr);

        return *this;
    }
};

}  // namespace mongo
