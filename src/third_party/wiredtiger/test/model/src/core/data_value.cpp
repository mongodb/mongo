/*-
 * Public Domain 2014-present MongoDB, Inc.
 * Public Domain 2008-2014 WiredTiger, Inc.
 *
 * This is free and unencumbered software released into the public domain.
 *
 * Anyone is free to copy, modify, publish, use, compile, sell, or
 * distribute this software, either in source code form or as a compiled
 * binary, for any purpose, commercial or non-commercial, and by any
 * means.
 *
 * In jurisdictions that recognize copyright laws, the author or authors
 * of this software dedicate any and all copyright interest in the
 * software to the public domain. We make this dedication for the benefit
 * of the public at large and to the detriment of our heirs and
 * successors. We intend this dedication to be an overt act of
 * relinquishment in perpetuity of all present and future rights to this
 * software under copyright law.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.
 * IN NO EVENT SHALL THE AUTHORS BE LIABLE FOR ANY CLAIM, DAMAGES OR
 * OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE,
 * ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR
 * OTHER DEALINGS IN THE SOFTWARE.
 */

#include <algorithm>
#include <cassert>
#include <iostream>

#include "model/data_value.h"
#include "wiredtiger.h"

namespace model {

/*
 * NONE --
 *     The "None" value.
 */
const data_value NONE = data_value::create_none();

/*
 * data_value::wt_type --
 *     Get the WiredTiger type.
 */
const char *
data_value::wt_type() const
{
    if (std::holds_alternative<std::monostate>(*this) || this->valueless_by_exception())
        return "";
    else if (std::holds_alternative<int64_t>(*this))
        return "q";
    else if (std::holds_alternative<uint64_t>(*this))
        return "Q";
    else if (std::holds_alternative<std::string>(*this))
        return "S";
    else {
        assert(!"Invalid code path unexpected data_value type");
        return ""; /* Make gcc happy. */
    }
}

/*
 * operator<< --
 *     Add human-readable output to the stream.
 */
std::ostream &
operator<<(std::ostream &out, const data_value &value)
{
    if (std::holds_alternative<std::monostate>(value) || value.valueless_by_exception())
        out << "(none)";
    else if (std::holds_alternative<int64_t>(value))
        out << std::get<int64_t>(value);
    else if (std::holds_alternative<uint64_t>(value))
        out << std::get<uint64_t>(value);
    else if (std::holds_alternative<std::string>(value))
        out << std::get<std::string>(value);
    else
        assert(!"Invalid code path unexpected data_value type");

    return out;
}

/*
 * set_wt_cursor_key_or_value --
 *     Set the value as WiredTiger cursor key or value.
 */
inline static void
set_wt_cursor_key_or_value(
  WT_CURSOR *cursor, void set_fn(WT_CURSOR *cursor, ...), const data_value &value)
{
    if (std::holds_alternative<std::monostate>(value) || value.valueless_by_exception())
        set_fn(cursor);
    else if (std::holds_alternative<int64_t>(value))
        set_fn(cursor, std::get<int64_t>(value));
    else if (std::holds_alternative<uint64_t>(value))
        set_fn(cursor, std::get<uint64_t>(value));
    else if (std::holds_alternative<std::string>(value))
        set_fn(cursor, std::get<std::string>(value).c_str());
    else
        assert(!"Invalid code path unexpected data_value type");
}

/*
 * set_wt_cursor_key --
 *     Set the value as WiredTiger cursor key.
 */
void
set_wt_cursor_key(WT_CURSOR *cursor, const data_value &value)
{
    set_wt_cursor_key_or_value(cursor, cursor->set_key, value);
}

/*
 * set_wt_cursor_value --
 *     Set the value as WiredTiger cursor value.
 */
void
set_wt_cursor_value(WT_CURSOR *cursor, const data_value &value)
{
    set_wt_cursor_key_or_value(cursor, cursor->set_value, value);
}

} /* namespace model */
