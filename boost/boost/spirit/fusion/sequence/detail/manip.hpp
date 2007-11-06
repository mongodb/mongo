/*=============================================================================
    Copyright (c) 1999-2003 Jeremiah Willcock
    Copyright (c) 1999-2003 Jaakko Järvi
    Copyright (c) 2001-2003 Joel de Guzman
    Copyright (c) 2004 Peder Holt

    Use, modification and distribution is subject to the Boost Software
    License, Version 1.0. (See accompanying file LICENSE_1_0.txt or copy at
    http://www.boost.org/LICENSE_1_0.txt)
==============================================================================*/
#ifndef FUSION_SEQUENCE_DETAIL_MANIP_HPP
#define FUSION_SEQUENCE_DETAIL_MANIP_HPP

#include <boost/config.hpp>
#include <iostream>
#include <string>
#include <vector>
#include <cctype>

// Tuple I/O manipulators

#include <boost/spirit/fusion/detail/config.hpp>

#if BOOST_WORKAROUND(BOOST_MSVC, < 1300)
FUSION_MSVC_ETI_WRAPPER(char_type)
FUSION_MSVC_ETI_WRAPPER(traits_type)
# define FUSION_GET_CHAR_TYPE(T) get_char_type<T>::type
# define FUSION_GET_TRAITS_TYPE(T) get_traits_type<T>::type
#else
# define FUSION_GET_CHAR_TYPE(T) typename T::char_type
# define FUSION_GET_TRAITS_TYPE(T) typename T::traits_type
#endif

#if defined (BOOST_NO_TEMPLATED_STREAMS)
#define FUSION_STRING_OF_STREAM(Stream) std::string
#else
#define FUSION_STRING_OF_STREAM(Stream)                                         \
    std::basic_string<                                                          \
        FUSION_GET_CHAR_TYPE(Stream)                                            \
      , FUSION_GET_TRAITS_TYPE(Stream)                                          \
    >
#endif

namespace boost { namespace fusion
{
    namespace detail
    {
        template <typename Tag>
        int get_xalloc_index(Tag* = 0)
        {
            // each Tag will have a unique index
            static int index = std::ios::xalloc();
            return index;
        }

        template <typename Stream, typename Tag, typename T>
        struct stream_data
        {
            struct arena
            {
                ~arena()
                {
                    for (
                        typename std::vector<T*>::iterator i = data.begin()
                      ; i != data.end()
                      ; ++i)
                    {
                        delete *i;
                    }
                }

                std::vector<T*> data;
            };

            static void attach(Stream& stream, T const& data);
            static T const* get(Stream& stream);
        };

        template <typename Stream, typename Tag, typename T>
        void stream_data<Stream,Tag,T>::attach(Stream& stream, T const& data)
        {
            static arena ar; // our arena
            ar.data.push_back(new T(data));
            stream.pword(get_xalloc_index<Tag>()) = ar.data.back();
        }

        template <typename Stream, typename Tag, typename T>
        T const* stream_data<Stream,Tag,T>::get(Stream& stream)
        {
            return (T const*)stream.pword(get_xalloc_index<Tag>());
        }

        template <class Tag, class Stream>
        class string_ios_manip
        {
        public:

            typedef FUSION_STRING_OF_STREAM(Stream) string_type;

            typedef stream_data<Stream, Tag, string_type> stream_data_t;

            string_ios_manip(Stream& str_);
            void set(string_type const& s);
            void print(char const* default_) const;
            void read(char const* default_) const;
        private:

            template <typename Char>
            void
            check_delim(Char c) const
            {
                if (!isspace(c))
                {
                    if (stream.get() != c)
                    {
                        stream.unget();
                        stream.setstate(std::ios::failbit);
                    }
                }
            }

            Stream& stream;
        };

        template <class Tag, class Stream>
        string_ios_manip<Tag,Stream>::string_ios_manip(Stream& str_)
            : stream(str_)
        {}

        template <class Tag, class Stream>
        void 
        string_ios_manip<Tag,Stream>::set(string_type const& s)
        {
            stream_data_t::attach(stream, s);
        }

        template <class Tag, class Stream>
        void 
        string_ios_manip<Tag,Stream>::print(char const* default_) const
        {
            // print a delimiter
            string_type const* p = stream_data_t::get(stream);
            if (p)
                stream << *p;
            else
                stream << default_;
        }

        template <class Tag, class Stream>
        void 
        string_ios_manip<Tag,Stream>::read(char const* default_) const
        {
            // read a delimiter
            string_type const* p = stream_data_t::get(stream);
            using namespace std;
            ws(stream);

            if (p)
            {
                typedef typename string_type::const_iterator iterator;
                for (iterator i = p->begin(); i != p->end(); ++i)
                    check_delim(*i);
            }
            else
            {
                while (*default_)
                    check_delim(*default_++);
            }
        }

    } // detail

#if defined (BOOST_NO_TEMPLATED_STREAMS)

#define STD_TUPLE_DEFINE_MANIPULATOR(name)                                      \
    namespace detail                                                            \
    {                                                                           \
        struct name##_tag;                                                      \
                                                                                \
        struct name##_type                                                      \
        {                                                                       \
            typedef std::string string_type;                                    \
            string_type data;                                                   \
            name##_type(const string_type& d): data(d) {}                       \
        };                                                                      \
                                                                                \
        template <class Stream>                                                 \
        Stream& operator>>(Stream& s, const name##_type& m)                     \
        {                                                                       \
            string_ios_manip<name##_tag, Stream>(s).set(m.data);                \
            return s;                                                           \
        }                                                                       \
                                                                                \
        template <class Stream>                                                 \
        Stream& operator<<(Stream& s, const name##_type& m)                     \
        {                                                                       \
            string_ios_manip<name##_tag, Stream>(s).set(m.data);                \
            return s;                                                           \
        }                                                                       \
    }

#define STD_TUPLE_DEFINE_MANIPULATOR_FUNCTIONS(name)                            \
    inline detail::name##_type                                                  \
    name(const std::string& s)                                                  \
    {                                                                           \
        return detail::name##_type(s);                                          \
    }                                                                           \
                                                                                \
    inline detail::name##_type                                                  \
    name(const char* s)                                                         \
    {                                                                           \
        return detail::name##_type(std::string(s));                             \
    }                                                                           \
                                                                                \
    inline detail::name##_type                                                  \
    name(char c)                                                                \
    {                                                                           \
        return detail::name##_type(std::string(1, c));                          \
    }

#else // defined(BOOST_NO_TEMPLATED_STREAMS)

#if defined(BOOST_NO_FUNCTION_TEMPLATE_ORDERING)

#define STD_TUPLE_DEFINE_MANIPULATOR_FUNCTIONS(name)                            \
    template <class Char, class Traits>                                         \
    inline detail::name##_type<Char, Traits>                                    \
    name(const std::basic_string<Char, Traits>& s)                              \
    {                                                                           \
        return detail::name##_type<Char, Traits>(s);                            \
    }                                                                           \
                                                                                \
    inline detail::name##_type<char>                                            \
    name(char const* s)                                                         \
    {                                                                           \
        return detail::name##_type<char>(std::basic_string<char>(s));           \
    }                                                                           \
                                                                                \
    inline detail::name##_type<wchar_t>                                         \
    name(wchar_t const* s)                                                      \
    {                                                                           \
        return detail::name##_type<wchar_t>(std::basic_string<wchar_t>(s));     \
    }                                                                           \
                                                                                \
    inline detail::name##_type<char>                                            \
    name(char c)                                                                \
    {                                                                           \
        return detail::name##_type<char>(std::basic_string<char>(1, c));        \
    }                                                                           \
                                                                                \
    inline detail::name##_type<wchar_t>                                         \
    name(wchar_t c)                                                             \
    {                                                                           \
        return detail::name##_type<wchar_t>(std::basic_string<wchar_t>(1, c));  \
    }

#else // defined(BOOST_NO_FUNCTION_TEMPLATE_ORDERING)

#define STD_TUPLE_DEFINE_MANIPULATOR_FUNCTIONS(name)                            \
    template <class Char, class Traits>                                         \
    inline detail::name##_type<Char, Traits>                                    \
    name(const std::basic_string<Char, Traits>& s)                              \
    {                                                                           \
        return detail::name##_type<Char, Traits>(s);                            \
    }                                                                           \
                                                                                \
    template <class Char>                                                       \
    inline detail::name##_type<Char>                                            \
    name(Char s[])                                                              \
    {                                                                           \
        return detail::name##_type<Char>(std::basic_string<Char>(s));           \
    }                                                                           \
                                                                                \
    template <class Char>                                                       \
    inline detail::name##_type<Char>                                            \
    name(Char const s[])                                                        \
    {                                                                           \
        return detail::name##_type<Char>(std::basic_string<Char>(s));           \
    }                                                                           \
                                                                                \
    template <class Char>                                                       \
    inline detail::name##_type<Char>                                            \
    name(Char c)                                                                \
    {                                                                           \
        return detail::name##_type<Char>(std::basic_string<Char>(1, c));        \
    }

#endif

#define STD_TUPLE_DEFINE_MANIPULATOR(name)                                      \
    namespace detail                                                            \
    {                                                                           \
        struct name##_tag;                                                      \
                                                                                \
        template <class Char, class Traits = std::char_traits<Char> >           \
        struct name##_type                                                      \
        {                                                                       \
            typedef std::basic_string<Char, Traits> string_type;                \
            string_type data;                                                   \
            name##_type(const string_type& d): data(d) {}                       \
        };                                                                      \
                                                                                \
        template <class Stream, class Char, class Traits>                       \
        Stream& operator>>(Stream& s, const name##_type<Char,Traits>& m)        \
        {                                                                       \
            string_ios_manip<name##_tag, Stream>(s).set(m.data);                \
            return s;                                                           \
        }                                                                       \
                                                                                \
        template <class Stream, class Char, class Traits>                       \
        Stream& operator<<(Stream& s, const name##_type<Char,Traits>& m)        \
        {                                                                       \
            string_ios_manip<name##_tag, Stream>(s).set(m.data);                \
            return s;                                                           \
        }                                                                       \
    }                                                                           \

#endif // defined(BOOST_NO_TEMPLATED_STREAMS)

    STD_TUPLE_DEFINE_MANIPULATOR(tuple_open)
    STD_TUPLE_DEFINE_MANIPULATOR(tuple_close)
    STD_TUPLE_DEFINE_MANIPULATOR(tuple_delimiter)

    STD_TUPLE_DEFINE_MANIPULATOR_FUNCTIONS(tuple_open)
    STD_TUPLE_DEFINE_MANIPULATOR_FUNCTIONS(tuple_close)
    STD_TUPLE_DEFINE_MANIPULATOR_FUNCTIONS(tuple_delimiter)

#undef STD_TUPLE_DEFINE_MANIPULATOR
#undef STD_TUPLE_DEFINE_MANIPULATOR_FUNCTIONS
#undef FUSION_STRING_OF_STREAM
#undef FUSION_GET_CHAR_TYPE
#undef FUSION_GET_TRAITS_TYPE

}}

#endif
