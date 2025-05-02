// Copyright David Abrahams 2003. Use, modification and distribution is
// subject to the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
#ifndef BOOST_ITERATOR_MINIMUM_CATEGORY_HPP_INCLUDED_
#define BOOST_ITERATOR_MINIMUM_CATEGORY_HPP_INCLUDED_

#include <boost/mpl/arg_fwd.hpp>
#include <boost/iterator/min_category.hpp>

namespace boost {
namespace iterators {

// Deprecated metafunction for selecting minimum iterator category,
// use min_category instead.
template< class T1 = mpl::arg<1>, class T2 = mpl::arg<2> >
struct minimum_category :
    public min_category<T1, T2>
{
};

template <>
struct minimum_category< mpl::arg<1>, mpl::arg<2> >
{
    template <class T1, class T2>
    struct apply :
        public min_category<T1, T2>
    {};
};

} // namespace iterators
} // namespace boost

#endif // BOOST_ITERATOR_MINIMUM_CATEGORY_HPP_INCLUDED_
