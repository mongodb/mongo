//
// Copyright (c) 2009-2011 Artyom Beilis (Tonkikh)
// Copyright (c) 2021-2023 Alexander Grund
//
// Distributed under the Boost Software License, Version 1.0.
// https://www.boost.org/LICENSE_1_0.txt

#ifndef BOOST_LOCALE_MESSAGE_HPP_INCLUDED
#define BOOST_LOCALE_MESSAGE_HPP_INCLUDED

#include <boost/locale/detail/facet_id.hpp>
#include <boost/locale/detail/is_supported_char.hpp>
#include <boost/locale/formatting.hpp>
#include <boost/locale/util/string.hpp>
#include <locale>
#include <memory>
#include <set>
#include <string>
#include <type_traits>

#ifdef BOOST_MSVC
#    pragma warning(push)
#    pragma warning(disable : 4275 4251 4231 4660)
#endif

// glibc < 2.3.4 declares those as macros if compiled with optimization turned on
#ifdef gettext
#    undef gettext
#    undef ngettext
#    undef dgettext
#    undef dngettext
#endif

namespace boost { namespace locale {
    ///
    /// \defgroup message Message Formatting (translation)
    ///
    /// This module provides message translation functionality, i.e. allow your application to speak native language
    ///
    /// @{
    ///

    /// Type used for the count/n argument to the translation functions choosing between singular and plural forms
    using count_type = long long;

    /// \brief This facet provides message formatting abilities
    template<typename CharType>
    class BOOST_SYMBOL_VISIBLE message_format : public std::locale::facet,
                                                public detail::facet_id<message_format<CharType>> {
        BOOST_LOCALE_ASSERT_IS_SUPPORTED(CharType);

    public:
        /// Character type
        typedef CharType char_type;
        /// String type
        typedef std::basic_string<CharType> string_type;

        /// Standard constructor
        message_format(size_t refs = 0) : std::locale::facet(refs) {}

        /// This function returns a pointer to the string for a message defined by a \a context
        /// and identification string \a id. Both create a single key for message lookup in
        /// a domain defined by \a domain_id.
        ///
        /// If \a context is NULL it is not considered to be a part of the key
        ///
        /// If a translated string is found, it is returned, otherwise NULL is returned
        virtual const char_type* get(int domain_id, const char_type* context, const char_type* id) const = 0;

        /// This function returns a pointer to the string for a plural message defined by a \a context
        /// and identification string \a single_id.
        ///
        /// If \a context is NULL it is not considered to be a part of the key
        ///
        /// Both create a single key for message lookup in
        /// a domain defined \a domain_id. \a n is used to pick the correct translation string for a specific
        /// number.
        ///
        /// If a translated string is found, it is returned, otherwise NULL is returned
        virtual const char_type*
        get(int domain_id, const char_type* context, const char_type* single_id, count_type n) const = 0;

        /// Convert a string that defines \a domain to the integer id used by \a get functions
        virtual int domain(const std::string& domain) const = 0;

        /// Convert the string \a msg to target locale's encoding. If \a msg is already
        /// in target encoding it would be returned otherwise the converted
        /// string is stored in temporary \a buffer and buffer.c_str() is returned.
        ///
        /// Note: for char_type that is char16_t, char32_t and wchar_t it is no-op, returns
        /// msg
        virtual const char_type* convert(const char_type* msg, string_type& buffer) const = 0;
    };

    /// \cond INTERNAL

    namespace detail {
        inline bool is_us_ascii_char(char c)
        {
            // works for null terminated strings regardless char "signedness"
            return 0 < c && c < 0x7F;
        }
        inline bool is_us_ascii_string(const char* msg)
        {
            while(*msg) {
                if(!is_us_ascii_char(*msg++))
                    return false;
            }
            return true;
        }

        template<typename CharType>
        struct string_cast_traits {
            static const CharType* cast(const CharType* msg, std::basic_string<CharType>& /*unused*/) { return msg; }
        };

        template<>
        struct string_cast_traits<char> {
            static const char* cast(const char* msg, std::string& buffer)
            {
                if(is_us_ascii_string(msg))
                    return msg;
                buffer.reserve(strlen(msg));
                char c;
                while((c = *msg++) != 0) {
                    if(is_us_ascii_char(c))
                        buffer += c;
                }
                return buffer.c_str();
            }
        };
    } // namespace detail

    /// \endcond

    /// \brief This class represents a message that can be converted to a specific locale message
    ///
    /// It holds the original ASCII string that is queried in the dictionary when converting to the output string.
    /// The created string may be UTF-8, UTF-16, UTF-32 or other 8-bit encoded string according to the target
    /// character type and locale encoding.
    template<typename CharType>
    class basic_message {
    public:
        typedef CharType char_type;                       ///< The character this message object is used with
        typedef std::basic_string<char_type> string_type; ///< The string type this object can be used with
        typedef message_format<char_type> facet_type;     ///< The type of the facet the messages are fetched with

        /// Create default empty message
        basic_message() : n_(0), c_id_(nullptr), c_context_(nullptr), c_plural_(nullptr) {}

        /// Create a simple message from 0 terminated string. The string should exist
        /// until the message is destroyed. Generally useful with static constant strings
        explicit basic_message(const char_type* id) : n_(0), c_id_(id), c_context_(nullptr), c_plural_(nullptr) {}

        /// Create a simple plural form message from 0 terminated strings. The strings should exist
        /// until the message is destroyed. Generally useful with static constant strings.
        ///
        /// \a n is the number, \a single and \a plural are singular and plural forms of the message
        explicit basic_message(const char_type* single, const char_type* plural, count_type n) :
            n_(n), c_id_(single), c_context_(nullptr), c_plural_(plural)
        {}

        /// Create a simple message from 0 terminated strings, with context
        /// information. The string should exist
        /// until the message is destroyed. Generally useful with static constant strings
        explicit basic_message(const char_type* context, const char_type* id) :
            n_(0), c_id_(id), c_context_(context), c_plural_(nullptr)
        {}

        /// Create a simple plural form message from 0 terminated strings, with context. The strings should exist
        /// until the message is destroyed. Generally useful with static constant strings.
        ///
        /// \a n is the number, \a single and \a plural are singular and plural forms of the message
        explicit basic_message(const char_type* context,
                               const char_type* single,
                               const char_type* plural,
                               count_type n) :
            n_(n),
            c_id_(single), c_context_(context), c_plural_(plural)
        {}

        /// Create a simple message from a string.
        explicit basic_message(const string_type& id) :
            n_(0), c_id_(nullptr), c_context_(nullptr), c_plural_(nullptr), id_(id)
        {}

        /// Create a simple plural form message from strings.
        ///
        /// \a n is the number, \a single and \a plural are single and plural forms of the message
        explicit basic_message(const string_type& single, const string_type& plural, count_type number) :
            n_(number), c_id_(nullptr), c_context_(nullptr), c_plural_(nullptr), id_(single), plural_(plural)
        {}

        /// Create a simple message from a string with context.
        explicit basic_message(const string_type& context, const string_type& id) :
            n_(0), c_id_(nullptr), c_context_(nullptr), c_plural_(nullptr), id_(id), context_(context)
        {}

        /// Create a simple plural form message from strings.
        ///
        /// \a n is the number, \a single and \a plural are single and plural forms of the message
        explicit basic_message(const string_type& context,
                               const string_type& single,
                               const string_type& plural,
                               count_type number) :
            n_(number),
            c_id_(nullptr), c_context_(nullptr), c_plural_(nullptr), id_(single), context_(context), plural_(plural)
        {}

        /// Copy an object
        basic_message(const basic_message&) = default;
        basic_message(basic_message&&) noexcept = default;

        /// Assign other message object to this one
        basic_message& operator=(const basic_message&) = default;
        basic_message&
        operator=(basic_message&&) noexcept(std::is_nothrow_move_assignable<string_type>::value) = default;

        /// Swap two message objects
        void
        swap(basic_message& other) noexcept(noexcept(std::declval<string_type&>().swap(std::declval<string_type&>())))
        {
            using std::swap;
            swap(n_, other.n_);
            swap(c_id_, other.c_id_);
            swap(c_context_, other.c_context_);
            swap(c_plural_, other.c_plural_);
            swap(id_, other.id_);
            swap(context_, other.context_);
            swap(plural_, other.plural_);
        }
        friend void swap(basic_message& x, basic_message& y) noexcept(noexcept(x.swap(y))) { x.swap(y); }

        /// Message class can be explicitly converted to string class
        operator string_type() const { return str(); }

        /// Translate message to a string in the default global locale, using default domain
        string_type str() const { return str(std::locale()); }

        /// Translate message to a string in the locale \a locale, using default domain
        string_type str(const std::locale& locale) const { return str(locale, 0); }

        /// Translate message to a string using locale \a locale and message domain  \a domain_id
        string_type str(const std::locale& locale, const std::string& domain_id) const
        {
            int id = 0;
            if(std::has_facet<facet_type>(locale))
                id = std::use_facet<facet_type>(locale).domain(domain_id);
            return str(locale, id);
        }

        /// Translate message to a string using the default locale and message domain  \a domain_id
        string_type str(const std::string& domain_id) const { return str(std::locale(), domain_id); }

        /// Translate message to a string using locale \a loc and message domain index  \a id
        string_type str(const std::locale& loc, int id) const
        {
            string_type buffer;
            const char_type* ptr = write(loc, id, buffer);
            if(ptr != buffer.c_str())
                buffer = ptr;
            return buffer;
        }

        /// Translate message and write to stream \a out, using imbued locale and domain set to the
        /// stream
        void write(std::basic_ostream<char_type>& out) const
        {
            const std::locale& loc = out.getloc();
            int id = ios_info::get(out).domain_id();
            string_type buffer;
            out << write(loc, id, buffer);
        }

    private:
        const char_type* plural() const
        {
            if(c_plural_)
                return c_plural_;
            if(plural_.empty())
                return nullptr;
            return plural_.c_str();
        }
        const char_type* context() const
        {
            if(c_context_)
                return c_context_;
            if(context_.empty())
                return nullptr;
            return context_.c_str();
        }

        const char_type* id() const { return c_id_ ? c_id_ : id_.c_str(); }

        const char_type* write(const std::locale& loc, int domain_id, string_type& buffer) const
        {
            static const char_type empty_string[1] = {0};

            const char_type* id = this->id();
            const char_type* context = this->context();
            const char_type* plural = this->plural();

            if(*id == 0)
                return empty_string;

            const facet_type* facet = nullptr;
            if(std::has_facet<facet_type>(loc))
                facet = &std::use_facet<facet_type>(loc);

            const char_type* translated = nullptr;
            if(facet) {
                if(!plural)
                    translated = facet->get(domain_id, context, id);
                else
                    translated = facet->get(domain_id, context, id, n_);
            }

            if(!translated) {
                const char_type* msg = plural ? (n_ == 1 ? id : plural) : id;

                if(facet)
                    translated = facet->convert(msg, buffer);
                else
                    translated = detail::string_cast_traits<char_type>::cast(msg, buffer);
            }
            return translated;
        }

        /// members

        count_type n_;
        const char_type* c_id_;
        const char_type* c_context_;
        const char_type* c_plural_;
        string_type id_;
        string_type context_;
        string_type plural_;
    };

    /// Convenience typedef for char
    typedef basic_message<char> message;
    /// Convenience typedef for wchar_t
    typedef basic_message<wchar_t> wmessage;
#ifdef __cpp_lib_char8_t
    /// Convenience typedef for char8_t
    typedef basic_message<char8_t> u8message;
#endif
#ifdef BOOST_LOCALE_ENABLE_CHAR16_T
    /// Convenience typedef for char16_t
    typedef basic_message<char16_t> u16message;
#endif
#ifdef BOOST_LOCALE_ENABLE_CHAR32_T
    /// Convenience typedef for char32_t
    typedef basic_message<char32_t> u32message;
#endif

    /// Translate message \a msg and write it to stream
    template<typename CharType>
    std::basic_ostream<CharType>& operator<<(std::basic_ostream<CharType>& out, const basic_message<CharType>& msg)
    {
        msg.write(out);
        return out;
    }

    /// \anchor boost_locale_translate_family \name Indirect message translation function family
    /// @{

    /// \brief Translate a message, \a msg is not copied
    template<typename CharType>
    inline basic_message<CharType> translate(const CharType* msg)
    {
        return basic_message<CharType>(msg);
    }

    /// \brief Translate a message in context, \a msg and \a context are not copied
    template<typename CharType>
    inline basic_message<CharType> translate(const CharType* context, const CharType* msg)
    {
        return basic_message<CharType>(context, msg);
    }

    /// \brief Translate a plural message form, \a single and \a plural are not copied
    template<typename CharType>
    inline basic_message<CharType> translate(const CharType* single, const CharType* plural, count_type n)
    {
        return basic_message<CharType>(single, plural, n);
    }

    /// \brief Translate a plural message from in context, \a context, \a single and \a plural are not copied
    template<typename CharType>
    inline basic_message<CharType>
    translate(const CharType* context, const CharType* single, const CharType* plural, count_type n)
    {
        return basic_message<CharType>(context, single, plural, n);
    }

    /// \brief Translate a message, \a msg is copied
    template<typename CharType>
    inline basic_message<CharType> translate(const std::basic_string<CharType>& msg)
    {
        return basic_message<CharType>(msg);
    }

    /// \brief Translate a message in context,\a context and \a msg is copied
    template<typename CharType>
    inline basic_message<CharType> translate(const std::basic_string<CharType>& context,
                                             const std::basic_string<CharType>& msg)
    {
        return basic_message<CharType>(context, msg);
    }

    /// \brief Translate a plural message form in context, \a context, \a single and \a plural are copied
    template<typename CharType>
    inline basic_message<CharType> translate(const std::basic_string<CharType>& context,
                                             const std::basic_string<CharType>& single,
                                             const std::basic_string<CharType>& plural,
                                             count_type n)
    {
        return basic_message<CharType>(context, single, plural, n);
    }

    /// \brief Translate a plural message form, \a single and \a plural are copied
    template<typename CharType>
    inline basic_message<CharType>
    translate(const std::basic_string<CharType>& single, const std::basic_string<CharType>& plural, count_type n)
    {
        return basic_message<CharType>(single, plural, n);
    }

    /// @}

    /// \anchor boost_locale_gettext_family \name Direct message translation functions family

    /// Translate message \a id according to locale \a loc
    template<typename CharType>
    std::basic_string<CharType> gettext(const CharType* id, const std::locale& loc = std::locale())
    {
        return basic_message<CharType>(id).str(loc);
    }
    /// Translate plural form according to locale \a loc
    template<typename CharType>
    std::basic_string<CharType>
    ngettext(const CharType* s, const CharType* p, count_type n, const std::locale& loc = std::locale())
    {
        return basic_message<CharType>(s, p, n).str(loc);
    }

    /// Translate message \a id according to locale \a loc in domain \a domain
    template<typename CharType>
    std::basic_string<CharType> dgettext(const char* domain, const CharType* id, const std::locale& loc = std::locale())
    {
        return basic_message<CharType>(id).str(loc, domain);
    }

    /// Translate plural form according to locale \a loc in domain \a domain
    template<typename CharType>
    std::basic_string<CharType> dngettext(const char* domain,
                                          const CharType* s,
                                          const CharType* p,
                                          count_type n,
                                          const std::locale& loc = std::locale())
    {
        return basic_message<CharType>(s, p, n).str(loc, domain);
    }

    /// Translate message \a id according to locale \a loc in context \a context
    template<typename CharType>
    std::basic_string<CharType>
    pgettext(const CharType* context, const CharType* id, const std::locale& loc = std::locale())
    {
        return basic_message<CharType>(context, id).str(loc);
    }

    /// Translate plural form according to locale \a loc in context \a context
    template<typename CharType>
    std::basic_string<CharType> npgettext(const CharType* context,
                                          const CharType* s,
                                          const CharType* p,
                                          count_type n,
                                          const std::locale& loc = std::locale())
    {
        return basic_message<CharType>(context, s, p, n).str(loc);
    }

    /// Translate message \a id according to locale \a loc in domain \a domain in context \a context
    template<typename CharType>
    std::basic_string<CharType>
    dpgettext(const char* domain, const CharType* context, const CharType* id, const std::locale& loc = std::locale())
    {
        return basic_message<CharType>(context, id).str(loc, domain);
    }

    /// Translate plural form according to locale \a loc in domain \a domain in context \a context
    template<typename CharType>
    std::basic_string<CharType> dnpgettext(const char* domain,
                                           const CharType* context,
                                           const CharType* s,
                                           const CharType* p,
                                           count_type n,
                                           const std::locale& loc = std::locale())
    {
        return basic_message<CharType>(context, s, p, n).str(loc, domain);
    }

    /// @}

    namespace as {
        /// \cond INTERNAL
        namespace detail {
            struct set_domain {
                std::string domain_id;
            };
            template<typename CharType>
            std::basic_ostream<CharType>& operator<<(std::basic_ostream<CharType>& out, const set_domain& dom)
            {
                int id = std::use_facet<message_format<CharType>>(out.getloc()).domain(dom.domain_id);
                ios_info::get(out).domain_id(id);
                return out;
            }
        } // namespace detail
        /// \endcond

        /// \addtogroup manipulators
        ///
        /// @{

        /// Manipulator for switching message domain in ostream,
        ///
        /// \note The returned object throws std::bad_cast if the I/O stream does not have \ref message_format facet
        /// installed
        inline
#ifdef BOOST_LOCALE_DOXYGEN
          unspecified_type
#else
          detail::set_domain
#endif
          domain(const std::string& id)
        {
            detail::set_domain tmp = {id};
            return tmp;
        }
        /// @}
    } // namespace as
}}    // namespace boost::locale

#ifdef BOOST_MSVC
#    pragma warning(pop)
#endif

#endif
