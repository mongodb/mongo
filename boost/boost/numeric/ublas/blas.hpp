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

#ifndef _BOOST_UBLAS_BLAS_
#define _BOOST_UBLAS_BLAS_

#include <boost/numeric/ublas/traits.hpp>

namespace boost { namespace numeric { namespace ublas {

    namespace blas_1 {

          /** \namespace boost::numeric::ublas::blas_1
                  \brief wrapper functions for level 1 blas
          */


          /** \brief 1-Norm: \f$\sum_i |x_i|\f$
                  \ingroup blas1
           */
        template<class V>
        typename type_traits<typename V::value_type>::real_type
        asum (const V &v) {
            return norm_1 (v);
        }
          /** \brief 2-Norm: \f$\sum_i |x_i|^2\f$
                  \ingroup blas1
           */
        template<class V>
        typename type_traits<typename V::value_type>::real_type
        nrm2 (const V &v) {
            return norm_2 (v);
        }
          /** \brief element with larges absolute value: \f$\max_i |x_i|\f$
                  \ingroup blas1
          */                 
        template<class V>
        typename type_traits<typename V::value_type>::real_type
        amax (const V &v) {
            return norm_inf (v);
        }

          /** \brief inner product of vectors \a v1 and \a v2
                  \ingroup blas1
          */                 
        template<class V1, class V2>
        typename promote_traits<typename V1::value_type, typename V2::value_type>::promote_type
        dot (const V1 &v1, const V2 &v2) {
            return inner_prod (v1, v2);
        }

          /** \brief copy vector \a v2 to \a v1
                  \ingroup blas1
          */                 
        template<class V1, class V2>
        V1 &
        copy (V1 &v1, const V2 &v2) {
            return v1.assign (v2);
        }

          /** \brief swap vectors \a v1 and \a v2
                  \ingroup blas1
          */                 
        template<class V1, class V2>
        void swap (V1 &v1, V2 &v2) {
            v1.swap (v2);
        }

          /** \brief scale vector \a v with scalar \a t
                  \ingroup blas1
          */                 
        template<class V, class T>
        V &
        scal (V &v, const T &t) {
            return v *= t;
        }

          /** \brief compute \a v1 = \a v1 + \a t * \a v2
                  \ingroup blas1
          */                 
        template<class V1, class T, class V2>
        V1 &
        axpy (V1 &v1, const T &t, const V2 &v2) {
            return v1.plus_assign (t * v2);
        }

          /** \brief apply plane rotation
                  \ingroup blas1
          */                 
        template<class T1, class V1, class T2, class V2>
        void
        rot (const T1 &t1, V1 &v1, const T2 &t2, V2 &v2) {
            typedef typename promote_traits<typename V1::value_type, typename V2::value_type>::promote_type promote_type;
            vector<promote_type> vt (t1 * v1 + t2 * v2);
            v2.assign (- t2 * v1 + t1 * v2);
            v1.assign (vt);
        }

    }

    namespace blas_2 {

          /** \namespace boost::numeric::ublas::blas_2
                  \brief wrapper functions for level 2 blas
          */

          /** \brief multiply vector \a v with triangular matrix \a m
                  \ingroup blas2
                  \todo: check that matrix is really triangular
          */                 
        template<class V, class M>
        V &
        tmv (V &v, const M &m) {
            return v = prod (m, v);
        }

          /** \brief solve \a m \a x = \a v in place, \a m is triangular matrix
                  \ingroup blas2
          */                 
        template<class V, class M, class C>
        V &
        tsv (V &v, const M &m, C) {
            return v = solve (m, v, C ());
        }

          /** \brief compute \a v1 = \a t1 * \a v1 + \a t2 * (\a m * \a v2)
                  \ingroup blas2
          */                 
        template<class V1, class T1, class T2, class M, class V2>
        V1 &
        gmv (V1 &v1, const T1 &t1, const T2 &t2, const M &m, const V2 &v2) {
            return v1 = t1 * v1 + t2 * prod (m, v2);
        }

          /** \brief rank 1 update: \a m = \a m + \a t * (\a v1 * \a v2<sup>T</sup>)
                  \ingroup blas2
          */                 
        template<class M, class T, class V1, class V2>
        M &
        gr (M &m, const T &t, const V1 &v1, const V2 &v2) {
#ifndef BOOST_UBLAS_SIMPLE_ET_DEBUG
            return m += t * outer_prod (v1, v2);
#else
            return m = m + t * outer_prod (v1, v2);
#endif
        }

          /** \brief symmetric rank 1 update: \a m = \a m + \a t * (\a v * \a v<sup>T</sup>)
                  \ingroup blas2
          */                 
        template<class M, class T, class V>
        M &
        sr (M &m, const T &t, const V &v) {
#ifndef BOOST_UBLAS_SIMPLE_ET_DEBUG
            return m += t * outer_prod (v, v);
#else
            return m = m + t * outer_prod (v, v);
#endif
        }
          /** \brief hermitian rank 1 update: \a m = \a m + \a t * (\a v * \a v<sup>H</sup>)
                  \ingroup blas2
          */                 
        template<class M, class T, class V>
        M &
        hr (M &m, const T &t, const V &v) {
#ifndef BOOST_UBLAS_SIMPLE_ET_DEBUG
            return m += t * outer_prod (v, conj (v));
#else
            return m = m + t * outer_prod (v, conj (v));
#endif
        }

          /** \brief symmetric rank 2 update: \a m = \a m + \a t * 
                  (\a v1 * \a v2<sup>T</sup> + \a v2 * \a v1<sup>T</sup>) 
                  \ingroup blas2
          */                 
        template<class M, class T, class V1, class V2>
        M &
        sr2 (M &m, const T &t, const V1 &v1, const V2 &v2) {
#ifndef BOOST_UBLAS_SIMPLE_ET_DEBUG
            return m += t * (outer_prod (v1, v2) + outer_prod (v2, v1));
#else
            return m = m + t * (outer_prod (v1, v2) + outer_prod (v2, v1));
#endif
        }
          /** \brief hermitian rank 2 update: \a m = \a m + 
                  \a t * (\a v1 * \a v2<sup>H</sup>)
                  + \a v2 * (\a t * \a v1)<sup>H</sup>) 
                  \ingroup blas2
          */                 
        template<class M, class T, class V1, class V2>
        M &
        hr2 (M &m, const T &t, const V1 &v1, const V2 &v2) {
#ifndef BOOST_UBLAS_SIMPLE_ET_DEBUG
            return m += t * outer_prod (v1, conj (v2)) + type_traits<T>::conj (t) * outer_prod (v2, conj (v1));
#else
            return m = m + t * outer_prod (v1, conj (v2)) + type_traits<T>::conj (t) * outer_prod (v2, conj (v1));
#endif
        }

    }

    namespace blas_3 {

          /** \namespace boost::numeric::ublas::blas_3
                  \brief wrapper functions for level 3 blas
          */

          /** \brief triangular matrix multiplication
                  \ingroup blas3
          */                 
        template<class M1, class T, class M2, class M3>
        M1 &
        tmm (M1 &m1, const T &t, const M2 &m2, const M3 &m3) {
            return m1 = t * prod (m2, m3);
        }

          /** \brief triangular solve \a m2 * \a x = \a t * \a m1 in place,
                  \a m2 is a triangular matrix
                  \ingroup blas3
          */                 
        template<class M1, class T, class M2, class C>
        M1 &
        tsm (M1 &m1, const T &t, const M2 &m2, C) {
            return m1 = solve (m2, t * m1, C ());
        }

          /** \brief general matrix multiplication
                  \ingroup blas3
          */                 
        template<class M1, class T1, class T2, class M2, class M3>
        M1 &
        gmm (M1 &m1, const T1 &t1, const T2 &t2, const M2 &m2, const M3 &m3) {
            return m1 = t1 * m1 + t2 * prod (m2, m3);
        }

          /** \brief symmetric rank k update: \a m1 = \a t * \a m1 + 
                  \a t2 * (\a m2 * \a m2<sup>T</sup>)
                  \ingroup blas3
                  \todo use opb_prod()
          */                 
        template<class M1, class T1, class T2, class M2>
        M1 &
        srk (M1 &m1, const T1 &t1, const T2 &t2, const M2 &m2) {
            return m1 = t1 * m1 + t2 * prod (m2, trans (m2));
        }
          /** \brief hermitian rank k update: \a m1 = \a t * \a m1 + 
                  \a t2 * (\a m2 * \a m2<sup>H</sup>)
                  \ingroup blas3
                  \todo use opb_prod()
          */                 
        template<class M1, class T1, class T2, class M2>
        M1 &
        hrk (M1 &m1, const T1 &t1, const T2 &t2, const M2 &m2) {
            return m1 = t1 * m1 + t2 * prod (m2, herm (m2));
        }

          /** \brief generalized symmetric rank k update:
                  \a m1 = \a t1 * \a m1 + \a t2 * (\a m2 * \a m3<sup>T</sup>)
                  + \a t2 * (\a m3 * \a m2<sup>T</sup>)
                  \ingroup blas3
                  \todo use opb_prod()
          */                 
        template<class M1, class T1, class T2, class M2, class M3>
        M1 &
        sr2k (M1 &m1, const T1 &t1, const T2 &t2, const M2 &m2, const M3 &m3) {
            return m1 = t1 * m1 + t2 * (prod (m2, trans (m3)) + prod (m3, trans (m2)));
        }
          /** \brief generalized hermitian rank k update:
                  \a m1 = \a t1 * \a m1 + \a t2 * (\a m2 * \a m3<sup>H</sup>)
                  + (\a m3 * (\a t2 * \a m2)<sup>H</sup>)
                  \ingroup blas3
                  \todo use opb_prod()
          */                 
        template<class M1, class T1, class T2, class M2, class M3>
        M1 &
        hr2k (M1 &m1, const T1 &t1, const T2 &t2, const M2 &m2, const M3 &m3) {
            return m1 = t1 * m1 + t2 * prod (m2, herm (m3)) + type_traits<T2>::conj (t2) * prod (m3, herm (m2));
        }

    }

}}}

#endif


