//
// Copyright (c) Antony Polukhin, 2012-2014.
//
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//

#ifndef BOOST_TYPE_INDEX_DETAIL_COMPILE_TIME_TYPE_INFO_HPP
#define BOOST_TYPE_INDEX_DETAIL_COMPILE_TIME_TYPE_INFO_HPP

/// \file compile_time_type_info.hpp
/// \brief Contains helper macros and implementation details of boost::typeindex::ctti_type_index.
/// Not intended for inclusion from user's code. 

#include <boost/config.hpp>
#include <boost/static_assert.hpp>
#include <boost/mpl/bool.hpp>
#include <algorithm>

#ifdef BOOST_HAS_PRAGMA_ONCE
# pragma once
#endif

/// @cond
#define BOOST_TYPE_INDEX_REGISTER_CTTI_PARSING_PARAMS(begin_skip, end_skip, runtime_skip, runtime_skip_until)   \
    namespace boost { namespace typeindex { namespace detail {                                                  \
        BOOST_STATIC_CONSTEXPR std::size_t ctti_skip_size_at_begin  = begin_skip;                               \
        BOOST_STATIC_CONSTEXPR std::size_t ctti_skip_size_at_end    = end_skip;                                 \
        BOOST_STATIC_CONSTEXPR bool ctti_skip_more_at_runtime       = runtime_skip;                             \
        BOOST_STATIC_CONSTEXPR char ctti_skip_until_runtime[]       = runtime_skip_until;                       \
    }}} /* namespace boost::typeindex::detail */                                                                \
    /**/  
/// @endcond

#if defined(BOOST_TYPE_INDEX_DOXYGEN_INVOKED)
    /* Nothing to document. All the macro docs are moved to <boost/type_index.hpp> */
#elif defined(BOOST_TYPE_INDEX_CTTI_USER_DEFINED_PARSING)

#   include <boost/preprocessor/facilities/expand.hpp>
    BOOST_PP_EXPAND( BOOST_TYPE_INDEX_REGISTER_CTTI_PARSING_PARAMS BOOST_TYPE_INDEX_CTTI_USER_DEFINED_PARSING )

#elif defined(_MSC_VER)
    // sizeof("const char *__cdecl boost::detail::ctti<") - 1, sizeof(">::n(void)") - 1
    BOOST_TYPE_INDEX_REGISTER_CTTI_PARSING_PARAMS(40, 10, false, "")
#elif defined(__clang__) && defined(__APPLE__)
    // Someone made __clang_major__ equal to LLVM version rather than compiler version
    // on APPLE platform.
    //
    // Using less efficient solution because there is no good way to detect real version of Clang.
    // sizeof("static const char *boost::detail::ctti<") - 1, sizeof("]") - 1, true, "???????????>::n() [T = int"
    BOOST_TYPE_INDEX_REGISTER_CTTI_PARSING_PARAMS(39, 1, true, "T = ")
#elif defined(__clang__) && (__clang_major__ < 3 || (__clang_major__ == 3 && __clang_minor__ == 0))
    // sizeof("static const char *boost::detail::ctti<") - 1, sizeof(">::n()") - 1
    // note: checked on 3.0
    BOOST_TYPE_INDEX_REGISTER_CTTI_PARSING_PARAMS(39, 6, false, "")
#elif defined(__clang__) && __clang_major__ == 3 && __clang_minor__ > 0
    // sizeof("static const char *boost::detail::ctti<") - 1, sizeof("]") - 1, true, "int>::n() [T = int"
    // note: checked on 3.1, 3.4
    BOOST_TYPE_INDEX_REGISTER_CTTI_PARSING_PARAMS(39, 1, true, "T = ")
#elif defined(__GNUC__)
    // sizeof("static const char* boost::detail::ctti<T>::n() [with T = ") - 1, sizeof("]") - 1
    BOOST_TYPE_INDEX_REGISTER_CTTI_PARSING_PARAMS(57, 1, false, "")
#else
    // Deafult code for other platforms... Just skip nothing!
    BOOST_TYPE_INDEX_REGISTER_CTTI_PARSING_PARAMS(0, 0, false, "")
#endif

#undef BOOST_TYPE_INDEX_REGISTER_CTTI_PARSING_PARAMS

namespace boost { namespace typeindex { namespace detail { 
    template <bool Condition>
    inline void assert_compile_time_legths() BOOST_NOEXCEPT {
        BOOST_STATIC_ASSERT_MSG(
            Condition,
            "TypeIndex library is misconfigured for your compiler. "
            "Please define BOOST_TYPE_INDEX_CTTI_USER_DEFINED_PARSING to correct values. See section "
            "'RTTI emulation limitations' of the documentation for more information."
        );
    }
    
    template <std::size_t ArrayLength>
    inline const char* skip_begining_runtime(const char* begin, boost::mpl::false_) BOOST_NOEXCEPT {
        return begin;
    }

    template <std::size_t ArrayLength>
    inline const char* skip_begining_runtime(const char* begin, boost::mpl::true_) BOOST_NOEXCEPT {
        const char* const it = std::search(
            begin, begin + ArrayLength,
            ctti_skip_until_runtime, ctti_skip_until_runtime + sizeof(ctti_skip_until_runtime) - 1
        );
        return (it == begin + ArrayLength ? begin : it + sizeof(ctti_skip_until_runtime) - 1);
    }

    template <std::size_t ArrayLength>
    inline const char* skip_begining(const char* begin) BOOST_NOEXCEPT {
        assert_compile_time_legths<(ArrayLength > ctti_skip_size_at_begin + ctti_skip_size_at_end)>();
        return skip_begining_runtime<ArrayLength - ctti_skip_size_at_begin>(
            begin + ctti_skip_size_at_begin, 
            boost::mpl::bool_<ctti_skip_more_at_runtime>()
        );
    }
}}} // namespace boost::typeindex::detail

namespace boost { namespace detail {

/// Noncopyable type_info that does not require RTTI.
/// CTTI == Compile Time Type Info.
/// This name must be as short as possible, to avoid code bloat
template <class T>
struct ctti {

    /// Returns raw name. Must be as short, as possible, to avoid code bloat
    static const char* n() BOOST_NOEXCEPT {
    #if defined(BOOST_TYPE_INDEX_FUNCTION_SIGNATURE)
        return boost::typeindex::detail::skip_begining< sizeof(BOOST_TYPE_INDEX_FUNCTION_SIGNATURE >(BOOST_TYPE_INDEX_FUNCTION_SIGNATURE);
    #elif defined(__FUNCSIG__)
        return boost::typeindex::detail::skip_begining< sizeof(__FUNCSIG__) >(__FUNCSIG__);
    #elif defined(__PRETTY_FUNCTION__) \
                || defined(__GNUC__) \
                || (defined(__SUNPRO_CC) && (__SUNPRO_CC >= 0x5130)) \
                || (defined(__MWERKS__) && (__MWERKS__ >= 0x3000)) \
                || (defined(__ICC) && (__ICC >= 600)) \
                || defined(__ghs__) \
                || defined(__DMC__)

        return boost::typeindex::detail::skip_begining< sizeof(__PRETTY_FUNCTION__) >(__PRETTY_FUNCTION__);
    #else
        BOOST_STATIC_ASSERT_MSG(
            sizeof(T) && false,
            "TypeIndex library could not detect your compiler. "
            "Please make the BOOST_TYPE_INDEX_FUNCTION_SIGNATURE macro use "
            "correct compiler macro for getting the whole function name. "
            "Define BOOST_TYPE_INDEX_CTTI_USER_DEFINED_PARSING to correct value after that."
        );
    #endif
    }
};

}} // namespace boost::detail

#endif // BOOST_TYPE_INDEX_DETAIL_COMPILE_TIME_TYPE_INFO_HPP

