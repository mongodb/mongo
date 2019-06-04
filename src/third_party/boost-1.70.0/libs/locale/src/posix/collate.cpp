//
//  Copyright (c) 2009-2011 Artyom Beilis (Tonkikh)
//
//  Distributed under the Boost Software License, Version 1.0. (See
//  accompanying file LICENSE_1_0.txt or copy at
//  http://www.boost.org/LICENSE_1_0.txt)
//
#define BOOST_LOCALE_SOURCE
#if defined(__FreeBSD__)
#include <xlocale.h>
#endif
#include <locale>
#include <locale.h>
#include <string.h>
#include <wchar.h>
#include <string>
#include <stdexcept>
#include <ios>
#include <vector>
#include <boost/locale/generator.hpp>
#include "../shared/mo_hash.hpp"

#include "all_generator.hpp"

namespace boost {
namespace locale {
namespace impl_posix {

template<typename CharType>
struct coll_traits;

template<>
struct coll_traits<char> {
    static size_t xfrm(char *out,char const *in,size_t n,locale_t l)
    {
        return strxfrm_l(out,in,n,l);
    }
    static size_t coll(char const *left,char const *right,locale_t l)
    {
        return strcoll_l(left,right,l);
    }
};

template<>
struct coll_traits<wchar_t> {
    static size_t xfrm(wchar_t *out,wchar_t const *in,size_t n,locale_t l)
    {
        return wcsxfrm_l(out,in,n,l);
    }
    static size_t coll(wchar_t const *left,wchar_t const *right,locale_t l)
    {
        return wcscoll_l(left,right,l);
    }
};

template<typename CharType>
class collator : public std::collate<CharType> {
public:
    typedef CharType char_type;
    typedef std::basic_string<char_type> string_type;
    collator(boost::shared_ptr<locale_t> l,size_t refs = 0) : 
        std::collate<CharType>(refs),
        lc_(l)
    {
    }
    virtual ~collator()
    {
    }
    virtual int do_compare(char_type const *lb,char_type const *le,char_type const *rb,char_type const *re) const
    {
        string_type left(lb,le-lb);
        string_type right(rb,re-rb);
        int res = coll_traits<char_type>::coll(left.c_str(),right.c_str(),*lc_);
        if(res < 0)
            return -1;
        if(res > 0)
            return 1;
        return 0;
    }
    virtual long do_hash(char_type const *b,char_type const *e) const
    {
        string_type s(do_transform(b,e));
        char const *begin = reinterpret_cast<char const *>(s.c_str());
        char const *end = begin + s.size() * sizeof(char_type);
        return gnu_gettext::pj_winberger_hash_function(begin,end);
    }
    virtual string_type do_transform(char_type const *b,char_type const *e) const
    {
        string_type s(b,e-b);
        std::vector<char_type> buf((e-b)*2+1);
        size_t n = coll_traits<char_type>::xfrm(&buf.front(),s.c_str(),buf.size(),*lc_);
        if(n>buf.size()) {
            buf.resize(n);
            coll_traits<char_type>::xfrm(&buf.front(),s.c_str(),n,*lc_);
        }
        return string_type(&buf.front(),n);
    }
private:
    boost::shared_ptr<locale_t> lc_;
};


std::locale create_collate( std::locale const &in,
                            boost::shared_ptr<locale_t> lc,
                            character_facet_type type)
{
    switch(type) {
    case char_facet:
        return std::locale(in,new collator<char>(lc));
    case wchar_t_facet:
        return std::locale(in,new collator<wchar_t>(lc));
    default:
        return in;
    }
}


} // impl_std
} // locale 
} //boost



// vim: tabstop=4 expandtab shiftwidth=4 softtabstop=4
