//  filesystem path_traits.cpp  --------------------------------------------------------//

//  Copyright Beman Dawes 2008, 2009

//  Distributed under the Boost Software License, Version 1.0.
//  See http://www.boost.org/LICENSE_1_0.txt

//  Library home page: http://www.boost.org/libs/filesystem

//--------------------------------------------------------------------------------------//

#include "platform_config.hpp"

#include <boost/filesystem/config.hpp>
#include <boost/filesystem/detail/path_traits.hpp>
#include <boost/filesystem/path.hpp>
#include <boost/system/system_error.hpp>
#include <boost/assert.hpp>
#include <memory>
#include <string>
#include <locale>  // for codecvt_base::result
#include <cwchar>  // for mbstate_t
#include <cstddef>

#include <boost/filesystem/detail/header.hpp> // must be the last #include

namespace pt = boost::filesystem::detail::path_traits;
namespace fs = boost::filesystem;
namespace bs = boost::system;

//--------------------------------------------------------------------------------------//
//                                  configuration                                       //
//--------------------------------------------------------------------------------------//

#ifndef BOOST_FILESYSTEM_CODECVT_BUF_SIZE
#define BOOST_FILESYSTEM_CODECVT_BUF_SIZE 256
#endif

namespace {

BOOST_CONSTEXPR_OR_CONST std::size_t default_codecvt_buf_size = BOOST_FILESYSTEM_CODECVT_BUF_SIZE;

//--------------------------------------------------------------------------------------//
//                                                                                      //
//  The public convert() functions do buffer management, and then forward to the        //
//  convert_aux() functions for the actual call to the codecvt facet.                   //
//                                                                                      //
//--------------------------------------------------------------------------------------//

//--------------------------------------------------------------------------------------//
//                      convert_aux const char* to wstring                             //
//--------------------------------------------------------------------------------------//

void convert_aux(const char* from, const char* from_end, wchar_t* to, wchar_t* to_end, std::wstring& target, pt::codecvt_type const& cvt)
{
    //std::cout << std::hex
    //          << " from=" << std::size_t(from)
    //          << " from_end=" << std::size_t(from_end)
    //          << " to=" << std::size_t(to)
    //          << " to_end=" << std::size_t(to_end)
    //          << std::endl;

    std::mbstate_t state = std::mbstate_t(); // perhaps unneeded, but cuts bug reports
    const char* from_next;
    wchar_t* to_next;

    std::codecvt_base::result res;

    if ((res = cvt.in(state, from, from_end, from_next, to, to_end, to_next)) != std::codecvt_base::ok)
    {
        //std::cout << " result is " << static_cast<int>(res) << std::endl;
        BOOST_FILESYSTEM_THROW(bs::system_error(res, fs::codecvt_error_category(), "boost::filesystem::path codecvt to wstring"));
    }
    target.append(to, to_next);
}

//--------------------------------------------------------------------------------------//
//                      convert_aux const wchar_t* to string                           //
//--------------------------------------------------------------------------------------//

void convert_aux(const wchar_t* from, const wchar_t* from_end, char* to, char* to_end, std::string& target, pt::codecvt_type const& cvt)
{
    //std::cout << std::hex
    //          << " from=" << std::size_t(from)
    //          << " from_end=" << std::size_t(from_end)
    //          << " to=" << std::size_t(to)
    //          << " to_end=" << std::size_t(to_end)
    //          << std::endl;

    std::mbstate_t state = std::mbstate_t(); // perhaps unneeded, but cuts bug reports
    const wchar_t* from_next;
    char* to_next;

    std::codecvt_base::result res;

    if ((res = cvt.out(state, from, from_end, from_next, to, to_end, to_next)) != std::codecvt_base::ok)
    {
        //std::cout << " result is " << static_cast<int>(res) << std::endl;
        BOOST_FILESYSTEM_THROW(bs::system_error(res, fs::codecvt_error_category(), "boost::filesystem::path codecvt to string"));
    }
    target.append(to, to_next);
}

} // unnamed namespace

//--------------------------------------------------------------------------------------//
//                                   path_traits                                        //
//--------------------------------------------------------------------------------------//

namespace boost {
namespace filesystem {
namespace detail {
namespace path_traits {

//--------------------------------------------------------------------------------------//
//                          convert const char* to wstring                              //
//--------------------------------------------------------------------------------------//

BOOST_FILESYSTEM_DECL
void convert(const char* from, const char* from_end, std::wstring& to, const codecvt_type* cvt)
{
    if (from == from_end)
        return;

    BOOST_ASSERT(from != nullptr);
    BOOST_ASSERT(from_end != nullptr);

    if (!cvt)
        cvt = &fs::path::codecvt();

    std::size_t buf_size = (from_end - from) * 3; // perhaps too large, but that's OK

    //  dynamically allocate a buffer only if source is unusually large
    if (buf_size > default_codecvt_buf_size)
    {
        std::unique_ptr< wchar_t[] > buf(new wchar_t[buf_size]);
        convert_aux(from, from_end, buf.get(), buf.get() + buf_size, to, *cvt);
    }
    else
    {
        wchar_t buf[default_codecvt_buf_size];
        convert_aux(from, from_end, buf, buf + default_codecvt_buf_size, to, *cvt);
    }
}

//--------------------------------------------------------------------------------------//
//                         convert const wchar_t* to string                            //
//--------------------------------------------------------------------------------------//

BOOST_FILESYSTEM_DECL
void convert(const wchar_t* from, const wchar_t* from_end, std::string& to, const codecvt_type* cvt)
{
    if (from == from_end)
        return;

    BOOST_ASSERT(from != nullptr);
    BOOST_ASSERT(from_end != nullptr);

    if (!cvt)
        cvt = &fs::path::codecvt();

    //  The codecvt length functions may not be implemented, and I don't really
    //  understand them either. Thus this code is just a guess; if it turns
    //  out the buffer is too small then an error will be reported and the code
    //  will have to be fixed.
    std::size_t buf_size = (from_end - from) * 4; // perhaps too large, but that's OK
    buf_size += 4;                                // encodings like shift-JIS need some prefix space

    //  dynamically allocate a buffer only if source is unusually large
    if (buf_size > default_codecvt_buf_size)
    {
        std::unique_ptr< char[] > buf(new char[buf_size]);
        convert_aux(from, from_end, buf.get(), buf.get() + buf_size, to, *cvt);
    }
    else
    {
        char buf[default_codecvt_buf_size];
        convert_aux(from, from_end, buf, buf + default_codecvt_buf_size, to, *cvt);
    }
}

} // namespace path_traits
} // namespace detail
} // namespace filesystem
} // namespace boost

#include <boost/filesystem/detail/footer.hpp>
