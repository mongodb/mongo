// Copyright 2002 The Trustees of Indiana University.

// Use, modification and distribution is subject to the Boost Software 
// License, Version 1.0. (See accompanying file LICENSE_1_0.txt or copy at
// http://www.boost.org/LICENSE_1_0.txt)

//  Boost.MultiArray Library
//  Authors: Ronald Garcia
//           Jeremy Siek
//           Andrew Lumsdaine
//  See http://www.boost.org/libs/multi_array for documentation.

#ifndef COLLECTION_CONCEPT_RG103101_HPP
#define COLLECTION_CONCEPT_RG103101_HPP

#include "boost/concept_check.hpp"

namespace boost {
namespace detail {
namespace multi_array {

  //===========================================================================
  // Collection Concept

  template <class Collection>
  struct CollectionConcept
  {
    typedef typename Collection::value_type value_type;
    typedef typename Collection::iterator iterator;
    typedef typename Collection::const_iterator const_iterator;
    typedef typename Collection::reference reference;
    typedef typename Collection::const_reference const_reference;
    // typedef typename Collection::pointer pointer;
    typedef typename Collection::difference_type difference_type;
    typedef typename Collection::size_type size_type;

    void constraints() {
      boost::function_requires<boost::InputIteratorConcept<iterator> >();
      boost::function_requires<boost::InputIteratorConcept<const_iterator> >();
      boost::function_requires<boost::CopyConstructibleConcept<value_type> >();
      const_constraints(c);
      i = c.begin();
      i = c.end();
      c.swap(c);
    }
    void const_constraints(const Collection& c) {
      ci = c.begin();
      ci = c.end();
      n = c.size();
      b = c.empty();
    }
    Collection c;
    bool b;
    iterator i;
    const_iterator ci;
    size_type n;
  };

}
}
}
#endif // COLLECTION_CONCEPT_RG103101_HPP
