// ----------------------------------------------------------------------------
//  format_fwd.hpp :  forward declarations
// ----------------------------------------------------------------------------

//  Copyright Samuel Krempp 2003. Use, modification, and distribution are
//  subject to the Boost Software License, Version 1.0. (See accompanying
//  file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)

//  See http://www.boost.org/libs/format for library home page

// ----------------------------------------------------------------------------

#ifndef BOOST_FORMAT_FWD_HPP
#define BOOST_FORMAT_FWD_HPP

#include <string>
#include <iosfwd>

#include <boost/format/detail/compat_workarounds.hpp> 

namespace boost {

    template <class Ch, 
#if !( BOOST_WORKAROUND(__GNUC__, <3) && !defined(__SGI_STL_PORT) && !defined(_STLPORT_VERSION) )
    // gcc-2.95's native  stdlid needs special treatment
        class Tr = BOOST_IO_STD char_traits<Ch>, class Alloc = std::allocator<Ch> > 
#else
        class Tr = std::string_char_traits<Ch>, class Alloc = std::alloc > 
#endif
    class basic_format;

    typedef basic_format<char >     format;

#if !defined(BOOST_NO_STD_WSTRING)  && !defined(BOOST_NO_STD_WSTREAMBUF) \
    && !defined(BOOST_FORMAT_IGNORE_STRINGSTREAM)
    typedef basic_format<wchar_t >  wformat;
#endif

    namespace io {
        enum format_error_bits { bad_format_string_bit = 1, 
                                 too_few_args_bit = 2, too_many_args_bit = 4,
                                 out_of_range_bit = 8,
                                 all_error_bits = 255, no_error_bits=0 };
                  
    } // namespace io

} // namespace boost

#endif // BOOST_FORMAT_FWD_HPP
