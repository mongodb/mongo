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

#ifndef _BOOST_UBLAS_TEMPORARY_
#define _BOOST_UBLAS_TEMPORARY_


namespace boost { namespace numeric { namespace ublas {

/// For the creation of temporary vectors in the assignment of proxies
template <class M>
struct vector_temporary_traits {
   typedef typename M::vector_temporary_type type ;
};

/// For the creation of temporary vectors in the assignment of proxies
template <class M>
struct matrix_temporary_traits {
   typedef typename M::matrix_temporary_type type ;
};

} } }

#endif
