// Copyright Vladimir Prus 2004.
// Distributed under the Boost Software License, Version 1.0.
// (See accompanying file LICENSE_1_0.txt
// or copy at http://www.boost.org/LICENSE_1_0.txt)

#define BOOST_PROGRAM_OPTIONS_SOURCE
#include <boost/program_options/config.hpp>
#include <boost/program_options/value_semantic.hpp>
#include <boost/program_options/detail/convert.hpp>

#include <cctype>

namespace boost { namespace program_options {

    using namespace std;

    void 
    value_semantic_codecvt_helper<char>::
    parse(boost::any& value_store, 
          const std::vector<std::string>& new_tokens,
          bool utf8) const
    {
        if (utf8) {
#ifndef BOOST_NO_STD_WSTRING
            // Need to convert to local encoding.
            std::vector<string> local_tokens;
            for (unsigned i = 0; i < new_tokens.size(); ++i) {
                std::wstring w = from_utf8(new_tokens[i]);
                local_tokens.push_back(to_local_8_bit(w));
            }
            xparse(value_store, local_tokens);
#else
            boost::throw_exception(
                std::runtime_error("UTF-8 conversion not supported."));
#endif
        } else {
            // Already in local encoding, pass unmodified
            xparse(value_store, new_tokens);
        }        
    }

#ifndef BOOST_NO_STD_WSTRING
    void 
    value_semantic_codecvt_helper<wchar_t>::
    parse(boost::any& value_store, 
          const std::vector<std::string>& new_tokens,
          bool utf8) const
    {
        std::vector<wstring> tokens;
        if (utf8) {
            // Convert from utf8
            for (unsigned i = 0; i < new_tokens.size(); ++i) {
                tokens.push_back(from_utf8(new_tokens[i]));
            }
               
        } else {
            // Convert from local encoding
            for (unsigned i = 0; i < new_tokens.size(); ++i) {
                tokens.push_back(from_local_8_bit(new_tokens[i]));
            }
        }      

        xparse(value_store, tokens);  
    }
#endif

    BOOST_PROGRAM_OPTIONS_DECL std::string arg("arg");

    std::string
    untyped_value::name() const
    {
        return arg;
    }
    
    unsigned 
    untyped_value::min_tokens() const
    {
        if (m_zero_tokens)
            return 0;
        else
            return 1;
    }

    unsigned 
    untyped_value::max_tokens() const
    {
        if (m_zero_tokens)
            return 0;
        else
            return 1;
    }


    void 
    untyped_value::xparse(boost::any& value_store,
                          const std::vector<std::string>& new_tokens) const
    {
        if (!value_store.empty()) 
            boost::throw_exception(
                multiple_occurrences());
        if (new_tokens.size() > 1)
            boost::throw_exception(multiple_values());
        value_store = new_tokens.empty() ? std::string("") : new_tokens.front();
    }

    BOOST_PROGRAM_OPTIONS_DECL typed_value<bool>*
    bool_switch()
    {
        return bool_switch(0);
    }

    BOOST_PROGRAM_OPTIONS_DECL typed_value<bool>*
    bool_switch(bool* v)
    {
        typed_value<bool>* r = new typed_value<bool>(v);
        r->default_value(0);
        r->zero_tokens();

        return r;
    }

    /* Validates bool value.
        Any of "1", "true", "yes", "on" will be converted to "1".<br>
        Any of "0", "false", "no", "off" will be converted to "0".<br>
        Case is ignored. The 'xs' vector can either be empty, in which
        case the value is 'true', or can contain explicit value.
    */
    BOOST_PROGRAM_OPTIONS_DECL void validate(any& v, const vector<string>& xs,
                       bool*, int)
    {
        check_first_occurrence(v);
        string s(get_single_string(xs, true));

        for (size_t i = 0; i < s.size(); ++i)
            s[i] = char(tolower(s[i]));

        if (s.empty() || s == "on" || s == "yes" || s == "1" || s == "true")
            v = any(true);
        else if (s == "off" || s == "no" || s == "0" || s == "false")
            v = any(false);
        else
            boost::throw_exception(validation_error(validation_error::invalid_bool_value, s));
    }

    // This is blatant copy-paste. However, templating this will cause a problem,
    // since wstring can't be constructed/compared with char*. We'd need to
    // create auxiliary 'widen' routine to convert from char* into 
    // needed string type, and that's more work.
#if !defined(BOOST_NO_STD_WSTRING)
    BOOST_PROGRAM_OPTIONS_DECL 
    void validate(any& v, const vector<wstring>& xs, bool*, int)
    {
        check_first_occurrence(v);
        wstring s(get_single_string(xs, true));

        for (size_t i = 0; i < s.size(); ++i)
            s[i] = wchar_t(tolower(s[i]));

        if (s.empty() || s == L"on" || s == L"yes" || s == L"1" || s == L"true")
            v = any(true);
        else if (s == L"off" || s == L"no" || s == L"0" || s == L"false")
            v = any(false);
        else
            boost::throw_exception(validation_error(validation_error::invalid_bool_value));
    }
#endif
    BOOST_PROGRAM_OPTIONS_DECL 
    void validate(any& v, const vector<string>& xs, std::string*, int)
    {
        check_first_occurrence(v);
        v = any(get_single_string(xs));
    }

#if !defined(BOOST_NO_STD_WSTRING)
    BOOST_PROGRAM_OPTIONS_DECL 
    void validate(any& v, const vector<wstring>& xs, std::string*, int)
    {
        check_first_occurrence(v);
        v = any(get_single_string(xs));
    }
#endif

    namespace validators {

        BOOST_PROGRAM_OPTIONS_DECL 
        void check_first_occurrence(const boost::any& value)
        {
            if (!value.empty())
                boost::throw_exception(
                    multiple_occurrences());
        }
    }


    invalid_option_value::
    invalid_option_value(const std::string& bad_value)
    : validation_error(validation_error::invalid_option_value, bad_value)
    {}

#ifndef BOOST_NO_STD_WSTRING
    namespace
    {
        std::string convert_value(const std::wstring& s)
        {
            try {
                return to_local_8_bit(s);
            }
            catch(const std::exception&) {
                return "<unrepresentable unicode string>";
            }
        }
    }

    invalid_option_value::
    invalid_option_value(const std::wstring& bad_value)
    : validation_error(validation_error::invalid_option_value, convert_value(bad_value))
    {}
#endif
    const std::string& 
    unknown_option::get_option_name() const throw()
    { 
        return m_option_name; 
    }

    const std::string& 
    ambiguous_option::get_option_name() const throw()
    { 
        return m_option_name; 
    }
 
    const std::vector<std::string>& 
    ambiguous_option::alternatives() const throw()
    {
        return m_alternatives;
    }

    void 
    multiple_values::set_option_name(const std::string& option_name)
    {
        m_option_name = option_name;
    }

    const std::string& 
    multiple_values::get_option_name() const throw()
    {
        return m_option_name;
    }
    
    void 
    multiple_occurrences::set_option_name(const std::string& option_name)
    {
        m_option_name = option_name;
    }

    const std::string& 
    multiple_occurrences::get_option_name() const throw()
    {
        return m_option_name;
    }
        
    validation_error::    
    validation_error(kind_t kind, 
                     const std::string& option_value, 
                     const std::string& option_name)
     : error("")
     , m_kind(kind) 
     , m_option_name(option_name)
     , m_option_value(option_value)
     , m_message(error_message(kind))
    {
       if (!option_value.empty())
       {
          m_message.append(std::string("'") + option_value + std::string("'"));
       }
    }

    void 
    validation_error::set_option_name(const std::string& option_name)
    {
        m_option_name = option_name;
    }

    const std::string& 
    validation_error::get_option_name() const throw()
    {
        return m_option_name;
    }

    std::string 
    validation_error::error_message(kind_t kind)
    {
        // Initially, store the message in 'const char*' variable,
        // to avoid conversion to std::string in all cases.
        const char* msg;
        switch(kind)
        {
        case multiple_values_not_allowed:
            msg = "multiple values not allowed";
            break;
        case at_least_one_value_required:
            msg = "at least one value required";
            break;
        case invalid_bool_value:
            msg = "invalid bool value";
            break;
        case invalid_option_value:
            msg = "invalid option value";
            break;
        case invalid_option:
            msg = "invalid option";
            break;
        default:
            msg = "unknown error";
        }
        return msg;
    }

    const char* 
    validation_error::what() const throw()
    {
        if (!m_option_name.empty())
        {
            m_message = "in option '" + m_option_name + "': " 
                + error_message(m_kind);
        }
        return m_message.c_str();
    }
    
    const std::string& 
    required_option::get_option_name() const throw()
    {
        return m_option_name;
    }

}}
