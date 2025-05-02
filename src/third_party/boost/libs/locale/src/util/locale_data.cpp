//
// Copyright (c) 2009-2011 Artyom Beilis (Tonkikh)
// Copyright (c) 2022-2024 Alexander Grund
//
// Distributed under the Boost Software License, Version 1.0.
// https://www.boost.org/LICENSE_1_0.txt

#include <boost/locale/util/locale_data.hpp>
#include <boost/locale/util/string.hpp>
#include "encoding.hpp"
#include <boost/assert.hpp>
#include <algorithm>
#include <stdexcept>
#include <string>

namespace boost { namespace locale { namespace util {
    /// Convert uppercase ASCII to lower case, return true if converted
    static bool make_lower(char& c)
    {
        if(is_upper_ascii(c)) {
            c += 'a' - 'A';
            return true;
        } else
            return false;
    }

    /// Convert lowercase ASCII to upper case, return true if converted
    static bool make_upper(char& c)
    {
        if(is_lower_ascii(c)) {
            c += 'A' - 'a';
            return true;
        } else
            return false;
    }

    locale_data::locale_data()
    {
        reset();
    }

    locale_data::locale_data(const std::string& locale_name)
    {
        if(!parse(locale_name))
            throw std::invalid_argument("Failed to parse locale name: " + locale_name);
    }

    void locale_data::reset()
    {
        language_ = "C";
        script_.clear();
        country_.clear();
        encoding_ = "US-ASCII";
        variant_.clear();
        utf8_ = false;
    }

    std::string locale_data::to_string() const
    {
        std::string result = language_;
        if(!script_.empty())
            (result += '_') += script_;
        if(!country_.empty())
            (result += '_') += country_;
        if(!encoding_.empty() && !util::are_encodings_equal(encoding_, "US-ASCII"))
            (result += '.') += encoding_;
        if(!variant_.empty())
            (result += '@') += variant_;
        return result;
    }

    bool locale_data::parse(const std::string& locale_name)
    {
        reset();
        return parse_from_lang(locale_name);
    }

    bool locale_data::parse_from_lang(const std::string& input)
    {
        const auto end = input.find_first_of("-_@.");
        std::string tmp = input.substr(0, end);
        if(tmp.empty())
            return false;
        // lowercase ASCII
        for(char& c : tmp) {
            if(!is_lower_ascii(c) && !make_lower(c))
                return false;
        }
        if(tmp != "c" && tmp != "posix") // Keep default
            language_ = tmp;

        if(end >= input.size())
            return true;
        else if(input[end] == '-' || input[end] == '_')
            return parse_from_script(input.substr(end + 1));
        else if(input[end] == '.')
            return parse_from_encoding(input.substr(end + 1));
        else {
            BOOST_ASSERT_MSG(input[end] == '@', "Unexpected delimiter");
            return parse_from_variant(input.substr(end + 1));
        }
    }

    bool locale_data::parse_from_script(const std::string& input)
    {
        const auto end = input.find_first_of("-_@.");
        std::string tmp = input.substr(0, end);
        // Script is exactly 4 ASCII characters, otherwise it is not present
        if(tmp.length() != 4)
            return parse_from_country(input);

        for(char& c : tmp) {
            if(!is_lower_ascii(c) && !make_lower(c))
                return parse_from_country(input);
        }
        make_upper(tmp[0]); // Capitalize first letter only
        script_ = tmp;

        if(end >= input.size())
            return true;
        else if(input[end] == '-' || input[end] == '_')
            return parse_from_country(input.substr(end + 1));
        else if(input[end] == '.')
            return parse_from_encoding(input.substr(end + 1));
        else {
            BOOST_ASSERT_MSG(input[end] == '@', "Unexpected delimiter");
            return parse_from_variant(input.substr(end + 1));
        }
    }

    bool locale_data::parse_from_country(const std::string& input)
    {
        if(language_ == "C")
            return false;

        const auto end = input.find_first_of("@.");
        std::string tmp = input.substr(0, end);
        if(tmp.empty())
            return false;

        // Make uppercase
        for(char& c : tmp)
            make_upper(c);

        // If it's ALL uppercase ASCII, assume ISO 3166 country id
        if(std::find_if_not(tmp.begin(), tmp.end(), util::is_upper_ascii) != tmp.end()) {
            // else handle special cases:
            //   - en_US_POSIX is an alias for C
            //   - M49 country code: 3 digits
            if(language_ == "en" && tmp == "US_POSIX") {
                language_ = "C";
                tmp.clear();
            } else if(tmp.size() != 3u || std::find_if_not(tmp.begin(), tmp.end(), util::is_numeric_ascii) != tmp.end())
                return false;
        }

        country_ = tmp;
        if(end >= input.size())
            return true;
        else if(input[end] == '.')
            return parse_from_encoding(input.substr(end + 1));
        else {
            BOOST_ASSERT_MSG(input[end] == '@', "Unexpected delimiter");
            return parse_from_variant(input.substr(end + 1));
        }
    }

    bool locale_data::parse_from_encoding(const std::string& input)
    {
        const auto end = input.find_first_of('@');
        std::string tmp = input.substr(0, end);
        if(tmp.empty())
            return false;
        // No assumptions, but uppercase
        encoding(std::move(tmp));
        if(end >= input.size())
            return true;
        else {
            BOOST_ASSERT_MSG(input[end] == '@', "Unexpected delimiter");
            return parse_from_variant(input.substr(end + 1));
        }
    }

    bool locale_data::parse_from_variant(const std::string& input)
    {
        if(language_ == "C")
            return false;
        if(input.empty())
            return false;
        variant_ = input;
        // No assumptions, just make it lowercase
        for(char& c : variant_)
            make_lower(c);
        return true;
    }

    locale_data& locale_data::encoding(std::string new_encoding, const bool uppercase)
    {
        if(uppercase) {
            for(char& c : new_encoding)
                make_upper(c);
        }
        encoding_ = std::move(new_encoding);
        utf8_ = util::normalize_encoding(encoding_) == "utf8";
        return *this;
    }

}}} // namespace boost::locale::util
