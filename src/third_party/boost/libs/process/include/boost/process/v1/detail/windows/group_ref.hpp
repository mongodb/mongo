// Copyright (c) 2016 Klemens D. Morgenstern
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)

#ifndef BOOST_PROCESS_DETAIL_WINDOWS_GROUP_REF_HPP_
#define BOOST_PROCESS_DETAIL_WINDOWS_GROUP_REF_HPP_

#include <boost/winapi/process.hpp>
#include <boost/process/v1/detail/config.hpp>
#include <boost/process/v1/detail/windows/group_handle.hpp>
#include <boost/process/v1/detail/used_handles.hpp>
#include <boost/process/v1/detail/windows/handler.hpp>

namespace boost { namespace process { BOOST_PROCESS_V1_INLINE namespace v1 {

namespace detail { namespace windows {



struct group_ref : handler_base_ext, ::boost::process::v1::detail::uses_handles
{
    ::boost::winapi::HANDLE_ handle;

    ::boost::winapi::HANDLE_ get_used_handles() const { return handle; }

    explicit group_ref(group_handle &g) :
                handle(g.handle())
    {}

    template <class Executor>
    void on_setup(Executor& exec) const
    {
        //I can only enable this if the current process supports breakaways.
        if (in_group() && break_away_enabled(nullptr))
            exec.creation_flags  |= boost::winapi::CREATE_BREAKAWAY_FROM_JOB_;
    }


    template <class Executor>
    void on_success(Executor& exec) const
    {
        if (!::boost::winapi::AssignProcessToJobObject(handle, exec.proc_info.hProcess))
            exec.set_error(::boost::process::v1::detail::get_last_error(),
                           "AssignProcessToJobObject() failed.");

    }

};

}}}}}


#endif /* BOOST_PROCESS_DETAIL_WINDOWS_GROUP_HPP_ */
