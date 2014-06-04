// Copyright David Abrahams 2006. Distributed under the Boost
// Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
#ifndef BOOST_PARAMETER_AUX_PARENTHESIZED_TYPE_DWA2006414_HPP
# define BOOST_PARAMETER_AUX_PARENTHESIZED_TYPE_DWA2006414_HPP

# include <boost/config.hpp>
# include <boost/detail/workaround.hpp>

namespace boost { namespace parameter { namespace aux { 

// A macro that takes a parenthesized C++ type name (T) and transforms
// it into an un-parenthesized type expression equivalent to T.
#  define BOOST_PARAMETER_PARENTHESIZED_TYPE(x)                    \
    boost::parameter::aux::unaryfunptr_arg_type< void(*)x >::type

// A metafunction that transforms void(*)(T) -> T
template <class UnaryFunctionPointer>
struct unaryfunptr_arg_type;

# if !BOOST_WORKAROUND(BOOST_MSVC, <= 1300)

template <class Arg>
struct unaryfunptr_arg_type<void(*)(Arg)>
{
    typedef Arg type;
};

# else

// Use the "native typeof" bugfeatures of older versions of MSVC to
// accomplish what we'd normally do with partial specialization.  This
// capability was discovered by Igor Chesnokov.

#  if BOOST_WORKAROUND(BOOST_MSVC, != 1300)

// This version applies to VC6.5 and VC7.1 (except that we can just
// use partial specialization for the latter in this case).

// This gets used as a base class.  
template<typename Address>
struct msvc_type_memory
{
    // A nullary metafunction that will yield the Value type "stored"
    // at this Address.
    struct storage;
};

template<typename Value, typename Address>
struct msvc_store_type : msvc_type_memory<Address>
{
    // VC++ somehow lets us define the base's nested storage
    // metafunction here, where we have the Value type we'd like to
    // "store" in it.  Later we can come back to the base class and
    // extract the "stored type."
    typedef msvc_type_memory<Address> location;
    struct location::storage 
    {
        typedef Value type;
    };
};

#  else

// This slightly more complicated version of the same thing is
// required for msvc-7.0
template<typename Address>
struct msvc_type_memory
{
    template<bool>
    struct storage_impl;

    typedef storage_impl<true> storage;
};

template<typename Value, typename Address>
struct msvc_store_type : msvc_type_memory<Address>
{
    // Rather than supplying a definition for the base class' nested
    // class, we specialize the base class' nested template
    template<>
    struct storage_impl<true>  
    {
        typedef Value type;
    };
};

#  endif

// Function template argument deduction does many of the same things
// as type matching during partial specialization, so we call a
// function template to "store" T into the type memory addressed by
// void(*)(T).
template <class T>
msvc_store_type<T,void(*)(T)>
msvc_store_argument_type(void(*)(T));

template <class FunctionPointer>
struct unaryfunptr_arg_type
{
    // We don't want the function to be evaluated, just instantiated,
    // so protect it inside of sizeof.
    enum { dummy = sizeof(msvc_store_argument_type((FunctionPointer)0)) };

    // Now pull the type out of the instantiated base class
    typedef typename msvc_type_memory<FunctionPointer>::storage::type type;
};

# endif

template <>
struct unaryfunptr_arg_type<void(*)(void)>
{
    typedef void type;
};
    
}}} // namespace boost::parameter::aux

#endif // BOOST_PARAMETER_AUX_PARENTHESIZED_TYPE_DWA2006414_HPP
