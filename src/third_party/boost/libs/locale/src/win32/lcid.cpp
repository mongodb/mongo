//
// Copyright (c) 2009-2011 Artyom Beilis (Tonkikh)
//
// Distributed under the Boost Software License, Version 1.0.
// https://www.boost.org/LICENSE_1_0.txt

#ifndef NOMINMAX
#    define NOMINMAX
#endif

#include "lcid.hpp"
#include <boost/locale/util/locale_data.hpp>
#include <boost/thread/locks.hpp>
#include <boost/thread/mutex.hpp>
#include <map>
#include <sstream>
#include <string.h>
#include <string>
#include <windows.h>

namespace boost { namespace locale { namespace impl_win {

    typedef std::map<std::string, unsigned> table_type;

    static table_type* volatile table = 0;

    boost::mutex& lcid_table_mutex()
    {
        static boost::mutex m;
        return m;
    }

    table_type& real_lcid_table()
    {
        static table_type real_table;
        return real_table;
    }

    BOOL CALLBACK proc(char* s)
    {
        table_type& tbl = real_lcid_table();
        try {
            std::istringstream ss;
            ss.str(s);
            ss >> std::hex;

            unsigned lcid;
            ss >> lcid;
            if(ss.fail() || !ss.eof())
                return FALSE;

            char iso_639_lang[16];
            char iso_3166_country[16];
            if(GetLocaleInfoA(lcid, LOCALE_SISO639LANGNAME, iso_639_lang, sizeof(iso_639_lang)) == 0)
                return FALSE;
            std::string lc_name = iso_639_lang;
            if(GetLocaleInfoA(lcid, LOCALE_SISO3166CTRYNAME, iso_3166_country, sizeof(iso_3166_country)) != 0) {
                lc_name += "_";
                lc_name += iso_3166_country;
            }
            const auto p = tbl.find(lc_name);
            if(p != tbl.end()) {
                if(p->second > lcid)
                    p->second = lcid;
            } else
                tbl[lc_name] = lcid;
        } catch(...) {
            tbl.clear();
            return FALSE;
        }
        return TRUE;
    }

    const table_type& get_ready_lcid_table()
    {
        if(table)
            return *table;
        else {
            boost::unique_lock<boost::mutex> lock(lcid_table_mutex());
            if(table)
                return *table;
            EnumSystemLocalesA(proc, LCID_INSTALLED);
            table = &real_lcid_table();
            return *table;
        }
    }

    unsigned locale_to_lcid(const std::string& locale_name)
    {
        if(locale_name.empty())
            return LOCALE_USER_DEFAULT;
        boost::locale::util::locale_data d;
        d.parse(locale_name);
        std::string id = d.language();

        if(!d.country().empty())
            id += "_" + d.country();
        if(!d.variant().empty())
            id += "@" + d.variant();

        const table_type& tbl = get_ready_lcid_table();
        const auto p = tbl.find(id);
        return (p != tbl.end()) ? p->second : 0;
    }

}}} // namespace boost::locale::impl_win
