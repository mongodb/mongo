/////////1/////////2/////////3/////////4/////////5/////////6/////////7/////////8
// basic_binary_iprimitive.ipp:

// (C) Copyright 2002 Robert Ramey - http://www.rrsd.com . 
// Use, modification and distribution is subject to the Boost Software
// License, Version 1.0. (See accompanying file LICENSE_1_0.txt or copy at
// http://www.boost.org/LICENSE_1_0.txt)

//  See http://www.boost.org for updates, documentation, and revision history.

#include <cassert>
#include <cstddef> // size_t
#include <cstring> // memcpy

#include <boost/config.hpp>
#if defined(BOOST_NO_STDC_NAMESPACE)
namespace std{ 
    using ::size_t;
    using ::memcpy;
} // namespace std
#endif

#include <boost/detail/workaround.hpp> // fixup for RogueWave

#include <boost/throw_exception.hpp>
#include <boost/scoped_ptr.hpp>

#include <boost/archive/archive_exception.hpp>
#include <boost/archive/codecvt_null.hpp>
#include <boost/archive/add_facet.hpp>

namespace boost {
namespace archive {

//////////////////////////////////////////////////////////////////////
// implementation of basic_binary_iprimitive

template<class Archive, class Elem, class Tr>
BOOST_ARCHIVE_OR_WARCHIVE_DECL(void)
basic_binary_iprimitive<Archive, Elem, Tr>::init()
{
    // Detect  attempts to pass native binary archives across
    // incompatible platforms. This is not fool proof but its
    // better than nothing.
    unsigned char size;
    this->This()->load(size);
    if(sizeof(int) != size)
        boost::throw_exception(
            archive_exception(archive_exception::incompatible_native_format)
        );
    this->This()->load(size);
    if(sizeof(long) != size)
        boost::throw_exception(
            archive_exception(archive_exception::incompatible_native_format)
        );
    this->This()->load(size);
    if(sizeof(float) != size)
        boost::throw_exception(
            archive_exception(archive_exception::incompatible_native_format)
        );
    this->This()->load(size);
    if(sizeof(double) != size)
        boost::throw_exception(
            archive_exception(archive_exception::incompatible_native_format)
        );

    // for checking endian
    int i;
    this->This()->load(i);
    if(1 != i)
        boost::throw_exception(
            archive_exception(archive_exception::incompatible_native_format)
        );
}

template<class Archive, class Elem, class Tr>
BOOST_ARCHIVE_OR_WARCHIVE_DECL(void)
basic_binary_iprimitive<Archive, Elem, Tr>::load(wchar_t * ws)
{
    std::size_t l;
    this->This()->load(l);
    load_binary(ws, l);
    ws[l / sizeof(wchar_t)] = L'\0';
}

template<class Archive, class Elem, class Tr>
BOOST_ARCHIVE_OR_WARCHIVE_DECL(void)
basic_binary_iprimitive<Archive, Elem, Tr>::load(std::string & s)
{
    std::size_t l;
    this->This()->load(l);
    // borland de-allocator fixup
    #if BOOST_WORKAROUND(_RWSTD_VER, BOOST_TESTED_AT(20101))
    if(NULL != s.data())
    #endif
        s.resize(l);
    // note breaking a rule here - could be a problem on some platform
    load_binary(const_cast<char *>(s.data()), l);
}

#ifndef BOOST_NO_CWCHAR
template<class Archive, class Elem, class Tr>
BOOST_ARCHIVE_OR_WARCHIVE_DECL(void)
basic_binary_iprimitive<Archive, Elem, Tr>::load(char * s)
{
    std::size_t l;
    this->This()->load(l);
    load_binary(s, l);
    s[l] = '\0';
}
#endif

#ifndef BOOST_NO_STD_WSTRING
template<class Archive, class Elem, class Tr>
BOOST_ARCHIVE_OR_WARCHIVE_DECL(void)
basic_binary_iprimitive<Archive, Elem, Tr>::load(std::wstring & ws)
{
    std::size_t l;
    this->This()->load(l);
    // borland de-allocator fixup
    #if BOOST_WORKAROUND(_RWSTD_VER, BOOST_TESTED_AT(20101))
    if(NULL != ws.data())
    #endif
        ws.resize(l);
    // note breaking a rule here - is could be a problem on some platform
    load_binary(const_cast<wchar_t *>(ws.data()), l * sizeof(wchar_t) / sizeof(char));
}
#endif

template<class Archive, class Elem, class Tr>
BOOST_ARCHIVE_OR_WARCHIVE_DECL(BOOST_PP_EMPTY())
basic_binary_iprimitive<Archive, Elem, Tr>::basic_binary_iprimitive(
    std::basic_streambuf<Elem, Tr> & sb, 
    bool no_codecvt
) :
    m_sb(sb),
    archive_locale(NULL),
    locale_saver(m_sb)
{
    if(! no_codecvt){
        archive_locale.reset(
            boost::archive::add_facet(
                std::locale::classic(),
                new codecvt_null<Elem>
            )
        );
        m_sb.pubimbue(* archive_locale);
    }
}

// some libraries including stl and libcomo fail if the
// buffer isn't flushed before the code_cvt facet is changed.
// I think this is a bug.  We explicity invoke sync to when
// we're done with the streambuf to work around this problem.
// Note that sync is a protected member of stream buff so we
// have to invoke it through a contrived derived class.
namespace detail {
// note: use "using" to get past msvc bug
using namespace std;
template<class Elem, class Tr>
class input_streambuf_access : public std::basic_streambuf<Elem, Tr> {
    public:
        virtual int sync(){
#if BOOST_WORKAROUND(__MWERKS__, BOOST_TESTED_AT(0x3206))
            return this->basic_streambuf::sync();
#else
            return this->basic_streambuf<Elem, Tr>::sync();
#endif
        }
};
} // detail

// scoped_ptr requires that archive_locale be a complete type at time of
// destruction so define destructor here rather than in the header
template<class Archive, class Elem, class Tr>
BOOST_ARCHIVE_OR_WARCHIVE_DECL(BOOST_PP_EMPTY())
basic_binary_iprimitive<Archive, Elem, Tr>::~basic_binary_iprimitive(){
    // push back unread characters
    int result = static_cast<detail::input_streambuf_access<Elem, Tr> &>(
        m_sb
    ).sync();
    if(0 != result){ 
        boost::throw_exception(
            archive_exception(archive_exception::stream_error)
        );
    }
}

} // namespace archive
} // namespace boost
