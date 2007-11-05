/////////1/////////2/////////3/////////4/////////5/////////6/////////7/////////8
// basic_binary_oprimitive.ipp:

// (C) Copyright 2002 Robert Ramey - http://www.rrsd.com . 
// Use, modification and distribution is subject to the Boost Software
// License, Version 1.0. (See accompanying file LICENSE_1_0.txt or copy at
// http://www.boost.org/LICENSE_1_0.txt)

//  See http://www.boost.org for updates, documentation, and revision history.

#include <ostream>
#include <cstring>

#include <boost/config.hpp>

#if defined(BOOST_NO_STDC_NAMESPACE) && ! defined(__LIBCOMO__)
namespace std{ 
    using ::strlen; 
} // namespace std
#endif


#ifndef BOOST_NO_CWCHAR
#include <cwchar>
#ifdef BOOST_NO_STDC_NAMESPACE
namespace std{ using ::wcslen; }
#endif
#endif

#include <boost/detail/workaround.hpp>

#include <boost/throw_exception.hpp>
#include <boost/scoped_ptr.hpp>
#include <boost/archive/archive_exception.hpp>
#include <boost/archive/add_facet.hpp>
#include <boost/archive/codecvt_null.hpp>

namespace boost {
namespace archive {

//////////////////////////////////////////////////////////////////////
// implementation of basic_binary_oprimitive

template<class Archive, class Elem, class Tr>
BOOST_ARCHIVE_OR_WARCHIVE_DECL(void)
basic_binary_oprimitive<Archive, Elem, Tr>::init()
{
    // record native sizes of fundamental types
    // this is to permit detection of attempts to pass
    // native binary archives accross incompatible machines.
    // This is not foolproof but its better than nothing.
    this->This()->save(static_cast<unsigned char>(sizeof(int)));
    this->This()->save(static_cast<unsigned char>(sizeof(long)));
    this->This()->save(static_cast<unsigned char>(sizeof(float)));
    this->This()->save(static_cast<unsigned char>(sizeof(double)));
    // for checking endianness
    this->This()->save(int(1));
}

template<class Archive, class Elem, class Tr>
BOOST_ARCHIVE_OR_WARCHIVE_DECL(void)
basic_binary_oprimitive<Archive, Elem, Tr>::save(const char * s)
{
    std::size_t l = std::strlen(s);
    this->This()->save(l);
    save_binary(s, l);
}

template<class Archive, class Elem, class Tr>
BOOST_ARCHIVE_OR_WARCHIVE_DECL(void)
basic_binary_oprimitive<Archive, Elem, Tr>::save(const std::string &s)
{
    std::size_t l = static_cast<unsigned int>(s.size());
    this->This()->save(l);
    save_binary(s.data(), l);
}

#ifndef BOOST_NO_CWCHAR
template<class Archive, class Elem, class Tr>
BOOST_ARCHIVE_OR_WARCHIVE_DECL(void)
basic_binary_oprimitive<Archive, Elem, Tr>::save(const wchar_t * ws)
{
    std::size_t l = std::wcslen(ws);
    this->This()->save(l);
    save_binary(ws, l * sizeof(wchar_t) / sizeof(char));
}

#ifndef BOOST_NO_STD_WSTRING
template<class Archive, class Elem, class Tr>
BOOST_ARCHIVE_OR_WARCHIVE_DECL(void)
basic_binary_oprimitive<Archive, Elem, Tr>::save(const std::wstring &ws)
{
    std::size_t l = ws.size();
    this->This()->save(l);
    save_binary(ws.data(), l * sizeof(wchar_t) / sizeof(char));
}
#endif
#endif

template<class Archive, class Elem, class Tr>
BOOST_ARCHIVE_OR_WARCHIVE_DECL(BOOST_PP_EMPTY())
basic_binary_oprimitive<Archive, Elem, Tr>::basic_binary_oprimitive(
    std::basic_streambuf<Elem, Tr> & sb, 
    bool no_codecvt
) : 
    m_sb(sb),
    archive_locale(NULL),
    locale_saver(m_sb)
{
    if(! no_codecvt){
        archive_locale.reset(
            add_facet(
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
class output_streambuf_access : public std::basic_streambuf<Elem, Tr> {
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

// scoped_ptr requires that g be a complete type at time of
// destruction so define destructor here rather than in the header
template<class Archive, class Elem, class Tr>
BOOST_ARCHIVE_OR_WARCHIVE_DECL(BOOST_PP_EMPTY())
basic_binary_oprimitive<Archive, Elem, Tr>::~basic_binary_oprimitive(){
    // flush buffer
    int result = static_cast<detail::output_streambuf_access<Elem, Tr> &>(
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
