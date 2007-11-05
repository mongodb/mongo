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

#ifndef _BOOST_UBLAS_TRAITS_
#define _BOOST_UBLAS_TRAITS_

#include <iterator>
#include <complex>
#include <cmath>

#include <boost/numeric/ublas/detail/config.hpp>
#include <boost/numeric/ublas/detail/iterator.hpp>
#include <boost/numeric/ublas/detail/returntype_deduction.hpp>


namespace boost { namespace numeric { namespace ublas {

    // Use Joel de Guzman's return type deduction
    // uBLAS assumes a common return type for all binary arithmetic operators
    template<class X, class Y>
    struct promote_traits {
        typedef type_deduction_detail::base_result_of<X, Y> base_type;
        static typename base_type::x_type x;
        static typename base_type::y_type y;
        static const std::size_t size = sizeof (
                type_deduction_detail::test<
                    typename base_type::x_type
                  , typename base_type::y_type
                >(x + y)     // Use x+y to stand of all the arithmetic actions
            );

        static const std::size_t index = (size / sizeof (char)) - 1;
        typedef typename mpl::at_c<
            typename base_type::types, index>::type id;
        typedef typename id::type promote_type;
    };


        // Type traits - generic numeric properties and functions
    template<class T>
    struct type_traits;
        
    // Define properties for a generic scalar type
    template<class T>
    struct scalar_traits {
        typedef scalar_traits<T> self_type;
        typedef T value_type;
        typedef const T &const_reference;
        typedef T &reference;

        typedef T real_type;
        typedef real_type precision_type;       // we do not know what type has more precision then the real_type

        static const unsigned plus_complexity = 1;
        static const unsigned multiplies_complexity = 1;

        static
        BOOST_UBLAS_INLINE
        real_type real (const_reference t) {
                return t;
        }
        static
        BOOST_UBLAS_INLINE
        real_type imag (const_reference /*t*/) {
                return 0;
        }
        static
        BOOST_UBLAS_INLINE
        value_type conj (const_reference t) {
                return t;
        }

        static
        BOOST_UBLAS_INLINE
        real_type type_abs (const_reference t) {
            return std::abs (t);    // must use explict std:: as bultin types are not in std namespace
        }
        static
        BOOST_UBLAS_INLINE
        value_type type_sqrt (const_reference t) {
               // force a type conversion back to value_type for intgral types
            return value_type (std::sqrt (t));   // must use explict std:: as bultin types are not in std namespace
        }

        static
        BOOST_UBLAS_INLINE
        real_type norm_1 (const_reference t) {
            return self_type::type_abs (t);
        }
        static
        BOOST_UBLAS_INLINE
        real_type norm_2 (const_reference t) {
            return self_type::type_abs (t);
        }
        static
        BOOST_UBLAS_INLINE
        real_type norm_inf (const_reference t) {
            return self_type::type_abs (t);
        }

        static
        BOOST_UBLAS_INLINE
        bool equals (const_reference t1, const_reference t2) {
            return self_type::norm_inf (t1 - t2) < BOOST_UBLAS_TYPE_CHECK_EPSILON *
                   (std::max) ((std::max) (self_type::norm_inf (t1),
                                       self_type::norm_inf (t2)),
                             BOOST_UBLAS_TYPE_CHECK_MIN);
        }
    };

    // Define default type traits, assume T is a scalar type
    template<class T>
    struct type_traits : scalar_traits <T> {
        typedef type_traits<T> self_type;
        typedef T value_type;
        typedef const T &const_reference;
        typedef T &reference;

        typedef T real_type;
        typedef real_type precision_type;
        static const unsigned multiplies_complexity = 1;

    };

    // Define real type traits
    template<>
    struct type_traits<float> : scalar_traits<float> {
        typedef type_traits<float> self_type;
        typedef float value_type;
        typedef const value_type &const_reference;
        typedef value_type &reference;
        typedef value_type real_type;
        typedef double precision_type;
    };
    template<>
    struct type_traits<double> : scalar_traits<double> {
        typedef type_traits<double> self_type;
        typedef double value_type;
        typedef const value_type &const_reference;
        typedef value_type &reference;
        typedef value_type real_type;
        typedef long double precision_type;
    };
    template<>
    struct type_traits<long double>  : scalar_traits<long double> {
        typedef type_traits<long double> self_type;
        typedef long double value_type;
        typedef const value_type &const_reference;
        typedef value_type &reference;
        typedef value_type real_type;
        typedef value_type precision_type;
    };

    // Define properties for a generic complex type
    template<class T>
    struct complex_traits {
        typedef complex_traits<T> self_type;
        typedef T value_type;
        typedef const T &const_reference;
        typedef T &reference;

        typedef typename T::value_type real_type;
        typedef real_type precision_type;       // we do not know what type has more precision then the real_type

        static const unsigned plus_complexity = 2;
        static const unsigned multiplies_complexity = 6;

        static
        BOOST_UBLAS_INLINE
        real_type real (const_reference t) {
                return std::real (t);
        }
        static
        BOOST_UBLAS_INLINE
        real_type imag (const_reference t) {
                return std::imag (t);
        }
        static
        BOOST_UBLAS_INLINE
        value_type conj (const_reference t) {
                return std::conj (t);
        }

        static
        BOOST_UBLAS_INLINE
        real_type type_abs (const_reference t) {
                return abs (t);
        }
        static
        BOOST_UBLAS_INLINE
        value_type type_sqrt (const_reference t) {
                return sqrt (t);
        }

        static
        BOOST_UBLAS_INLINE
        real_type norm_1 (const_reference t) {
            return type_traits<real_type>::type_abs (self_type::real (t)) +
                   type_traits<real_type>::type_abs (self_type::imag (t));
        }
        static
        BOOST_UBLAS_INLINE
        real_type norm_2 (const_reference t) {
            return self_type::type_abs (t);
        }
        static
        BOOST_UBLAS_INLINE
        real_type norm_inf (const_reference t) {
            return (std::max) (type_traits<real_type>::type_abs (self_type::real (t)),
                             type_traits<real_type>::type_abs (self_type::imag (t)));
        }

        static
        BOOST_UBLAS_INLINE
        bool equals (const_reference t1, const_reference t2) {
            return self_type::norm_inf (t1 - t2) < BOOST_UBLAS_TYPE_CHECK_EPSILON *
                   (std::max) ((std::max) (self_type::norm_inf (t1),
                                       self_type::norm_inf (t2)),
                             BOOST_UBLAS_TYPE_CHECK_MIN);
        }
    };
    
    // Define complex type traits
    template<>
    struct type_traits<std::complex<float> > : complex_traits<std::complex<float> >{
        typedef type_traits<std::complex<float> > self_type;
        typedef std::complex<float> value_type;
        typedef const value_type &const_reference;
        typedef value_type &reference;
        typedef float real_type;
        typedef std::complex<double> precision_type;

    };
    template<>
    struct type_traits<std::complex<double> > : complex_traits<std::complex<double> >{
        typedef type_traits<std::complex<double> > self_type;
        typedef std::complex<double> value_type;
        typedef const value_type &const_reference;
        typedef value_type &reference;
        typedef double real_type;
        typedef std::complex<long double> precision_type;
    };
    template<>
    struct type_traits<std::complex<long double> > : complex_traits<std::complex<long double> > {
        typedef type_traits<std::complex<long double> > self_type;
        typedef std::complex<long double> value_type;
        typedef const value_type &const_reference;
        typedef value_type &reference;
        typedef long double real_type;
        typedef value_type precision_type;
    };

#ifdef BOOST_UBLAS_USE_INTERVAL
    // Define properties for a generic scalar interval type
    template<class T>
    struct scalar_interval_type_traits : scalar_type_traits<T> {
        typedef scalar_interval_type_traits<T> self_type;
        typedef boost::numeric::interval<float> value_type;
        typedef const value_type &const_reference;
        typedef value_type &reference;
        typedef value_type real_type;
        typedef real_type precision_type;       // we do not know what type has more precision then the real_type

        static const unsigned plus_complexity = 1;
        static const unsigned multiplies_complexity = 1;

        static
        BOOST_UBLAS_INLINE
        real_type type_abs (const_reference t) {
            return abs (t);
        }
        static
        BOOST_UBLAS_INLINE
        value_type type_sqrt (const_reference t) {
            return sqrt (t);
        }

        static
        BOOST_UBLAS_INLINE
        real_type norm_1 (const_reference t) {
            return self_type::type_abs (t);
        }
        static
        BOOST_UBLAS_INLINE
        real_type norm_2 (const_reference t) {
            return self_type::type_abs (t);
        }
        static
        BOOST_UBLAS_INLINE
        real_type norm_inf (const_reference t) {
            return self_type::type_abs (t);
        }

        static
        BOOST_UBLAS_INLINE
        bool equals (const_reference t1, const_reference t2) {
            return self_type::norm_inf (t1 - t2) < BOOST_UBLAS_TYPE_CHECK_EPSILON *
                   (std::max) ((std::max) (self_type::norm_inf (t1),
                                       self_type::norm_inf (t2)),
                             BOOST_UBLAS_TYPE_CHECK_MIN);
        }
    };

    // Define scalar interval type traits
    template<>
    struct type_traits<boost::numeric::interval<float> > : scalar_interval_type_traits<boost::numeric::interval<float> > {
        typedef type_traits<boost::numeric::interval<float> > self_type;
        typedef boost::numeric::interval<float> value_type;
        typedef const value_type &const_reference;
        typedef value_type &reference;
        typedef value_type real_type;
        typedef boost::numeric::interval<double> precision_type;

    };
    template<>
    struct type_traits<boost::numeric::interval<double> > : scalar_interval_type_traits<boost::numeric::interval<double> > {
        typedef type_traits<boost::numeric::interval<double> > self_type;
        typedef boost::numeric::interval<double> value_type;
        typedef const value_type &const_reference;
        typedef value_type &reference;
        typedef value_type real_type;
        typedef boost::numeric::interval<long double> precision_type;
    };
    template<>
    struct type_traits<boost::numeric::interval<long double> > : scalar_interval_type_traits<boost::numeric::interval<long double> > {
        typedef type_traits<boost::numeric::interval<long double> > self_type;
        typedef boost::numeric::interval<long double> value_type;
        typedef const value_type &const_reference;
        typedef value_type &reference;
        typedef value_type real_type;
        typedef value_type precision_type;
    };

#endif


    // Storage tags -- hierarchical definition of storage characteristics

    struct unknown_storage_tag {};
    struct sparse_proxy_tag: public unknown_storage_tag {};
    struct sparse_tag: public sparse_proxy_tag {};
    struct packed_proxy_tag: public sparse_proxy_tag {};
    struct packed_tag: public packed_proxy_tag {};
    struct dense_proxy_tag: public packed_proxy_tag {};
    struct dense_tag: public dense_proxy_tag {};

    template<class S1, class S2>
    struct storage_restrict_traits {
        typedef S1 storage_category;
    };

    template<>
    struct storage_restrict_traits<sparse_tag, dense_proxy_tag> {
        typedef sparse_proxy_tag storage_category;
    };
    template<>
    struct storage_restrict_traits<sparse_tag, packed_proxy_tag> {
        typedef sparse_proxy_tag storage_category;
    };
    template<>
    struct storage_restrict_traits<sparse_tag, sparse_proxy_tag> {
        typedef sparse_proxy_tag storage_category;
    };

    template<>
    struct storage_restrict_traits<packed_tag, dense_proxy_tag> {
        typedef packed_proxy_tag storage_category;
    };
    template<>
    struct storage_restrict_traits<packed_tag, packed_proxy_tag> {
        typedef packed_proxy_tag storage_category;
    };
    template<>
    struct storage_restrict_traits<packed_tag, sparse_proxy_tag> {
        typedef sparse_proxy_tag storage_category;
    };

    template<>
    struct storage_restrict_traits<packed_proxy_tag, sparse_proxy_tag> {
        typedef sparse_proxy_tag storage_category;
    };

    template<>
    struct storage_restrict_traits<dense_tag, dense_proxy_tag> {
        typedef dense_proxy_tag storage_category;
    };
    template<>
    struct storage_restrict_traits<dense_tag, packed_proxy_tag> {
        typedef packed_proxy_tag storage_category;
    };
    template<>
    struct storage_restrict_traits<dense_tag, sparse_proxy_tag> {
        typedef sparse_proxy_tag storage_category;
    };

    template<>
    struct storage_restrict_traits<dense_proxy_tag, packed_proxy_tag> {
        typedef packed_proxy_tag storage_category;
    };
    template<>
    struct storage_restrict_traits<dense_proxy_tag, sparse_proxy_tag> {
        typedef sparse_proxy_tag storage_category;
    };


    // Iterator tags -- hierarchical definition of storage characteristics

    struct sparse_bidirectional_iterator_tag : public std::bidirectional_iterator_tag {};
    struct packed_random_access_iterator_tag : public std::random_access_iterator_tag {};
    struct dense_random_access_iterator_tag : public packed_random_access_iterator_tag {};

    // Thanks to Kresimir Fresl for convincing Comeau with iterator_base_traits ;-)
    template<class IC>
    struct iterator_base_traits {};

    template<>
    struct iterator_base_traits<std::forward_iterator_tag> {
        template<class I, class T>
        struct iterator_base {
            typedef forward_iterator_base<std::forward_iterator_tag, I, T> type;
        };
    };

    template<>
    struct iterator_base_traits<std::bidirectional_iterator_tag> {
        template<class I, class T>
        struct iterator_base {
            typedef bidirectional_iterator_base<std::bidirectional_iterator_tag, I, T> type;
        };
    };
    template<>
    struct iterator_base_traits<sparse_bidirectional_iterator_tag> {
        template<class I, class T>
        struct iterator_base {
            typedef bidirectional_iterator_base<sparse_bidirectional_iterator_tag, I, T> type;
        };
    };

    template<>
    struct iterator_base_traits<std::random_access_iterator_tag> {
        template<class I, class T>
        struct iterator_base {
            typedef random_access_iterator_base<std::random_access_iterator_tag, I, T> type;
        };
    };
    template<>
    struct iterator_base_traits<packed_random_access_iterator_tag> {
        template<class I, class T>
        struct iterator_base {
            typedef random_access_iterator_base<packed_random_access_iterator_tag, I, T> type;
        };
    };
    template<>
    struct iterator_base_traits<dense_random_access_iterator_tag> {
        template<class I, class T>
        struct iterator_base {
            typedef random_access_iterator_base<dense_random_access_iterator_tag, I, T> type;
        };
    };

    template<class I1, class I2>
    struct iterator_restrict_traits {
        typedef I1 iterator_category;
    };

    template<>
    struct iterator_restrict_traits<packed_random_access_iterator_tag, sparse_bidirectional_iterator_tag> {
        typedef sparse_bidirectional_iterator_tag iterator_category;
    };
    template<>
    struct iterator_restrict_traits<sparse_bidirectional_iterator_tag, packed_random_access_iterator_tag> {
        typedef sparse_bidirectional_iterator_tag iterator_category;
    };

    template<>
    struct iterator_restrict_traits<dense_random_access_iterator_tag, sparse_bidirectional_iterator_tag> {
        typedef sparse_bidirectional_iterator_tag iterator_category;
    };
    template<>
    struct iterator_restrict_traits<sparse_bidirectional_iterator_tag, dense_random_access_iterator_tag> {
        typedef sparse_bidirectional_iterator_tag iterator_category;
    };

    template<>
    struct iterator_restrict_traits<dense_random_access_iterator_tag, packed_random_access_iterator_tag> {
        typedef packed_random_access_iterator_tag iterator_category;
    };
    template<>
    struct iterator_restrict_traits<packed_random_access_iterator_tag, dense_random_access_iterator_tag> {
        typedef packed_random_access_iterator_tag iterator_category;
    };

    template<class I>
    BOOST_UBLAS_INLINE
    void increment (I &it, const I &it_end, typename I::difference_type compare, packed_random_access_iterator_tag) {
        it += (std::min) (compare, it_end - it);
    }
    template<class I>
    BOOST_UBLAS_INLINE
    void increment (I &it, const I &/* it_end */, typename I::difference_type /* compare */, sparse_bidirectional_iterator_tag) {
        ++ it;
    }
    template<class I>
    BOOST_UBLAS_INLINE
    void increment (I &it, const I &it_end, typename I::difference_type compare) {
        increment (it, it_end, compare, typename I::iterator_category ());
    }

    template<class I>
    BOOST_UBLAS_INLINE
    void increment (I &it, const I &it_end) {
#if BOOST_UBLAS_TYPE_CHECK
        I cit (it);
        while (cit != it_end) {
            BOOST_UBLAS_CHECK (*cit == typename I::value_type/*zero*/(), internal_logic ());
            ++ cit;
        }
#endif
        it = it_end;
    }

}}}

#endif
