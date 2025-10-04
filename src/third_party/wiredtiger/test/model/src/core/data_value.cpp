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
#include "model/util.h"
#include "wiredtiger.h"

extern "C" {
#include "wt_internal.h"
}

namespace model {

/*
 * NONE --
 *     The "None" value.
 */
const data_value NONE = data_value::create_none();

/*
 * ZERO --
 *     The unsigned zero value.
 */
const data_value ZERO = data_value((uint64_t)0);

/*
 * item_to_byte_vector --
 *     Unpack a WiredTiger byte buffer into a byte vector.
 */
inline static byte_vector
item_to_byte_vector(const WT_ITEM &item)
{
    const uint8_t *p = (const uint8_t *)item.data;
    return byte_vector(p, p + item.size);
}

/*
 * data_value::unpack --
 *     Unpack a WiredTiger buffer into a data value.
 */
data_value
data_value::unpack(const void *buffer, size_t length, const char *format)
{
    if (buffer == NULL)
        throw model_exception("NULL buffer");
    if (format == NULL)
        throw model_exception("NULL format");
    if (*format == '\0')
        throw model_exception("Empty format");

    /* Remove length from the format. */
    if (isdigit(format[0]))
        (void)parse_uint64(format, &format);

    if (strlen(format) != 1)
        throw model_exception("The model does not currently support structs");

    int ret = 0;

    /*
     * UNPACK_TO_DATA_VALUE --
     *     Unpack the given raw buffer value into a data_value.
     */
#define UNPACK_TO_DATA_VALUE(wt_type, c_type, cast)                               \
    case wt_type[0]: {                                                            \
        c_type v;                                                                 \
        /* It is okay to use NULL session, as this is just for error handling. */ \
        ret = __wt_struct_unpack(nullptr, buffer, length, wt_type, &v);           \
        if (ret == 0)                                                             \
            return data_value(cast(v));                                           \
        throw model_exception("Failed to unpack data_value");                     \
    }

    switch (format[0]) {
        UNPACK_TO_DATA_VALUE("b", int8_t, static_cast<int64_t>);
        UNPACK_TO_DATA_VALUE("B", uint8_t, static_cast<uint64_t>);
        UNPACK_TO_DATA_VALUE("h", int16_t, static_cast<int64_t>);
        UNPACK_TO_DATA_VALUE("H", uint16_t, static_cast<uint64_t>);
        UNPACK_TO_DATA_VALUE("i", int32_t, static_cast<int64_t>);
        UNPACK_TO_DATA_VALUE("I", uint32_t, static_cast<uint64_t>);
        UNPACK_TO_DATA_VALUE("l", int32_t, static_cast<int64_t>);
        UNPACK_TO_DATA_VALUE("L", uint32_t, static_cast<uint64_t>);
        UNPACK_TO_DATA_VALUE("q", int64_t, static_cast<int64_t>);
        UNPACK_TO_DATA_VALUE("Q", uint64_t, static_cast<uint64_t>);
        UNPACK_TO_DATA_VALUE("r", uint64_t, static_cast<uint64_t>);
        UNPACK_TO_DATA_VALUE("s", const char *, std::string);
        UNPACK_TO_DATA_VALUE("S", const char *, std::string);
        UNPACK_TO_DATA_VALUE("t", uint8_t, static_cast<uint64_t>);
        UNPACK_TO_DATA_VALUE("u", WT_ITEM, item_to_byte_vector);
    case 'x':
        throw model_exception("Type \"x\" is not implemented.");
    default:
        throw model_exception("Unknown type.");
    };
#undef UNPACK_TO_DATA_VALUE
}

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
    else if (std::holds_alternative<byte_vector>(*this))
        return "u";
    else {
        assert(!"Invalid code path unexpected data_value type");
        return ""; /* Make gcc happy. */
    }
}

/*
 * operator<< --
 *     Add human-readable output to the stream for a byte vector.
 */
std::ostream &
operator<<(std::ostream &out, const byte_vector &data)
{
    char buf[4];
    for (size_t i = 0; i < data.size(); i++) {
        if (i > 0)
            out << " ";
        (void)snprintf(buf, sizeof(buf), "%02" PRIx8, data[i]);
        out << buf;
    }
    return out;
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
    else if (std::holds_alternative<byte_vector>(value))
        out << std::get<byte_vector>(value);
    else
        assert(!"Invalid code path unexpected data_value type");

    return out;
}

/*
 * get_wt_cursor_key_or_value --
 *     Get the key or value from a WiredTiger cursor.
 */
inline static data_value
get_wt_cursor_key_or_value(
  WT_CURSOR *cursor, int get_fn(WT_CURSOR *cursor, ...), const char *format)
{
    if (format == NULL)
        format = "u";
    if (*format == '\0')
        throw model_exception("Empty format");

    /* Remove length from the format. */
    if (isdigit(format[0]))
        (void)parse_uint64(format, &format);

    if (strlen(format) != 1)
        throw model_exception("The model does not currently support structs");

    int ret = 0;

    /*
     * GET_DATA_VALUE --
     *     Read the given key or value into a data_value.
     */
#define GET_DATA_VALUE(wt_type, c_type, cast) \
    case wt_type[0]: {                        \
        c_type v;                             \
        ret = get_fn(cursor, &v);             \
        if (ret == 0)                         \
            return data_value(cast(v));       \
    }

    switch (format[0]) {
        GET_DATA_VALUE("b", int8_t, static_cast<int64_t>);
        GET_DATA_VALUE("B", uint8_t, static_cast<uint64_t>);
        GET_DATA_VALUE("h", int16_t, static_cast<int64_t>);
        GET_DATA_VALUE("H", uint16_t, static_cast<uint64_t>);
        GET_DATA_VALUE("i", int32_t, static_cast<int64_t>);
        GET_DATA_VALUE("I", uint32_t, static_cast<uint64_t>);
        GET_DATA_VALUE("l", int32_t, static_cast<int64_t>);
        GET_DATA_VALUE("L", uint32_t, static_cast<uint64_t>);
        GET_DATA_VALUE("q", int64_t, static_cast<int64_t>);
        GET_DATA_VALUE("Q", uint64_t, static_cast<uint64_t>);
        GET_DATA_VALUE("r", uint64_t, static_cast<uint64_t>);
        GET_DATA_VALUE("s", const char *, std::string);
        GET_DATA_VALUE("S", const char *, std::string);
        GET_DATA_VALUE("t", uint8_t, static_cast<uint64_t>);
        GET_DATA_VALUE("u", WT_ITEM, item_to_byte_vector);
    case 'x':
        throw model_exception("Type \"x\" is not implemented.");
    default:
        throw model_exception("Unknown type.");
    };
#undef GET_DATA_VALUE
}

/*
 * get_wt_cursor_key --
 *     Get the key from a WiredTiger cursor.
 */
data_value
get_wt_cursor_key(WT_CURSOR *cursor)
{
    return get_wt_cursor_key_or_value(cursor, cursor->get_key, cursor->key_format);
}

/*
 * get_wt_cursor_value --
 *     Get the value from a WiredTiger cursor.
 */
data_value
get_wt_cursor_value(WT_CURSOR *cursor)
{
    return get_wt_cursor_key_or_value(cursor, cursor->get_value, cursor->value_format);
}

/*
 * set_wt_cursor_key_or_value --
 *     Set the value as WiredTiger cursor key or value.
 */
inline static void
set_wt_cursor_key_or_value(WT_CURSOR *cursor, void set_fn(WT_CURSOR *cursor, ...),
  const char *format, const data_value &value)
{
    if (format == NULL)
        format = "u";
    if (*format == '\0')
        throw model_exception("Empty format");

    /* Remove length from the format. */
    if (isdigit(format[0]))
        (void)parse_uint64(format, &format);

    if (strlen(format) != 1)
        throw model_exception("The model does not currently support structs");
    if (std::holds_alternative<std::monostate>(value) || value.valueless_by_exception())
        throw model_exception("Cannot use the NONE value in a WiredTiger cursor");

        /*
         * SET_DATA_VALUE --
         *     Read the given key or value into a data_value.
         */
#define SET_DATA_VALUE(wt_type, c_type, variant_type)              \
    case wt_type[0]: {                                             \
        if (!std::holds_alternative<variant_type>(value))          \
            throw model_exception("Incompatible data_value type"); \
        variant_type v = std::get<variant_type>(value);            \
        set_fn(cursor, static_cast<c_type>(v));                    \
        break;                                                     \
    }

    switch (format[0]) {
        SET_DATA_VALUE("b", int8_t, int64_t);
        SET_DATA_VALUE("B", uint8_t, uint64_t);
        SET_DATA_VALUE("h", int16_t, int64_t);
        SET_DATA_VALUE("H", uint16_t, uint64_t);
        SET_DATA_VALUE("i", int32_t, int64_t);
        SET_DATA_VALUE("I", uint32_t, uint64_t);
        SET_DATA_VALUE("l", int32_t, int64_t);
        SET_DATA_VALUE("L", uint32_t, uint64_t);
        SET_DATA_VALUE("q", int64_t, int64_t);
        SET_DATA_VALUE("Q", uint64_t, uint64_t);
        SET_DATA_VALUE("r", uint64_t, uint64_t);
        SET_DATA_VALUE("t", uint8_t, uint64_t);
    case 's':
    case 'S': {
        if (!std::holds_alternative<std::string>(value))
            throw model_exception("Incompatible data_value type");
        set_fn(cursor, std::get<std::string>(value).c_str());
        break;
    }
    case 'u': {
        if (!std::holds_alternative<byte_vector>(value))
            throw model_exception("Incompatible data_value type");
        WT_ITEM item;
        memset(&item, 0, sizeof(item));
        item.data = std::get<byte_vector>(value).data();
        item.size = std::get<byte_vector>(value).size();
        /*
         * It is safe to pass the pointer to WT_ITEM, because the implementation will only use its
         * data and size fields, without keeping a reference to the actual WT_ITEM.
         */
        set_fn(cursor, &item);
        break;
    }
    case 'x':
        throw model_exception("Type \"x\" is not implemented.");
    default:
        throw model_exception("Unknown type.");
    };
#undef SET_DATA_VALUE
}

/*
 * set_wt_cursor_key --
 *     Set the value as WiredTiger cursor key.
 */
void
set_wt_cursor_key(WT_CURSOR *cursor, const data_value &value)
{
    set_wt_cursor_key_or_value(cursor, cursor->set_key, cursor->key_format, value);
}

/*
 * set_wt_cursor_value --
 *     Set the value as WiredTiger cursor value.
 */
void
set_wt_cursor_value(WT_CURSOR *cursor, const data_value &value)
{
    set_wt_cursor_key_or_value(cursor, cursor->set_value, cursor->value_format, value);
}

} /* namespace model */
