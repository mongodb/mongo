//  filesystem windows_file_codecvt.hpp  -----------------------------------------------//

//  Copyright Beman Dawes 2009

//  Distributed under the Boost Software License, Version 1.0.
//  See http://www.boost.org/LICENSE_1_0.txt

//  Library home page: http://www.boost.org/libs/filesystem

#ifndef BOOST_FILESYSTEM_WINDOWS_FILE_CODECVT_HPP
#define BOOST_FILESYSTEM_WINDOWS_FILE_CODECVT_HPP

#include <boost/filesystem/config.hpp>

#ifdef BOOST_WINDOWS_API

#include <boost/config/workaround.hpp>
#include <cstddef>
#include <cwchar> // std::mbstate_t
#include <locale>

namespace boost {
namespace filesystem {
namespace detail {

//------------------------------------------------------------------------------------//
//                                                                                    //
//                          class windows_file_codecvt                                //
//                                                                                    //
//  Warning: partial implementation; even do_in and do_out only partially meet the    //
//  standard library specifications as the "to" buffer must hold the entire result.   //
//                                                                                    //
//------------------------------------------------------------------------------------//

class BOOST_SYMBOL_VISIBLE windows_file_codecvt BOOST_FINAL :
    public std::codecvt< wchar_t, char, std::mbstate_t >
{
public:
    explicit windows_file_codecvt(std::size_t refs = 0) :
        std::codecvt< wchar_t, char, std::mbstate_t >(refs)
    {
    }

protected:
    bool do_always_noconv() const BOOST_NOEXCEPT_OR_NOTHROW BOOST_OVERRIDE { return false; }

    //  seems safest to assume variable number of characters since we don't
    //  actually know what codepage is active
    int do_encoding() const BOOST_NOEXCEPT_OR_NOTHROW BOOST_OVERRIDE { return 0; }
    std::codecvt_base::result do_in(std::mbstate_t& state, const char* from, const char* from_end, const char*& from_next, wchar_t* to, wchar_t* to_end, wchar_t*& to_next) const BOOST_OVERRIDE;
    std::codecvt_base::result do_out(std::mbstate_t& state, const wchar_t* from, const wchar_t* from_end, const wchar_t*& from_next, char* to, char* to_end, char*& to_next) const BOOST_OVERRIDE;
    std::codecvt_base::result do_unshift(std::mbstate_t&, char* /*from*/, char* /*to*/, char*& /*next*/) const BOOST_OVERRIDE { return ok; }
    int do_length(std::mbstate_t&, const char* /*from*/, const char* /*from_end*/, std::size_t /*max*/) const
#if BOOST_WORKAROUND(__IBMCPP__, BOOST_TESTED_AT(600))
        throw()
#endif
        BOOST_OVERRIDE
    { return 0; }
    int do_max_length() const BOOST_NOEXCEPT_OR_NOTHROW BOOST_OVERRIDE { return 0; }
};

} // namespace detail
} // namespace filesystem
} // namespace boost

#endif // BOOST_WINDOWS_API

#endif // BOOST_FILESYSTEM_WINDOWS_FILE_CODECVT_HPP
