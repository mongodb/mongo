// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/base/status.h"
#include "mongo/db/storage/sorted_data_interface.h"
#include "mongo/unittest/unittest.h"

#include <variant>

namespace mongo::unittest {
template <typename T>
T* get_if(boost::optional<T>* opt) {
    return opt ? &**opt : nullptr;
}
}  // namespace mongo::unittest


#define ASSERT_SDI_INSERT_OK(EXPR)                  \
    {                                               \
        auto result = EXPR;                         \
        auto status = std::get_if<Status>(&result); \
        ASSERT(status);                             \
        ASSERT_OK(*status);                         \
    }

#define ASSERT_SDI_INSERT_KEY_EXISTS(EXPR)                \
    {                                                     \
        auto result = EXPR;                               \
        auto status = std::get_if<Status>(&result);       \
        ASSERT(status);                                   \
        ASSERT_EQ(status->code(), ErrorCodes::KeyExists); \
    }

#define ASSERT_SDI_INSERT_DUPLICATE_KEY(EXPR, KEY, ID)                       \
    {                                                                        \
        using std::get_if;                                                   \
        using mongo::unittest::get_if;                                       \
        auto result = EXPR;                                                  \
        auto duplicate = get_if<SortedDataInterface::DuplicateKey>(&result); \
        ASSERT(duplicate);                                                   \
        ASSERT_BSONOBJ_EQ(duplicate->key, KEY);                              \
        ASSERT_EQ(duplicate->id, ID);                                        \
    }
