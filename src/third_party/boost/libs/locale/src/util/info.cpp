//
// Copyright (c) 2009-2011 Artyom Beilis (Tonkikh)
// Copyright (c) 2022-2023 Alexander Grund
//
// Distributed under the Boost Software License, Version 1.0.
// https://www.boost.org/LICENSE_1_0.txt

#include <boost/locale/generator.hpp>
#include <boost/locale/info.hpp>
#include <boost/locale/util.hpp>
#include <boost/locale/util/locale_data.hpp>
#include <cstdlib>
#include <ios>
#include <locale>
#include <sstream>
#include <string>

namespace boost { namespace locale { namespace util {

    class simple_info : public info {
    public:
        simple_info(const std::string& name, size_t refs = 0) : info(refs), name_(name) { d.parse(name); }
        std::string get_string_property(string_property v) const override
        {
            switch(v) {
                case language_property: return d.language();
                case country_property: return d.country();
                case variant_property: return d.variant();
                case encoding_property: return d.encoding();
                case name_property: return name_;
            }
            return ""; // LCOV_EXCL_LINE
        }

        int get_integer_property(integer_property v) const override
        {
            switch(v) {
                case utf8_property: return d.is_utf8();
            }
            return 0; // LCOV_EXCL_LINE
        }

    private:
        locale_data d;
        std::string name_;
    };

    std::locale create_info(const std::locale& in, const std::string& name)
    {
        return std::locale(in, new simple_info(name));
    }

}}} // namespace boost::locale::util
