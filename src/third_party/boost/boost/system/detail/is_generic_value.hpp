#ifndef BOOST_SYSTEM_DETAIL_IS_GENERIC_VALUE_HPP_INCLUDED
#define BOOST_SYSTEM_DETAIL_IS_GENERIC_VALUE_HPP_INCLUDED

// Copyright 2018 Peter Dimov
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
// See library home page at http://www.boost.org/libs/system

#include <boost/system/detail/errc.hpp>

namespace boost
{

namespace system
{

namespace detail
{

inline bool is_generic_value( int ev ) BOOST_NOEXCEPT
{
    using namespace errc;

    static int const gen[] =
    {
        success,
        address_family_not_supported,
        address_in_use,
        address_not_available,
        already_connected,
        argument_list_too_long,
        argument_out_of_domain,
        bad_address,
        bad_file_descriptor,
        bad_message,
        broken_pipe,
        connection_aborted,
        connection_already_in_progress,
        connection_refused,
        connection_reset,
        cross_device_link,
        destination_address_required,
        device_or_resource_busy,
        directory_not_empty,
        executable_format_error,
        file_exists,
        file_too_large,
        filename_too_long,
        function_not_supported,
        host_unreachable,
        identifier_removed,
        illegal_byte_sequence,
        inappropriate_io_control_operation,
        interrupted,
        invalid_argument,
        invalid_seek,
        io_error,
        is_a_directory,
        message_size,
        network_down,
        network_reset,
        network_unreachable,
        no_buffer_space,
        no_child_process,
        no_link,
        no_lock_available,
        no_message_available,
        no_message,
        no_protocol_option,
        no_space_on_device,
        no_stream_resources,
        no_such_device_or_address,
        no_such_device,
        no_such_file_or_directory,
        no_such_process,
        not_a_directory,
        not_a_socket,
        not_a_stream,
        not_connected,
        not_enough_memory,
        not_supported,
        operation_canceled,
        operation_in_progress,
        operation_not_permitted,
        operation_not_supported,
        operation_would_block,
        owner_dead,
        permission_denied,
        protocol_error,
        protocol_not_supported,
        read_only_file_system,
        resource_deadlock_would_occur,
        resource_unavailable_try_again,
        result_out_of_range,
        state_not_recoverable,
        stream_timeout,
        text_file_busy,
        timed_out,
        too_many_files_open_in_system,
        too_many_files_open,
        too_many_links,
        too_many_symbolic_link_levels,
        value_too_large,
        wrong_protocol_type
    };

    int const n = sizeof( gen ) / sizeof( gen[0] );

    for( int i = 0; i < n; ++i )
    {
        if( ev == gen[ i ] ) return true;
    }

    return false;
}

} // namespace detail

} // namespace system

} // namespace boost

#endif // #ifndef BOOST_SYSTEM_DETAIL_IS_GENERIC_VALUE_HPP_INCLUDED
