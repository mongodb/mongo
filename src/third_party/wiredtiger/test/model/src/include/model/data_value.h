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

#ifndef MODEL_DATA_VALUE_H
#define MODEL_DATA_VALUE_H

#include <ostream>
#include <string>
#include <variant>

#include "model/core.h"
#include "wiredtiger.h"

namespace model {

/*
 * base_data_value --
 *     The base type for data values, which is an std::variant of all the relevant types that we
 *     support.
 */
using base_data_value = std::variant<std::monostate, int64_t, uint64_t, std::string>;

/*
 * data_value --
 *     The data value stored in the model used for keys and values. We use a generic class, rather
 *     than a specific type such as std::string, to give us flexibility to change data types in the
 *     future, e.g., if this becomes necessary to explore additional code paths. This class is
 *     intended to parallel WiredTiger's WT_ITEM, which supports multiple data types, plus the
 *     ability to specify a NONE value to simplify modeling deleted data.
 */
class data_value : public base_data_value {

public:
    /*
     * data_value::data_value --
     *     Create a new instance.
     */
    inline data_value(const base_data_value &data) : base_data_value(data) {}

    /*
     * data_value::create_none --
     *     Create an instance of a "None" value.
     */
    inline static data_value
    create_none() noexcept
    {
        return data_value();
    }

    /*
     * data_value::none --
     *     Check if this is a None value.
     */
    inline bool
    none() const noexcept
    {
        return std::holds_alternative<std::monostate>(*this);
    }

    /*
     * data_value::wt_type --
     *     Get the WiredTiger type.
     */
    const char *wt_type() const;

private:
    /*
     * data_value::data_value --
     *     Create a new instance.
     */
    inline data_value() : base_data_value(std::monostate{}) {}
};

/*
 * NONE --
 *     The "None" value.
 */
extern const data_value NONE;

/*
 * operator<< --
 *     Add human-readable output to the stream.
 */
std::ostream &operator<<(std::ostream &out, const data_value &value);

/*
 * set_wt_cursor_key --
 *     Set the value as WiredTiger cursor key.
 */
void set_wt_cursor_key(WT_CURSOR *cursor, const data_value &value);

/*
 * set_wt_cursor_value --
 *     Set the value as WiredTiger cursor value.
 */
void set_wt_cursor_value(WT_CURSOR *cursor, const data_value &value);

} /* namespace model */
#endif
