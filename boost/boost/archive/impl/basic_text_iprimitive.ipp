/////////1/////////2/////////3/////////4/////////5/////////6/////////7/////////8
// basic_text_iprimitive.ipp:

// (C) Copyright 2002 Robert Ramey - http://www.rrsd.com .
// Distributed under the Boost Software License, Version 1.0. (See
// accompanying file LICENSE_1_0.txt or copy at
// http://www.boost.org/LICENSE_1_0.txt)

//  See http://www.boost.org for updates, documentation, and revision history.

#include <cstddef> // size_t

#include <boost/config.hpp>
#if defined(BOOST_NO_STDC_NAMESPACE)
namespace std{ 
    using ::size_t; 
} // namespace std
#endif

#include <boost/throw_exception.hpp>
#include <boost/pfto.hpp>

#include <boost/archive/basic_text_iprimitive.hpp>
#include <boost/archive/codecvt_null.hpp>
#include <boost/archive/add_facet.hpp>

#include <boost/archive/iterators/remove_whitespace.hpp>
#include <boost/archive/iterators/istream_iterator.hpp>
#include <boost/archive/iterators/binary_from_base64.hpp>
#include <boost/archive/iterators/transform_width.hpp>

namespace boost { 
namespace archive {

// translate base64 text into binary and copy into buffer
// until buffer is full.
template<class IStream>
BOOST_ARCHIVE_OR_WARCHIVE_DECL(void)
basic_text_iprimitive<IStream>::load_binary(
    void *address, 
    std::size_t count
){
    typedef BOOST_DEDUCED_TYPENAME IStream::char_type CharType;
    
    if(0 == count)
        return;
        
    assert(
        static_cast<std::size_t>((std::numeric_limits<std::streamsize>::max)())
        > (count + sizeof(CharType) - 1)/sizeof(CharType)
    );
        
    if(is.fail())
        boost::throw_exception(archive_exception(archive_exception::stream_error));
    // convert from base64 to binary
    typedef BOOST_DEDUCED_TYPENAME
        iterators::transform_width<
            iterators::binary_from_base64<
                iterators::remove_whitespace<
                    iterators::istream_iterator<CharType>
                >
                ,CharType
            >
            ,8
            ,6
            ,CharType
        > 
        binary;

    binary ti_begin = binary(
        BOOST_MAKE_PFTO_WRAPPER(
            iterators::istream_iterator<CharType>(is)
        )
    );
                
    char * caddr = static_cast<char *>(address);
    std::size_t padding = 2 - count % 3;
    
    // take care that we don't increment anymore than necessary
    while(--count > 0){
        *caddr++ = static_cast<char>(*ti_begin);
        ++ti_begin;
    }
    *caddr++ = static_cast<char>(*ti_begin);
    
    if(padding > 1)
        ++ti_begin;
        if(padding > 2)
            ++ti_begin;
}

template<class IStream>
BOOST_ARCHIVE_OR_WARCHIVE_DECL(BOOST_PP_EMPTY())
basic_text_iprimitive<IStream>::basic_text_iprimitive(
    IStream  &is_,
    bool no_codecvt
) : 
    is(is_),
    flags_saver(is_),
    precision_saver(is_),
    archive_locale(NULL),
    locale_saver(is_)
{
    if(! no_codecvt){
        archive_locale.reset(
            add_facet(
                std::locale::classic(), 
                new codecvt_null<BOOST_DEDUCED_TYPENAME IStream::char_type>
            )
        );
        is.imbue(* archive_locale);
    }
    is >> std::noboolalpha;
}

template<class IStream>
BOOST_ARCHIVE_OR_WARCHIVE_DECL(BOOST_PP_EMPTY())
basic_text_iprimitive<IStream>::~basic_text_iprimitive(){
    is.sync();
}

} // namespace archive
} // namespace boost
