/*-
 * Copyright (c) 2014-present MongoDB, Inc.
 * Copyright (c) 2008-2014 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

#pragma once

#include "wt_internal.h"

#include <string>

#define DB_HOME "test_db"

#define BREAK utils::break_here(__FILE__, __func__, __LINE__)

namespace utils {
void break_here(const char *file, const char *func, int line);
void throw_if_non_zero(int result);
void wiredtiger_cleanup(const std::string &db_home);

class shared_library {
    WT_DLH *dlh_ = nullptr;

public:
    ~shared_library()
    {
        if (dlh_ != nullptr) {
            WT_IGNORE_RET(__wt_dlclose(nullptr, dlh_));
        }
        dlh_ = nullptr;
    }

    explicit shared_library(const char *library_path)
    {
        int ret = __wt_dlopen(nullptr, library_path, &dlh_);
        throw_if_non_zero(ret);
    }

    shared_library(const shared_library &) = delete;
    shared_library &operator=(const shared_library &) = delete;

    template <typename SymbolT>
    SymbolT
    get(const char *symbol_name) const
    {
        SymbolT symbol = nullptr;
        int ret = __wt_dlsym(nullptr, dlh_, symbol_name, true, &symbol);
        throw_if_non_zero(ret);
        return symbol;
    }
};

} // namespace utils.
