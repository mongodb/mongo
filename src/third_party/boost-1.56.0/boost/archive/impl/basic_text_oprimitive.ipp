/////////1/////////2/////////3/////////4/////////5/////////6/////////7/////////8
// basic_text_oprimitive.ipp:

// (C) Copyright 2002 Robert Ramey - http://www.rrsd.com . 
// Use, modification and distribution is subject to the Boost Software
// License, Version 1.0. (See accompanying file LICENSE_1_0.txt or copy at
// http://www.boost.org/LICENSE_1_0.txt)

//  See http://www.boost.org for updates, documentation, and revision history.

#include <cstddef> // NULL
#include <algorithm> // std::copy
#include <boost/serialization/pfto.hpp>

#include <boost/archive/basic_text_oprimitive.hpp>
#include <boost/archive/codecvt_null.hpp>
#include <boost/archive/add_facet.hpp>

#include <boost/archive/iterators/base64_from_binary.hpp>
#include <boost/archive/iterators/insert_linebreaks.hpp>
#include <boost/archive/iterators/transform_width.hpp>
#include <boost/archive/iterators/ostream_iterator.hpp>

namespace boost {
namespace archive {

// translate to base64 and copy in to buffer.
template<class OStream>
BOOST_ARCHIVE_OR_WARCHIVE_DECL(void)
basic_text_oprimitive<OStream>::save_binary(
    const void *address, 
    std::size_t count
){
    typedef typename OStream::char_type CharType;
    
    if(0 == count)
        return;
    
    if(os.fail())
        boost::serialization::throw_exception(
            archive_exception(archive_exception::output_stream_error)
        );
        
    os.put('\n');
    
    typedef 
        boost::archive::iterators::insert_linebreaks<
            boost::archive::iterators::base64_from_binary<
                boost::archive::iterators::transform_width<
                    const char *,
                    6,
                    8
                >
            > 
            ,76
            ,const char // cwpro8 needs this
        > 
        base64_text;

    boost::archive::iterators::ostream_iterator<CharType> oi(os);
    std::copy(
        base64_text(BOOST_MAKE_PFTO_WRAPPER(static_cast<const char *>(address))),
        base64_text(
            BOOST_MAKE_PFTO_WRAPPER(static_cast<const char *>(address) + count)
        ),
        oi
    );
    
    std::size_t tail = count % 3;
    if(tail > 0){
        *oi++ = '=';
        if(tail < 2)
            *oi = '=';
    }
}

template<class OStream>
BOOST_ARCHIVE_OR_WARCHIVE_DECL(BOOST_PP_EMPTY())
basic_text_oprimitive<OStream>::basic_text_oprimitive(
    OStream & os_,
    bool no_codecvt
) : 
#ifndef BOOST_NO_STD_LOCALE
    os(os_),
    flags_saver(os_),
    precision_saver(os_),
    archive_locale(NULL),
    locale_saver(* os_.rdbuf())
{
    if(! no_codecvt){
        archive_locale.reset(
            add_facet(
                std::locale::classic(), 
                new codecvt_null<typename OStream::char_type>
            )
        );
        os.imbue(* archive_locale);
    }
    os << std::noboolalpha;
}
#else
    os(os_),
    flags_saver(os_),
    precision_saver(os_)
{}
#endif

template<class OStream>
BOOST_ARCHIVE_OR_WARCHIVE_DECL(BOOST_PP_EMPTY())
basic_text_oprimitive<OStream>::~basic_text_oprimitive(){
    os << std::endl;
}

} //namespace boost 
} //namespace archive 
