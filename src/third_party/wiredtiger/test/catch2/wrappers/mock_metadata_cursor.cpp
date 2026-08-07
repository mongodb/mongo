/*-
 * Copyright (c) 2014-present MongoDB, Inc.
 * Copyright (c) 2008-2014 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

#include <cstdarg>

#include "mock_metadata_cursor.h"

namespace {

mock_metadata_cursor &
to_mock_cursor(WT_CURSOR *cursor)
{
    return *static_cast<mock_metadata_cursor *>(cursor->lang_private);
}

} // namespace

mock_metadata_cursor::mock_metadata_cursor()
{
    _cursor.lang_private = this;
    _cursor.set_key = set_key;
    _cursor.search = search;
    _cursor.get_value = get_value;
    _cursor.reset = reset;
}

WT_CURSOR &
mock_metadata_cursor::cursor()
{
    return _cursor;
}

void
mock_metadata_cursor::insert_metadata(std::string_view uri, std::string_view value)
{
    _metadata.insert_or_assign(std::string(uri), std::string(value));
}

void
mock_metadata_cursor::insert_metadata_error(std::string_view uri, int error)
{
    _metadata.insert_or_assign(std::string(uri), error);
}

void
mock_metadata_cursor::set_key(WT_CURSOR *cursor, ...)
{
    va_list ap;
    va_start(ap, cursor);
    to_mock_cursor(cursor)._search_key = va_arg(ap, const char *);
    va_end(ap);
}

int
mock_metadata_cursor::search(WT_CURSOR *cursor)
{
    auto &mock = to_mock_cursor(cursor);
    mock._search_result.reset();

    const auto it = mock._metadata.find(mock._search_key);
    if (it == mock._metadata.end())
        return WT_NOTFOUND;

    const auto *value = std::get_if<std::string>(&it->second);
    if (value == nullptr)
        return std::get<int>(it->second);

    mock._search_result = *value;
    return 0;
}

int
mock_metadata_cursor::get_value(WT_CURSOR *cursor, ...)
{
    const auto &mock = to_mock_cursor(cursor);
    if (!mock._search_result.has_value())
        return EINVAL;

    va_list ap;
    va_start(ap, cursor);
    *va_arg(ap, const char **) = mock._search_result->c_str();
    va_end(ap);
    return 0;
}

int
mock_metadata_cursor::reset(WT_CURSOR *cursor)
{
    auto &mock = to_mock_cursor(cursor);
    mock._search_key = {};
    mock._search_result.reset();
    return 0;
}
