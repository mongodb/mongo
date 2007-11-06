// Copyright Vladimir Prus 2004.
// Distributed under the Boost Software License, Version 1.0.
// (See accompanying file LICENSE_1_0.txt
// or copy at http://www.boost.org/LICENSE_1_0.txt)

// This file defines template functions that are declared in
// ../value_semantic.hpp.

#include <boost/throw_exception.hpp>

namespace boost { namespace program_options { 

    extern BOOST_PROGRAM_OPTIONS_DECL std::string arg;
    
    template<class T, class charT>
    std::string
    typed_value<T, charT>::name() const
    {
        if (!m_default_value.empty() && !m_default_value_as_text.empty()) {
            return arg + " (=" + m_default_value_as_text + ")";
        } else {
            return arg;
        }
    }

    template<class T, class charT>
    void 
    typed_value<T, charT>::notify(const boost::any& value_store) const
    {
        const T* value = boost::any_cast<const T>(&value_store);
        if (m_store_to) {
            *m_store_to = *value;
        }
        if (m_notifier) {
            m_notifier(*value);
        }
    }

    namespace validators {
        /* If v.size() > 1, throw validation_error. 
           If v.size() == 1, return v.front()
           Otherwise, returns a reference to a statically allocated
           empty string if 'allow_empty' and throws validation_error
           otherwise. */
        template<class charT>
        const std::basic_string<charT>& get_single_string(
            const std::vector<std::basic_string<charT> >& v, 
            bool allow_empty = false)
        {
            static std::basic_string<charT> empty;
            if (v.size() > 1)
                throw validation_error("multiple values not allowed");
            if (v.size() == 1)
                return v.front();
            else if (allow_empty)
                return empty;
            else
                throw validation_error("at least one value required");
        }

        /* Throws multiple_occurrences if 'value' is not empty. */
        BOOST_PROGRAM_OPTIONS_DECL void 
        check_first_occurrence(const boost::any& value);
    }

    using namespace validators;

    /** Validates 's' and updates 'v'.
        @pre 'v' is either empty or in the state assigned by the previous
        invocation of 'validate'.
        The target type is specified via a parameter which has the type of 
        pointer to the desired type. This is workaround for compilers without
        partial template ordering, just like the last 'long/int' parameter.
    */
    template<class T, class charT>
    void validate(boost::any& v, 
                  const std::vector< std::basic_string<charT> >& xs, 
                  T*, long)
    {
        validators::check_first_occurrence(v);
        std::basic_string<charT> s(validators::get_single_string(xs));
        try {
            v = any(lexical_cast<T>(s));
        }
        catch(const bad_lexical_cast&) {
            boost::throw_exception(invalid_option_value(s));
        }
    }

    BOOST_PROGRAM_OPTIONS_DECL void validate(boost::any& v, 
                       const std::vector<std::string>& xs, 
                       bool*,
                       int);

#if !defined(BOOST_NO_STD_WSTRING)
    BOOST_PROGRAM_OPTIONS_DECL void validate(boost::any& v, 
                       const std::vector<std::wstring>& xs, 
                       bool*,
                       int);
#endif
    // For some reason, this declaration, which is require by the standard,
    // cause gcc 3.2 to not generate code to specialization defined in
    // value_semantic.cpp
#if ! ( ( BOOST_WORKAROUND(__GNUC__, <= 3) &&\
          BOOST_WORKAROUND(__GNUC_MINOR__, < 3) ) || \
        ( BOOST_WORKAROUND(BOOST_MSVC, == 1310) ) \
      ) 
    BOOST_PROGRAM_OPTIONS_DECL void validate(boost::any& v, 
                       const std::vector<std::string>& xs,
                       std::string*,
                       int);

#if !defined(BOOST_NO_STD_WSTRING)
    BOOST_PROGRAM_OPTIONS_DECL void validate(boost::any& v, 
                       const std::vector<std::wstring>& xs,
                       std::string*,
                       int);
#endif
#endif

    /** Validates sequences. Allows multiple values per option occurrence
       and multiple occurrences. */
    template<class T, class charT>
    void validate(boost::any& v, 
                  const std::vector<std::basic_string<charT> >& s, 
                  std::vector<T>*,
                  int)
    {
        if (v.empty()) {
            v = boost::any(std::vector<T>());
        }
        std::vector<T>* tv = boost::any_cast< std::vector<T> >(&v);
        assert(NULL != tv);
        for (unsigned i = 0; i < s.size(); ++i)
        {
            try {
                tv->push_back(boost::lexical_cast<T>(s[i]));
            }
            catch(const bad_lexical_cast& /*e*/) {
                boost::throw_exception(invalid_option_value(s[i]));
            }
        }
    }

    template<class T, class charT>
    void 
    typed_value<T, charT>::
    xparse(boost::any& value_store, 
           const std::vector<std::basic_string<charT> >& new_tokens) const
    {
        validate(value_store, new_tokens, (T*)0, 0);
    }

    template<class T>
    typed_value<T>*
    value()
    {
        // Explicit qualification is vc6 workaround.
        return boost::program_options::value<T>(0);
    }

    template<class T>
    typed_value<T>*
    value(T* v)
    {
        typed_value<T>* r = new typed_value<T>(v);

        return r;        
    }

    template<class T>
    typed_value<T, wchar_t>*
    wvalue()
    {
        return wvalue<T>(0);
    }

    template<class T>
    typed_value<T, wchar_t>*
    wvalue(T* v)
    {
        typed_value<T, wchar_t>* r = new typed_value<T, wchar_t>(v);

        return r;        
    }



}}
