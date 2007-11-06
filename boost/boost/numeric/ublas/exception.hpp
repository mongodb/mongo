//
//  Copyright (c) 2000-2002
//  Joerg Walter, Mathias Koch
//
//  Permission to use, copy, modify, distribute and sell this software
//  and its documentation for any purpose is hereby granted without fee,
//  provided that the above copyright notice appear in all copies and
//  that both that copyright notice and this permission notice appear
//  in supporting documentation.  The authors make no representations
//  about the suitability of this software for any purpose.
//  It is provided "as is" without express or implied warranty.
//
//  The authors gratefully acknowledge the support of
//  GeNeSys mbH & Co. KG in producing this work.
//

#ifndef _BOOST_UBLAS_EXCEPTION_
#define _BOOST_UBLAS_EXCEPTION_

#if ! defined (BOOST_NO_EXCEPTIONS) && ! defined (BOOST_UBLAS_NO_EXCEPTIONS)
#include <stdexcept>
#else
#include <cstdlib>
#endif
#ifndef BOOST_UBLAS_NO_STD_CERR
#include <iostream>
#endif

#include <boost/numeric/ublas/detail/config.hpp>

namespace boost { namespace numeric { namespace ublas {

    struct divide_by_zero
#if ! defined (BOOST_NO_EXCEPTIONS) && ! defined (BOOST_UBLAS_NO_EXCEPTIONS)
        // Inherit from standard exceptions as requested during review.
        : public std::runtime_error {
        explicit divide_by_zero (const char *s = "divide by zero") :
            std::runtime_error (s) {}
        void raise () {
            throw *this;
        }
#else
    {
        divide_by_zero ()
            {}
        explicit divide_by_zero (const char *)
            {}
        void raise () {
            std::abort ();
        }
#endif
    };

    struct internal_logic
#if ! defined (BOOST_NO_EXCEPTIONS) && ! defined (BOOST_UBLAS_NO_EXCEPTIONS)
        // Inherit from standard exceptions as requested during review.
        : public std::logic_error {
        explicit internal_logic (const char *s = "internal logic") :
            std::logic_error (s) {}
        void raise () {
            throw *this;
        }
#else
    {
        internal_logic ()
            {}
        explicit internal_logic (const char *)
            {}
        void raise () {
            std::abort ();
        }
#endif
    };

    struct external_logic
#if ! defined (BOOST_NO_EXCEPTIONS) && ! defined (BOOST_UBLAS_NO_EXCEPTIONS)
        // Inherit from standard exceptions as requested during review.
        : public std::logic_error {
        explicit external_logic (const char *s = "external logic") :
            std::logic_error (s) {}
        // virtual const char *what () const throw () {
        //     return "exception: external logic";
        // }
        void raise () {
            throw *this;
        }
#else
    {
        external_logic ()
            {}
        explicit external_logic (const char *)
            {}
        void raise () {
            std::abort ();
        }
#endif
    };

    struct bad_argument
#if ! defined (BOOST_NO_EXCEPTIONS) && ! defined (BOOST_UBLAS_NO_EXCEPTIONS)
        // Inherit from standard exceptions as requested during review.
        : public std::invalid_argument {
        explicit bad_argument (const char *s = "bad argument") :
            std::invalid_argument (s) {}
        void raise () {
            throw *this;
        }
#else
    {
        bad_argument ()
            {}
        explicit bad_argument (const char *)
            {}
        void raise () {
            std::abort ();
        }
#endif
    };

    struct bad_size
#if ! defined (BOOST_NO_EXCEPTIONS) && ! defined (BOOST_UBLAS_NO_EXCEPTIONS)
        // Inherit from standard exceptions as requested during review.
        : public std::domain_error {
        explicit bad_size (const char *s = "bad size") :
            std::domain_error (s) {}
        void raise () {
            throw *this;
        }
#else
    {
        bad_size ()
            {}
        explicit bad_size (const char *)
            {}
        void raise () {
            std::abort ();
        }
#endif
    };

    struct bad_index
#if ! defined (BOOST_NO_EXCEPTIONS) && ! defined (BOOST_UBLAS_NO_EXCEPTIONS)
        // Inherit from standard exceptions as requested during review.
        : public std::out_of_range {
        explicit bad_index (const char *s = "bad index") :
            std::out_of_range (s) {}
        void raise () {
            throw *this;
        }
#else
    {
        bad_index ()
            {}
        explicit bad_index (const char *)
            {}
        void raise () {
            std::abort ();
        }
#endif
    };

    struct singular
#if ! defined (BOOST_NO_EXCEPTIONS) && ! defined (BOOST_UBLAS_NO_EXCEPTIONS)
        // Inherit from standard exceptions as requested during review.
        : public std::runtime_error {
        explicit singular (const char *s = "singular") :
            std::runtime_error (s) {}
        void raise () {
            throw *this;
        }
#else
    {
        singular ()
            {}
        explicit singular (const char *)
            {}
        void raise () {
            throw *this;
            std::abort ();
        }
#endif
    };

    struct non_real
#if ! defined (BOOST_NO_EXCEPTIONS) && ! defined (BOOST_UBLAS_NO_EXCEPTIONS)
        // Inherit from standard exceptions as requested during review.
        : public std::domain_error {
        explicit non_real (const char *s = "exception: non real") :
            std::domain_error (s) {}
        void raise () {
            throw *this;
        }
#else
     {
        non_real ()
            {}
        explicit non_real (const char *)
            {}
        void raise () {
            std::abort ();
        }
#endif
    };

#if BOOST_UBLAS_CHECK_ENABLE
// Macros are equivilent to 
//    template<class E>
//    BOOST_UBLAS_INLINE
//    void check (bool expression, const E &e) {
//        if (! expression)
//            e.raise ();
//    }
//    template<class E>
//    BOOST_UBLAS_INLINE
//    void check_ex (bool expression, const char *file, int line, const E &e) {
//        if (! expression)
//            e.raise ();
//    }
#ifndef BOOST_UBLAS_NO_STD_CERR
#define BOOST_UBLAS_CHECK_FALSE(e) \
    std::cerr << "Check failed in file " << __FILE__ << " at line " << __LINE__ << ":" << std::endl; \
    e.raise ();
#define BOOST_UBLAS_CHECK(expression, e) \
    if (! (expression)) { \
        std::cerr << "Check failed in file " << __FILE__ << " at line " << __LINE__ << ":" << std::endl; \
        std::cerr << #expression << std::endl; \
        e.raise (); \
    }
#define BOOST_UBLAS_CHECK_EX(expression, file, line, e) \
    if (! (expression)) { \
        std::cerr << "Check failed in file " << (file) << " at line " << (line) << ":" << std::endl; \
        std::cerr << #expression << std::endl; \
        e.raise (); \
    }
#else
#define BOOST_UBLAS_CHECK_FALSE(e) \
    e.raise ();
#define BOOST_UBLAS_CHECK(expression, e) \
    if (! (expression)) { \
        e.raise (); \
    }
#define BOOST_UBLAS_CHECK_EX(expression, file, line, e) \
    if (! (expression)) { \
        e.raise (); \
    }
#endif
#else
// Macros are equivilent to 
//    template<class E>
//    BOOST_UBLAS_INLINE
//    void check (bool expression, const E &e) {}
//    template<class E>
//    BOOST_UBLAS_INLINE
//    void check_ex (bool expression, const char *file, int line, const E &e) {}
#define BOOST_UBLAS_CHECK_FALSE(e)
#define BOOST_UBLAS_CHECK(expression, e)
#define BOOST_UBLAS_CHECK_EX(expression, file, line, e)
#endif


#ifndef BOOST_UBLAS_USE_FAST_SAME
// Macro is equivilent to 
//    template<class T>
//    BOOST_UBLAS_INLINE
//    const T &same_impl (const T &size1, const T &size2) {
//        BOOST_UBLAS_CHECK (size1 == size2, bad_argument ());
//        return (std::min) (size1, size2);
//    }
// #define BOOST_UBLAS_SAME(size1, size2) same_impl ((size1), (size2))
    template<class T>
    BOOST_UBLAS_INLINE
    // Kresimir Fresl and Dan Muller reported problems with COMO.
    // We better change the signature instead of libcomo ;-)
    // const T &same_impl_ex (const T &size1, const T &size2, const char *file, int line) {
    T same_impl_ex (const T &size1, const T &size2, const char *file, int line) {
        BOOST_UBLAS_CHECK_EX (size1 == size2, file, line, bad_argument ());
        return (std::min) (size1, size2);
    }
#define BOOST_UBLAS_SAME(size1, size2) same_impl_ex ((size1), (size2), __FILE__, __LINE__)
#else
// Macros are equivilent to 
//    template<class T>
//    BOOST_UBLAS_INLINE
//    const T &same_impl (const T &size1, const T &size2) {
//        return size1;
//    }
// #define BOOST_UBLAS_SAME(size1, size2) same_impl ((size1), (size2))
#define BOOST_UBLAS_SAME(size1, size2) (size1)
#endif

}}}

#endif
