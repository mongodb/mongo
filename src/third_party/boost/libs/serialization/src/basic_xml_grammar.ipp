/////////1/////////2/////////3/////////4/////////5/////////6/////////7/////////8
// basic_xml_grammar.ipp:

// (C) Copyright 2002 Robert Ramey - http://www.rrsd.com . 
// Use, modification and distribution is subject to the Boost Software
// License, Version 1.0. (See accompanying file LICENSE_1_0.txt or copy at
// http://www.boost.org/LICENSE_1_0.txt)

//  See http://www.boost.org for updates, documentation, and revision history.

#if (defined _MSC_VER) && (_MSC_VER == 1200)
#  pragma warning (disable : 4786) // too long name, harmless warning
#endif

#include <istream>
#include <algorithm>
#include <boost/config.hpp> // typename

#ifdef BOOST_MSVC
#  pragma warning(push)
#  pragma warning(disable : 4244 4511 4512)
#endif

#include <cerrno>   // errno
#include <cstring>  // strerror(errno)

// spirit stuff
#include <boost/spirit/include/classic_operators.hpp>
#include <boost/spirit/include/classic_actions.hpp>
#include <boost/spirit/include/classic_numerics.hpp>

#ifdef BOOST_MSVC
#pragma warning(pop)
#endif

// for head_iterator test
#include <boost/function.hpp>

#include <boost/io/ios_state.hpp>
#include <boost/serialization/throw_exception.hpp>
#include <boost/archive/impl/basic_xml_grammar.hpp>
#include <boost/archive/xml_archive_exception.hpp>
#include <boost/archive/basic_xml_archive.hpp>
#include <boost/archive/iterators/xml_unescape.hpp>

using namespace boost::spirit::classic;

namespace boost {
namespace archive {

/////////1/////////2/////////3/////////4/////////5/////////6/////////7/////////8
// template code for basic_xml_grammar of both wchar_t and char types

namespace xml { // anonymous

#ifdef BOOST_MSVC
#  pragma warning(push)
#  pragma warning(disable : 4511 4512)
#endif

template<class T>
struct assign_impl {
    T & t;
    void operator()(const T t_) const {
        t = t_;
    }
    assign_impl(T &  t_)
        : t(t_)
    {}
};

template<>
struct assign_impl<std::string> {
    std::string & t;
    void operator()(
        std::string::const_iterator b, 
        std::string::const_iterator e
    ) const {
        t.resize(0);
        while(b != e){
            t += * b;
            ++b;
        }
    }
    assign_impl<std::string> & operator=(
        assign_impl<std::string> & rhs
    );
    assign_impl(std::string & t_)
        : t(t_)
    {}
};

#ifndef BOOST_NO_STD_WSTRING
template<>
struct assign_impl<std::wstring> {
    std::wstring & t;
    void operator()(
        std::wstring::const_iterator b, 
        std::wstring::const_iterator e
    ) const {
        t.resize(0);
        while(b != e){
            t += * b;
            ++b;
        }
    }
    assign_impl(std::wstring & t_)
        : t(t_)
    {}
};
#endif

template<class T>
assign_impl<T> assign_object(T &t){
    return assign_impl<T>(t);
} 

struct assign_level {
    tracking_type & tracking_level;
    void operator()(const unsigned int tracking_level_) const {
        tracking_level = (0 == tracking_level_) ? false : true;
    }
    assign_level(tracking_type &  tracking_level_)
        : tracking_level(tracking_level_)
    {}
};

template<class String, class Iterator>
struct append_string {
    String & contents;
    void operator()(Iterator start, Iterator end) const {
    #if 0
        typedef boost::archive::iterators::xml_unescape<Iterator> translator;
        contents.append(
            translator(BOOST_MAKE_PFTO_WRAPPER(start)), 
            translator(BOOST_MAKE_PFTO_WRAPPER(end))
        );
    #endif
        contents.append(start, end);
    }
    append_string(String & contents_)
        : contents(contents_)
    {}
};

template<class String>
struct append_char {
    String & contents;
    void operator()(const unsigned int char_value) const {
        contents += static_cast<typename String::value_type>(char_value);
    }
    append_char(String & contents_)
        : contents(contents_)
    {}
};

template<class String, unsigned int c>
struct append_lit {
    String & contents;
    template<class X, class Y>
    void operator()(const X & /*x*/, const Y & /*y*/) const {
        const typename String::value_type z = c;
        contents += z;
    }
    append_lit(String & contents_)
        : contents(contents_)
    {}
};

#ifdef BOOST_MSVC
#pragma warning(pop)
#endif

} // namespace anonymous

template<class CharType>
bool basic_xml_grammar<CharType>::my_parse(
    typename basic_xml_grammar<CharType>::IStream & is,
    const rule_t & rule_,
    CharType delimiter
) const {
    if(is.fail()){
        return false;
    }
    
    is >> std::noskipws;

    std::basic_string<CharType> arg;
    
    for(;;){
        CharType result;
        is.get(result);
        if(is.fail()){
            boost::serialization::throw_exception(
                boost::archive::archive_exception(
                    archive_exception::input_stream_error,
                    std::strerror(errno)
                )
            );
        }
        if(is.eof())
            return false;
        arg += result;
        if(result == delimiter)
            break;
    }

    // read just one more character.  This will be the newline after the tag
    // this is so that the next operation will return fail if the archive
    // is terminated.  This will permit the archive to be used for debug
    // and transaction data logging in the standard way.
    
    parse_info<typename std::basic_string<CharType>::iterator> 
        result = boost::spirit::classic::parse(arg.begin(), arg.end(), rule_);
    return result.hit;
}

template<class CharType>
bool basic_xml_grammar<CharType>::parse_start_tag(
    typename basic_xml_grammar<CharType>::IStream & is
){
    rv.class_name.resize(0);
    return my_parse(is, STag);
}

template<class CharType>
bool basic_xml_grammar<CharType>::parse_end_tag(IStream & is) const {
    return my_parse(is, ETag);
}

template<class CharType>
bool basic_xml_grammar<CharType>::parse_string(IStream & is, StringType & s){
    rv.contents.resize(0);
    bool result = my_parse(is, content, '<');
    // note: unget caused a problem with dinkumware.  replace with
 // is.unget();
    // putback another delimiter instead
    is.putback('<');
    if(result)
        s = rv.contents;
    return result;
}

template<class CharType>
basic_xml_grammar<CharType>::basic_xml_grammar(){
    init_chset();

    S =
        +(Sch)
    ;

    // refactoring to workaround template depth on darwin
    NameHead = (Letter | '_' | ':');
    NameTail = *NameChar ;
    Name =
      NameHead >> NameTail
    ;

    Eq =
        !S >> '=' >> !S
    ;

    AttributeList = 
        *(S >> Attribute)
    ;
    
    STag =
        !S
        >> '<'
        >> Name  [xml::assign_object(rv.object_name)]
        >> AttributeList
        >> !S
        >> '>'
    ;

    ETag =
        !S
        >> "</"
        >> Name [xml::assign_object(rv.object_name)]
        >> !S 
        >> '>'
    ;

    // refactoring to workaround template depth on darwin
    CharDataChars = +(anychar_p - chset_p(L"&<"));
    CharData =  
        CharDataChars [
            xml::append_string<
                StringType, 
                typename std::basic_string<CharType>::const_iterator
            >(rv.contents)
        ]
    ;

    // slight factoring works around ICE in msvc 6.0
    CharRef1 = 
        str_p(L"&#") >> uint_p [xml::append_char<StringType>(rv.contents)] >> L';'
    ;
    CharRef2 =
        str_p(L"&#x") >> hex_p [xml::append_char<StringType>(rv.contents)] >> L';'
    ;
    CharRef = CharRef1 | CharRef2 ;

    AmpRef = str_p(L"&amp;")[xml::append_lit<StringType, L'&'>(rv.contents)];
    LTRef = str_p(L"&lt;")[xml::append_lit<StringType, L'<'>(rv.contents)];
    GTRef = str_p(L"&gt;")[xml::append_lit<StringType, L'>'>(rv.contents)];
    AposRef = str_p(L"&apos;")[xml::append_lit<StringType, L'\''>(rv.contents)];
    QuoteRef = str_p(L"&quot;")[xml::append_lit<StringType, L'"'>(rv.contents)];

    Reference =
        AmpRef
        | LTRef
        | GTRef
        | AposRef
        | QuoteRef
        | CharRef
    ;

    content = 
        L"<" // should be end_p
        | +(Reference | CharData) >> L"<"
    ;

    ClassIDAttribute = 
        str_p(BOOST_ARCHIVE_XML_CLASS_ID()) >> NameTail
        >> Eq 
        >> L'"'
        >> int_p [xml::assign_object(rv.class_id)]
        >> L'"'
      ;

    ObjectIDAttribute = (
        str_p(BOOST_ARCHIVE_XML_OBJECT_ID()) 
        | 
        str_p(BOOST_ARCHIVE_XML_OBJECT_REFERENCE()) 
        )
        >> NameTail
        >> Eq 
        >> L'"'
        >> L'_'
        >> uint_p [xml::assign_object(rv.object_id)]
        >> L'"'
    ;
        
    AmpName = str_p(L"&amp;")[xml::append_lit<StringType, L'&'>(rv.class_name)];
    LTName = str_p(L"&lt;")[xml::append_lit<StringType, L'<'>(rv.class_name)];
    GTName = str_p(L"&gt;")[xml::append_lit<StringType, L'>'>(rv.class_name)];
    ClassNameChar = 
        AmpName
        | LTName
        | GTName
        | (anychar_p - chset_p(L"\"")) [xml::append_char<StringType>(rv.class_name)]
    ;
    
    ClassName =
        * ClassNameChar
    ;
    
    ClassNameAttribute = 
        str_p(BOOST_ARCHIVE_XML_CLASS_NAME()) 
        >> Eq 
        >> L'"'
        >> ClassName
        >> L'"'
    ;

    TrackingAttribute = 
        str_p(BOOST_ARCHIVE_XML_TRACKING())
        >> Eq
        >> L'"'
        >> uint_p [xml::assign_level(rv.tracking_level)]
        >> L'"'
    ;

    VersionAttribute = 
        str_p(BOOST_ARCHIVE_XML_VERSION())
        >> Eq
        >> L'"'
        >> uint_p [xml::assign_object(rv.version)]
        >> L'"'
    ;

    UnusedAttribute = 
        Name
        >> Eq
        >> L'"'
        >> !CharData
        >> L'"'
    ;

    Attribute =
        ClassIDAttribute
        | ObjectIDAttribute
        | ClassNameAttribute
        | TrackingAttribute
        | VersionAttribute
        | UnusedAttribute
    ;

    XMLDeclChars = *(anychar_p - chset_p(L"?>"));
    XMLDecl =
        !S
        >> str_p(L"<?xml")
        >> S
        >> str_p(L"version")
        >> Eq
        >> str_p(L"\"1.0\"")
        >> XMLDeclChars
        >> !S
        >> str_p(L"?>")
    ;

    DocTypeDeclChars = *(anychar_p - chset_p(L">"));
    DocTypeDecl =
        !S
        >> str_p(L"<!DOCTYPE")
        >> DocTypeDeclChars
        >> L'>'
    ;

    SignatureAttribute = 
        str_p(L"signature") 
        >> Eq 
        >> L'"'
        >> Name [xml::assign_object(rv.class_name)]
        >> L'"'
    ;
    
    SerializationWrapper =
        !S
        >> str_p(L"<boost_serialization")
        >> S
        >> ( (SignatureAttribute >> S >> VersionAttribute)
           | (VersionAttribute >> S >> SignatureAttribute)
           )
        >> !S
        >> L'>'
    ;
}

template<class CharType>
void basic_xml_grammar<CharType>::init(IStream & is){
    init_chset();
    if(! my_parse(is, XMLDecl))
        boost::serialization::throw_exception(
            xml_archive_exception(xml_archive_exception::xml_archive_parsing_error)
        );
    if(! my_parse(is, DocTypeDecl))
        boost::serialization::throw_exception(
            xml_archive_exception(xml_archive_exception::xml_archive_parsing_error)
        );
    if(! my_parse(is, SerializationWrapper))
        boost::serialization::throw_exception(
            xml_archive_exception(xml_archive_exception::xml_archive_parsing_error)
        );
    if(! std::equal(rv.class_name.begin(), rv.class_name.end(), BOOST_ARCHIVE_SIGNATURE()))
        boost::serialization::throw_exception(
            archive_exception(archive_exception::invalid_signature)
        );
}

template<class CharType>
bool basic_xml_grammar<CharType>::windup(IStream & is) {
    return my_parse(is, ETag);
}

} // namespace archive
} // namespace boost
