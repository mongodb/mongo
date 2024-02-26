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

#pragma once

#include <limits>
#include <stdexcept>
#include <string>
#include "wiredtiger.h"

/* Redefine important WiredTiger internal constants, if they are not already available. */

/*
 * WT_TS_MAX --
 *     The maximum timestamp, typically used in reads where we would like to get the latest value.
 */
#ifndef WT_TS_MAX
#define WT_TS_MAX UINT64_MAX
#endif

/*
 * WT_TS_NONE --
 *     No timestamp, e.g., when performing a non-timestamped update.
 */
#ifndef WT_TS_NONE
#define WT_TS_NONE 0
#endif

/*
 * WT_TXN_NONE --
 *     No transaction ID.
 */
#ifndef WT_TXN_NONE
#define WT_TXN_NONE 0
#endif

namespace model {

/*
 * timestamp_t --
 *     The timestamp. This is the model's equivalent of wt_timestamp_t.
 */
using timestamp_t = uint64_t;

/*
 * k_timestamp_none --
 *     No timestamp, e.g., when performing a non-timestamped update.
 */
constexpr timestamp_t k_timestamp_none = std::numeric_limits<timestamp_t>::min();

/*
 * k_timestamp_max --
 *     The maximum timestamp, typically used in reads where we would like to get the latest value.
 */
constexpr timestamp_t k_timestamp_max = std::numeric_limits<timestamp_t>::max();

/*
 * k_timestamp_latest --
 *     A convenience alias for k_timestamp_max, typically used to get the latest value.
 */
constexpr timestamp_t k_timestamp_latest = k_timestamp_max;

/* Verify that model's constants are numerically equal to WiredTiger's constants. */
static_assert(k_timestamp_latest == WT_TS_MAX);
static_assert(k_timestamp_max == WT_TS_MAX);
static_assert(k_timestamp_none == WT_TS_NONE);

/*
 * txn_id_t --
 *     The transaction ID.
 */
using txn_id_t = uint64_t;

/*
 * k_txn_none --
 *     No transaction ID.
 */
constexpr txn_id_t k_txn_none = std::numeric_limits<txn_id_t>::min();

/*
 * k_txn_max --
 *     The maximum ID.
 */
constexpr txn_id_t k_txn_max = std::numeric_limits<txn_id_t>::max() - 10;

/* Verify that model's constants are numerically equal to WiredTiger's constants. */
static_assert(k_txn_none == WT_TXN_NONE);
static_assert(k_txn_max == UINT64_MAX - 10);
/* We will check k_txn_max again in the .cpp file, as we don't have the right imports. */

/*
 * write_gen_t --
 *     The write generation number.
 */
using write_gen_t = uint64_t;

/*
 * k_write_gen_none --
 *     No write generation.
 */
constexpr write_gen_t k_write_gen_none = std::numeric_limits<write_gen_t>::min();

/*
 * k_write_gen_first --
 *     The first (initial) write generation.
 */
constexpr write_gen_t k_write_gen_first = k_write_gen_none + 1;

/* Verify that model's constants are numerically equal to WiredTiger's constants. */
static_assert(k_write_gen_none == 0);
static_assert(k_write_gen_first == 1);

/*
 * model_exception --
 *     An exception for model-related errors, which are not meant to faithfully model WiredTiger
 *     errors.
 */
class model_exception : public std::runtime_error {

public:
    /*
     * model_exception::model_exception --
     *     Create a new instance of the exception.
     */
    inline model_exception(const char *message) noexcept : std::runtime_error(message) {}

    /*
     * model_exception::model_exception --
     *     Create a new instance of the exception.
     */
    inline model_exception(const std::string &message) noexcept : std::runtime_error(message) {}
};

/*
 * wiredtiger_exception --
 *     A WiredTiger exception, which is coming either from a C++ wrapper around a WiredTiger
 *     function call, or from the model indicating that the given operation would result in
 *     returning the specified error.
 */
class wiredtiger_exception : public std::runtime_error {

public:
    /*
     * wiredtiger_exception::wiredtiger_exception --
     *     Create a new instance of the exception.
     */
    inline wiredtiger_exception(WT_SESSION *session, const char *message, int error) noexcept
        : std::runtime_error(std::string(message) + session->strerror(session, error)),
          _error(error)
    {
    }

    /*
     * wiredtiger_exception::wiredtiger_exception --
     *     Create a new instance of the exception.
     */
    inline wiredtiger_exception(WT_SESSION *session, int error) noexcept
        : std::runtime_error(session->strerror(session, error)), _error(error)
    {
    }

    /*
     * wiredtiger_exception::wiredtiger_exception --
     *     Create a new instance of the exception. This constructor is not thread-safe.
     */
    inline wiredtiger_exception(const char *message, int error) noexcept
        : std::runtime_error(std::string(message) + wiredtiger_strerror(error)), _error(error)
    {
    }

    /*
     * wiredtiger_exception::wiredtiger_exception --
     *     Create a new instance of the exception. This constructor is not thread-safe.
     */
    inline wiredtiger_exception(int error) noexcept
        : std::runtime_error(wiredtiger_strerror(error)), _error(error)
    {
    }

    /*
     * wiredtiger_exception::error --
     *     Get the error code.
     */
    inline int
    error() const noexcept
    {
        return _error;
    }

private:
    int _error;
};

/*
 * wiredtiger_abort_exception --
 *     An exception that models that WiredTiger would abort or panic, either at the point when this
 *     exception is thrown or in the future (e.g., during reconciliation).
 */
class wiredtiger_abort_exception : public std::runtime_error {

public:
    /*
     * wiredtiger_abort_exception::wiredtiger_abort_exception --
     *     Create a new instance of the exception.
     */
    inline wiredtiger_abort_exception(const char *message) noexcept : std::runtime_error(message) {}

    /*
     * wiredtiger_abort_exception::wiredtiger_abort_exception --
     *     Create a new instance of the exception.
     */
    inline wiredtiger_abort_exception(const std::string &message) noexcept
        : std::runtime_error(message)
    {
    }

    /*
     * wiredtiger_abort_exception::wiredtiger_abort_exception --
     *     Create a new instance of the exception.
     */
    inline wiredtiger_abort_exception() noexcept : std::runtime_error("WiredTiger would abort") {}
};

} /* namespace model */
