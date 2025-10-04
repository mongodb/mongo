// (C) Copyright David Abrahams 2002.
// (C) Copyright Jeremy Siek    2002.
// (C) Copyright Thomas Witt    2002.
// Distributed under the Boost Software License, Version 1.0. (See
// accompanying file LICENSE_1_0.txt or copy at
// http://www.boost.org/LICENSE_1_0.txt)

// no include guard multiple inclusion intended

//
// This is a temporary workaround until the bulk of this is
// available in boost config.
// 23/02/03 thw
//

#include <boost/config.hpp> // for prior
#include <boost/detail/workaround.hpp>

#ifdef BOOST_ITERATOR_CONFIG_DEF
# error you have nested config_def #inclusion.
#else
# define BOOST_ITERATOR_CONFIG_DEF
#endif

// We enable this always now.  Otherwise, the simple case in
// libs/iterator/test/constant_iterator_arrow.cpp fails to compile
// because the operator-> return is improperly deduced as a non-const
// pointer.

// Recall that in general, compilers without partial specialization
// can't strip constness.  Consider counting_iterator, which normally
// passes a const Value to iterator_facade.  As a result, any code
// which makes a std::vector of the iterator's value_type will fail
// when its allocator declares functions overloaded on reference and
// const_reference (the same type).
//
// Furthermore, Borland 5.5.1 drops constness in enough ways that we
// end up using a proxy for operator[] when we otherwise shouldn't.
// Using reference constness gives it an extra hint that it can
// return the value_type from operator[] directly, but is not
// strictly necessary.  Not sure how best to resolve this one.

# define BOOST_ITERATOR_REF_CONSTNESS_KILLS_WRITABILITY 1

// no include guard; multiple inclusion intended
