// Copyright Vladimir Prus 2002-2004.
// Distributed under the Boost Software License, Version 1.0.
// (See accompanying file LICENSE_1_0.txt
// or copy at http://www.boost.org/LICENSE_1_0.txt)


#ifndef BOOST_ERRORS_VP_2003_01_02
#define BOOST_ERRORS_VP_2003_01_02

#include <boost/program_options/config.hpp>

#include <string>
#include <stdexcept>
#include <vector>

#if defined(BOOST_MSVC)
#   pragma warning (push)
#   pragma warning (disable:4275) // non dll-interface class 'std::logic_error' used as base for dll-interface class 'boost::program_options::error'
#   pragma warning (disable:4251) // class 'std::vector<_Ty>' needs to have dll-interface to be used by clients of class 'boost::program_options::ambiguous_option'
#endif

namespace boost { namespace program_options {

    /** Base class for all errors in the library. */
    class BOOST_PROGRAM_OPTIONS_DECL error : public std::logic_error {
    public:
        error(const std::string& xwhat) : std::logic_error(xwhat) {}
    };

    class BOOST_PROGRAM_OPTIONS_DECL invalid_syntax : public error {
    public:
        enum kind_t {
            long_not_allowed = 30,
            long_adjacent_not_allowed,
            short_adjacent_not_allowed,
            empty_adjacent_parameter,
            missing_parameter,
            extra_parameter,
            unrecognized_line
        };
        
        invalid_syntax(const std::string& tokens, kind_t kind);

        // gcc says that throw specification on dtor is loosened
        // without this line
        ~invalid_syntax() throw() {}
        
        kind_t kind() const;
        
        const std::string& tokens() const;
        
    protected:
        /** Used to convert kind_t to a related error text */
        static std::string error_message(kind_t kind);

    private:
        // TODO: copy ctor might throw
        std::string m_tokens;

        kind_t m_kind;
    };

    /** Class thrown when option name is not recognized. */
    class BOOST_PROGRAM_OPTIONS_DECL unknown_option : public error {
    public:
        unknown_option(const std::string& name)
        : error(std::string("unknown option ").append(name)), 
          m_option_name(name)
        {}

        // gcc says that throw specification on dtor is loosened
        // without this line
        ~unknown_option() throw() {}
        
        const std::string& get_option_name() const throw();
        
    private:
        std::string m_option_name;
    };

    /** Class thrown when there's ambiguity amoung several possible options. */
    class BOOST_PROGRAM_OPTIONS_DECL ambiguous_option : public error {
    public:
        ambiguous_option(const std::string& name, 
                         const std::vector<std::string>& xalternatives)
        : error(std::string("ambiguous option ").append(name))
        , m_alternatives(xalternatives)
        , m_option_name(name)
        {}

        ~ambiguous_option() throw() {}
        
        const std::string& get_option_name() const throw();
        
        const std::vector<std::string>& alternatives() const throw();

    private:
        // TODO: copy ctor might throw
        std::vector<std::string> m_alternatives;
        std::string m_option_name;
    };

    /** Class thrown when there are several option values, but
        user called a method which cannot return them all. */
    class BOOST_PROGRAM_OPTIONS_DECL multiple_values : public error {
    public:
        multiple_values() 
         : error("multiple values")
         , m_option_name() {}
         
        ~multiple_values() throw() {}
        
        void set_option_name(const std::string& option);
        
        const std::string& get_option_name() const throw();
        
    private:
        std::string m_option_name; // The name of the option which
                                   // caused the exception.        
    };

    /** Class thrown when there are several occurrences of an
        option, but user called a method which cannot return 
        them all. */
    class BOOST_PROGRAM_OPTIONS_DECL multiple_occurrences : public error {
    public:
        multiple_occurrences() 
         : error("multiple occurrences")
         , m_option_name() {}
         
        ~multiple_occurrences() throw() {}
        
        void set_option_name(const std::string& option);
        
        const std::string& get_option_name() const throw();

    private:        
        std::string m_option_name; // The name of the option which
                                   // caused the exception.
    };

    /** Class thrown when value of option is incorrect. */
    class BOOST_PROGRAM_OPTIONS_DECL validation_error : public error {
    public:
        enum kind_t {
            multiple_values_not_allowed = 30,
            at_least_one_value_required, 
            invalid_bool_value,
            invalid_option_value,
            invalid_option
        };
        
        validation_error(kind_t kind, 
                         const std::string& option_value = "",
                         const std::string& option_name = "");
                         
        ~validation_error() throw() {}

        void set_option_name(const std::string& option);
        
        const std::string& get_option_name() const throw();
        
        const char* what() const throw();
        
    protected:
        /** Used to convert kind_t to a related error text */
        static std::string error_message(kind_t kind);

    private:
        kind_t m_kind;
        std::string m_option_name; // The name of the option which
                                   // caused the exception.
        std::string m_option_value; // Optional: value of the option m_options_name
        mutable std::string m_message; // For on-demand formatting in 'what'

    };

    /** Class thrown if there is an invalid option value givenn */
    class BOOST_PROGRAM_OPTIONS_DECL invalid_option_value 
        : public validation_error
    {
    public:
        invalid_option_value(const std::string& value);
#ifndef BOOST_NO_STD_WSTRING
        invalid_option_value(const std::wstring& value);
#endif
    };

    /** Class thrown when there are too many positional options. 
        This is a programming error.
    */
    class BOOST_PROGRAM_OPTIONS_DECL too_many_positional_options_error : public error {
    public:
        too_many_positional_options_error() 
         : error("too many positional options") 
        {}
    };

    /** Class thrown when there are syntax errors in given command line */
    class BOOST_PROGRAM_OPTIONS_DECL invalid_command_line_syntax : public invalid_syntax {
    public:
        invalid_command_line_syntax(const std::string& tokens, kind_t kind);
    };

    /** Class thrown when there are programming error related to style */
    class BOOST_PROGRAM_OPTIONS_DECL invalid_command_line_style : public error {
    public:
        invalid_command_line_style(const std::string& msg)
        : error(msg)
        {}
    };

    /** Class thrown if config file can not be read */
    class BOOST_PROGRAM_OPTIONS_DECL reading_file : public error {
    public:
        reading_file(const char* filename)
         : error(std::string("can not read file ").append(filename))
        {}
    };
    
     /** Class thrown when a required/mandatory option is missing */
     class BOOST_PROGRAM_OPTIONS_DECL required_option : public error {
     public:
        required_option(const std::string& name)
        : error(std::string("missing required option ").append(name))
        , m_option_name(name)
        {}
 
        ~required_option() throw() {}

        const std::string& get_option_name() const throw();
        
     private:
        std::string m_option_name; // The name of the option which
                                   // caused the exception.
     };
}}

#if defined(BOOST_MSVC)
#   pragma warning (pop)
#endif

#endif
