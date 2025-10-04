//  ratio.hpp  ---------------------------------------------------------------//

//  Copyright 2008 Howard Hinnant
//  Copyright 2008 Beman Dawes
//  Copyright 2009 Vicente J. Botet Escriba

//  Distributed under the Boost Software License, Version 1.0.
//  See http://www.boost.org/LICENSE_1_0.txt

/*

This code was derived by Beman Dawes from Howard Hinnant's time2_demo prototype.
Many thanks to Howard for making his code available under the Boost license.
The original code was modified to conform to Boost conventions and to section
20.4 Compile-time rational arithmetic [ratio], of the C++ committee working
paper N2798.
See http://www.open-std.org/jtc1/sc22/wg21/docs/papers/2008/n2798.pdf.

time2_demo contained this comment:

    Much thanks to Andrei Alexandrescu,
                   Walter Brown,
                   Peter Dimov,
                   Jeff Garland,
                   Terry Golubiewski,
                   Daniel Krugler,
                   Anthony Williams.
*/

// The way overflow is managed for ratio_less is taken from llvm/libcxx/include/ratio

#ifndef BOOST_RATIO_RATIO_HPP
#define BOOST_RATIO_RATIO_HPP

#include <boost/ratio/ratio_fwd.hpp>
#include <boost/ratio/detail/gcd_lcm.hpp>

namespace boost
{

// extension used by Chrono

template <class R1, class R2> using ratio_gcd = typename ratio<
    ratio_detail::gcd<R1::num, R2::num>::value,
    ratio_detail::lcm<R1::den, R2::den>::value>::type;

}  // namespace boost

#endif  // BOOST_RATIO_RATIO_HPP
