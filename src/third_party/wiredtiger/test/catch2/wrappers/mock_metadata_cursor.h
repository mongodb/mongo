/*-
 * Copyright (c) 2014-present MongoDB, Inc.
 * Copyright (c) 2008-2014 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

#pragma once

#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <variant>

#include "wt_internal.h"

class mock_metadata_cursor {
    using metadata_result = std::variant<std::string, int>;

public:
    mock_metadata_cursor();

    [[nodiscard]] WT_CURSOR &cursor();

    void insert_metadata(std::string_view uri, std::string_view value);
    void insert_metadata_error(std::string_view uri, int error);

private:
    static void set_key(WT_CURSOR *cursor, ...);
    static int search(WT_CURSOR *cursor);
    static int get_value(WT_CURSOR *cursor, ...);
    static int reset(WT_CURSOR *cursor);

    WT_CURSOR _cursor{};
    std::unordered_map<std::string, metadata_result> _metadata;
    std::string _search_key;
    std::optional<std::string> _search_result;
};
