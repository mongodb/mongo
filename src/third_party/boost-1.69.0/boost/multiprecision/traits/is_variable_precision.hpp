///////////////////////////////////////////////////////////////////////////////
//  Copyright 2018 John Maddock. Distributed under the Boost
//  Software License, Version 1.0. (See accompanying file
//  LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)

#ifndef BOOST_MP_IS_VARIABLE_PRECISION_HPP
#define BOOST_MP_IS_VARIABLE_PRECISION_HPP

#include <boost/type_traits/integral_constant.hpp>
#include <boost/multiprecision/detail/number_base.hpp>

namespace boost{ namespace multiprecision{  namespace detail{

   template <class Backend>
   struct is_variable_precision : public false_type{};

   template <class Backend, expression_template_option ExpressionTemplates>
   struct is_variable_precision<number<Backend, ExpressionTemplates> > : public is_variable_precision<Backend> {};

}
}
}

#endif // BOOST_MP_IS_BACKEND_HPP
