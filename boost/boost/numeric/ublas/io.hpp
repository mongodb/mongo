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

#ifndef _BOOST_UBLAS_IO_
#define _BOOST_UBLAS_IO_

// Only forward definition required to define stream operations
#include <iosfwd>
#include <boost/numeric/ublas/matrix_expression.hpp>


namespace boost { namespace numeric { namespace ublas {

    template<class E, class T, class VE>
    // BOOST_UBLAS_INLINE This function seems to be big. So we do not let the compiler inline it.
    std::basic_ostream<E, T> &operator << (std::basic_ostream<E, T> &os,
                                           const vector_expression<VE> &v) {
        typedef typename VE::size_type size_type;
        size_type size = v ().size ();
        std::basic_ostringstream<E, T, std::allocator<E> > s;
        s.flags (os.flags ());
        s.imbue (os.getloc ());
        s.precision (os.precision ());
        s << '[' << size << "](";
        if (size > 0)
            s << v () (0);
        for (size_type i = 1; i < size; ++ i)
            s << ',' << v () (i);
        s << ')';
        return os << s.str ().c_str ();
    }

    template<class E, class T, class VT, class VA>
    // BOOST_UBLAS_INLINE This function seems to be big. So we do not let the compiler inline it.
    std::basic_istream<E, T> &operator >> (std::basic_istream<E, T> &is,
                                           vector<VT, VA> &v) {
        typedef typename vector<VT, VA>::size_type size_type;
        E ch;
        size_type size;
        if (is >> ch && ch != '[') {
            is.putback (ch);
            is.setstate (std::ios_base::failbit);
        } else if (is >> size >> ch && ch != ']') {
            is.putback (ch);
            is.setstate (std::ios_base::failbit);
        } else if (! is.fail ()) {
            vector<VT, VA> s (size);
            if (is >> ch && ch != '(') {
                is.putback (ch);
                is.setstate (std::ios_base::failbit);
            } else if (! is.fail ()) {
                for (size_type i = 0; i < size; i ++) {
                    if (is >> s (i) >> ch && ch != ',') {
                        is.putback (ch);
                        if (i < size - 1)
                            is.setstate (std::ios_base::failbit);
                        break;
                    }
                }
                if (is >> ch && ch != ')') {
                    is.putback (ch);
                    is.setstate (std::ios_base::failbit);
                }
            }
            if (! is.fail ())
                v.swap (s);
        }
        return is;
    }

    template<class E, class T, class ME>
    // BOOST_UBLAS_INLINE This function seems to be big. So we do not let the compiler inline it.
    std::basic_ostream<E, T> &operator << (std::basic_ostream<E, T> &os,
                                           const matrix_expression<ME> &m) {
        typedef typename ME::size_type size_type;
        size_type size1 = m ().size1 ();
        size_type size2 = m ().size2 ();
        std::basic_ostringstream<E, T, std::allocator<E> > s;
        s.flags (os.flags ());
        s.imbue (os.getloc ());
        s.precision (os.precision ());
        s << '[' << size1 << ',' << size2 << "](";
        if (size1 > 0) {
            s << '(' ;
            if (size2 > 0)
                s << m () (0, 0);
            for (size_type j = 1; j < size2; ++ j)
                s << ',' << m () (0, j);
            s << ')';
        }
        for (size_type i = 1; i < size1; ++ i) {
            s << ",(" ;
            if (size2 > 0)
                s << m () (i, 0);
            for (size_type j = 1; j < size2; ++ j)
                s << ',' << m () (i, j);
            s << ')';
        }
        s << ')';
        return os << s.str ().c_str ();
    }

    template<class E, class T, class MT, class MF, class MA>
    // BOOST_UBLAS_INLINE This function seems to be big. So we do not let the compiler inline it.
    std::basic_istream<E, T> &operator >> (std::basic_istream<E, T> &is,
                                           matrix<MT, MF, MA> &m) {
        typedef typename matrix<MT, MF, MA>::size_type size_type;
        E ch;
        size_type size1, size2;
        if (is >> ch && ch != '[') {
            is.putback (ch);
            is.setstate (std::ios_base::failbit);
        } else if (is >> size1 >> ch && ch != ',') {
            is.putback (ch);
            is.setstate (std::ios_base::failbit);
        } else if (is >> size2 >> ch && ch != ']') {
            is.putback (ch);
            is.setstate (std::ios_base::failbit);
        } else if (! is.fail ()) {
            matrix<MT, MF, MA> s (size1, size2);
            if (is >> ch && ch != '(') {
                is.putback (ch);
                is.setstate (std::ios_base::failbit);
            } else if (! is.fail ()) {
                for (size_type i = 0; i < size1; i ++) {
                    if (is >> ch && ch != '(') {
                        is.putback (ch);
                        is.setstate (std::ios_base::failbit);
                        break;
                    }
                    for (size_type j = 0; j < size2; j ++) {
                        if (is >> s (i, j) >> ch && ch != ',') {
                            is.putback (ch);
                            if (j < size2 - 1) {
                                is.setstate (std::ios_base::failbit);
                                break;
                            }
                        }
                    }
                    if (is >> ch && ch != ')') {
                        is.putback (ch);
                        is.setstate (std::ios_base::failbit);
                        break;
                    }
                    if (is >> ch && ch != ',') {
                       is.putback (ch);
                       if (i < size1 - 1) {
                            is.setstate (std::ios_base::failbit);
                            break;
                       }
                    }
                }
                if (is >> ch && ch != ')') {
                    is.putback (ch);
                    is.setstate (std::ios_base::failbit);
                }
            }
            if (! is.fail ())
                m.swap (s);
        }
        return is;
    }

    // Special input operator for symmetrix_matrix
    template<class E, class T, class MT, class MF1, class MF2, class MA>
    // BOOST_UBLAS_INLINE This function seems to be big. So we do not let the compiler inline it.
    std::basic_istream<E, T> &operator >> (std::basic_istream<E, T> &is,
                                           symmetric_matrix<MT, MF1, MF2, MA> &m) {
        typedef typename symmetric_matrix<MT, MF1, MF2, MA>::size_type size_type;
        E ch;
        size_type size1, size2;
        MT value;
        if (is >> ch && ch != '[') {
            is.putback (ch);
            is.setstate (std::ios_base::failbit);
        } else if (is >> size1 >> ch && ch != ',') {
            is.putback (ch);
            is.setstate (std::ios_base::failbit);
        } else if (is >> size2 >> ch && (size2 != size1 || ch != ']')) { // symmetric matrix must be square
            is.putback (ch);
            is.setstate (std::ios_base::failbit);
        } else if (! is.fail ()) {
            symmetric_matrix<MT, MF1, MF2, MA> s (size1, size2);
            if (is >> ch && ch != '(') {
                is.putback (ch);
                is.setstate (std::ios_base::failbit);
             } else if (! is.fail ()) {
                for (size_type i = 0; i < size1; i ++) {
                    if (is >> ch && ch != '(') {
                        is.putback (ch);
                        is.setstate (std::ios_base::failbit);
                        break;
                    }
                    for (size_type j = 0; j < size2; j ++) {
                        if (is >> value >> ch && ch != ',') {
                            is.putback (ch);
                            if (j < size2 - 1) {
                                is.setstate (std::ios_base::failbit);
                                break;
                            }
                        }
                        if (i <= j) { 
                             // this is the first time we read this element - set the value
                            s(i,j) = value;
                        }
                        else if ( s(i,j) != value ) {
                            // matrix is not symmetric
                            is.setstate (std::ios_base::failbit);
                            break;
                        }
                     }
                     if (is >> ch && ch != ')') {
                         is.putback (ch);
                         is.setstate (std::ios_base::failbit);
                         break;
                     }
                     if (is >> ch && ch != ',') {
                        is.putback (ch);
                        if (i < size1 - 1) {
                             is.setstate (std::ios_base::failbit);
                             break;
                        }
                     }
                }
                if (is >> ch && ch != ')') {
                    is.putback (ch);
                    is.setstate (std::ios_base::failbit);
                }
            }
            if (! is.fail ())
                m.swap (s);
        }
        return is;
    }
 

}}}

#endif
