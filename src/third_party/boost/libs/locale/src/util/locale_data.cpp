//
//  Copyright (c) 2009-2011 Artyom Beilis (Tonkikh)
//
//  Distributed under the Boost Software License, Version 1.0. (See
//  accompanying file LICENSE_1_0.txt or copy at
//  http://www.boost.org/LICENSE_1_0.txt)
//
#define BOOST_LOCALE_SOURCE
#include "locale_data.hpp"
#include "../encoding/conv.hpp"
#include <string>

namespace boost {
namespace locale {
namespace util {
    void locale_data::parse(std::string const &locale_name)
    {
        language = "C";
        country.clear();
        variant.clear();
        encoding = "us-ascii";
        utf8=false;
        parse_from_lang(locale_name);
    }

    void locale_data::parse_from_lang(std::string const &locale_name) 
    {
        size_t end = locale_name.find_first_of("-_@.");
        std::string tmp = locale_name.substr(0,end);
        if(tmp.empty())
            return;
        for(unsigned i=0;i<tmp.size();i++) {
            if('A' <= tmp[i] && tmp[i]<='Z')
                tmp[i]=tmp[i]-'A'+'a';
            else if(tmp[i] < 'a' || 'z' < tmp[i])
                return;
        }
        language = tmp;
        if(end >= locale_name.size())
            return;

        if(locale_name[end] == '-' || locale_name[end]=='_') {
           parse_from_country(locale_name.substr(end+1));
        }
        else if(locale_name[end] == '.') {
           parse_from_encoding(locale_name.substr(end+1));
        }
        else if(locale_name[end] == '@') {
           parse_from_variant(locale_name.substr(end+1));
        }
    }

    void locale_data::parse_from_country(std::string const &locale_name) 
    {
        size_t end = locale_name.find_first_of("@.");
        std::string tmp = locale_name.substr(0,end);
        if(tmp.empty())
            return;
        for(unsigned i=0;i<tmp.size();i++) {
            if('a' <= tmp[i] && tmp[i]<='z')
                tmp[i]=tmp[i]-'a'+'A';
            else if(tmp[i] < 'A' || 'Z' < tmp[i])
                return;
        }

        country = tmp;

        if(end >= locale_name.size())
            return;
        else if(locale_name[end] == '.') {
           parse_from_encoding(locale_name.substr(end+1));
        }
        else if(locale_name[end] == '@') {
           parse_from_variant(locale_name.substr(end+1));
        }
    }
    
    void locale_data::parse_from_encoding(std::string const &locale_name) 
    {
        size_t end = locale_name.find_first_of("@");
        std::string tmp = locale_name.substr(0,end);
        if(tmp.empty())
            return;
        for(unsigned i=0;i<tmp.size();i++) {
            if('A' <= tmp[i] && tmp[i]<='Z')
                tmp[i]=tmp[i]-'A'+'a';
        }
        encoding = tmp;
        
        utf8 = conv::impl::normalize_encoding(encoding.c_str()) == "utf8";

        if(end >= locale_name.size())
            return;

        if(locale_name[end] == '@') {
           parse_from_variant(locale_name.substr(end+1));
        }
    }

    void locale_data::parse_from_variant(std::string const &locale_name)
    {
        variant = locale_name;
        for(unsigned i=0;i<variant.size();i++) {
            if('A' <= variant[i] && variant[i]<='Z')
                variant[i]=variant[i]-'A'+'a';
        }
    }

} // util
} // locale 
} // boost

// vim: tabstop=4 expandtab shiftwidth=4 softtabstop=4
