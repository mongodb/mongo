// Copyright (c) 2021 Klemens D. Morgenstern
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
#ifndef BOOST_PROCESS_V2_ERROR_HPP
#define BOOST_PROCESS_V2_ERROR_HPP

#include <boost/process/v2/detail/config.hpp>

BOOST_PROCESS_V2_BEGIN_NAMESPACE

namespace error
{

/// Errors used for utf8 <-> UCS-2 conversions.
enum utf8_conv_error
{
    insufficient_buffer = 1,
    invalid_character,
};

extern BOOST_PROCESS_V2_DECL const error_category& get_utf8_category();
static const error_category& utf8_category = get_utf8_category();

extern BOOST_PROCESS_V2_DECL const error_category& get_exit_code_category();

/// An error category that can be used to interpret exit codes of subprocesses.
/** Currently not used by boost.process, but it might be in the future.
 * 
 * void run_my_process(filesystem::path pt, error_code & ec)
 * {
 *     process proc(pt, {});
 *     proc.wait();
 *     ec.assign(proc.native_exit_code(), error::get_exit_code_category());
 * }
 * 
 * */
static const error_category& exit_code_category = get_exit_code_category();

}

BOOST_PROCESS_V2_END_NAMESPACE

#endif //BOOST_PROCESS_V2_ERROR_HPP
