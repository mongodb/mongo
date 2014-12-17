// Copyright David Abrahams 2002.
// Distributed under the Boost Software License, Version 1.0. (See
// accompanying file LICENSE_1_0.txt or copy at
// http://www.boost.org/LICENSE_1_0.txt)
#ifndef INDIRECT_TRAITS_DWA2002131_HPP
# define INDIRECT_TRAITS_DWA2002131_HPP
# include <boost/type_traits/is_function.hpp>
# include <boost/type_traits/is_reference.hpp>
# include <boost/type_traits/is_pointer.hpp>
# include <boost/type_traits/is_class.hpp>
# include <boost/type_traits/is_const.hpp>
# include <boost/type_traits/is_volatile.hpp>
# include <boost/type_traits/is_member_function_pointer.hpp>
# include <boost/type_traits/is_member_pointer.hpp>
# include <boost/type_traits/remove_cv.hpp>
# include <boost/type_traits/remove_reference.hpp>
# include <boost/type_traits/remove_pointer.hpp>

# include <boost/type_traits/detail/ice_and.hpp>
# include <boost/detail/workaround.hpp>

# include <boost/mpl/eval_if.hpp>
# include <boost/mpl/if.hpp>
# include <boost/mpl/bool.hpp>
# include <boost/mpl/and.hpp>
# include <boost/mpl/not.hpp>
# include <boost/mpl/aux_/lambda_support.hpp>

#  ifdef BOOST_NO_TEMPLATE_PARTIAL_SPECIALIZATION
#   include <boost/detail/is_function_ref_tester.hpp>
#  endif 

namespace boost { namespace detail {

namespace indirect_traits {

#  ifndef BOOST_NO_TEMPLATE_PARTIAL_SPECIALIZATION
template <class T>
struct is_reference_to_const : mpl::false_
{
};

template <class T>
struct is_reference_to_const<T const&> : mpl::true_
{
};

#   if defined(BOOST_MSVC) && _MSC_FULL_VER <= 13102140 // vc7.01 alpha workaround
template<class T>
struct is_reference_to_const<T const volatile&> : mpl::true_
{
};
#   endif 

template <class T>
struct is_reference_to_function : mpl::false_
{
};

template <class T>
struct is_reference_to_function<T&> : is_function<T>
{
};

template <class T>
struct is_pointer_to_function : mpl::false_
{
};

// There's no such thing as a pointer-to-cv-function, so we don't need
// specializations for those
template <class T>
struct is_pointer_to_function<T*> : is_function<T>
{
};

template <class T>
struct is_reference_to_member_function_pointer_impl : mpl::false_
{
};

template <class T>
struct is_reference_to_member_function_pointer_impl<T&>
    : is_member_function_pointer<typename remove_cv<T>::type>
{
};


template <class T>
struct is_reference_to_member_function_pointer
    : is_reference_to_member_function_pointer_impl<T>
{
    BOOST_MPL_AUX_LAMBDA_SUPPORT(1,is_reference_to_member_function_pointer,(T))
};

template <class T>
struct is_reference_to_function_pointer_aux
    : mpl::and_<
          is_reference<T>
        , is_pointer_to_function<
              typename remove_cv<
                  typename remove_reference<T>::type
              >::type
          >
      >
{
    // There's no such thing as a pointer-to-cv-function, so we don't need specializations for those
};

template <class T>
struct is_reference_to_function_pointer
    : mpl::if_<
          is_reference_to_function<T>
        , mpl::false_
        , is_reference_to_function_pointer_aux<T>
     >::type
{
};

template <class T>
struct is_reference_to_non_const
    : mpl::and_<
          is_reference<T>
        , mpl::not_<
             is_reference_to_const<T>
          >
      >
{
};

template <class T>
struct is_reference_to_volatile : mpl::false_
{
};

template <class T>
struct is_reference_to_volatile<T volatile&> : mpl::true_
{
};

#   if defined(BOOST_MSVC) && _MSC_FULL_VER <= 13102140 // vc7.01 alpha workaround
template <class T>
struct is_reference_to_volatile<T const volatile&> : mpl::true_
{
};
#   endif 


template <class T>
struct is_reference_to_pointer : mpl::false_
{
};

template <class T>
struct is_reference_to_pointer<T*&> : mpl::true_
{
};

template <class T>
struct is_reference_to_pointer<T* const&> : mpl::true_
{
};

template <class T>
struct is_reference_to_pointer<T* volatile&> : mpl::true_
{
};

template <class T>
struct is_reference_to_pointer<T* const volatile&> : mpl::true_
{
};

template <class T>
struct is_reference_to_class
    : mpl::and_<
          is_reference<T>
        , is_class<
              typename remove_cv<
                  typename remove_reference<T>::type
              >::type
          >
      >
{
    BOOST_MPL_AUX_LAMBDA_SUPPORT(1,is_reference_to_class,(T))
};

template <class T>
struct is_pointer_to_class
    : mpl::and_<
          is_pointer<T>
        , is_class<
              typename remove_cv<
                  typename remove_pointer<T>::type
              >::type
          >
      >
{
    BOOST_MPL_AUX_LAMBDA_SUPPORT(1,is_pointer_to_class,(T))
};

#  else

using namespace boost::detail::is_function_ref_tester_;

typedef char (&inner_yes_type)[3];
typedef char (&inner_no_type)[2];
typedef char (&outer_no_type)[1];

template <typename V>
struct is_const_help
{
    typedef typename mpl::if_<
          is_const<V>
        , inner_yes_type
        , inner_no_type
        >::type type;
};

template <typename V>
struct is_volatile_help
{
    typedef typename mpl::if_<
          is_volatile<V>
        , inner_yes_type
        , inner_no_type
        >::type type;
};

template <typename V>
struct is_pointer_help
{
    typedef typename mpl::if_<
          is_pointer<V>
        , inner_yes_type
        , inner_no_type
        >::type type;
};

template <typename V>
struct is_class_help
{
    typedef typename mpl::if_<
          is_class<V>
        , inner_yes_type
        , inner_no_type
        >::type type;
};

template <class T>
struct is_reference_to_function_aux
{
    static T t;
    BOOST_STATIC_CONSTANT(
        bool, value = sizeof(detail::is_function_ref_tester(t,0)) == sizeof(::boost::type_traits::yes_type));
    typedef mpl::bool_<value> type;
 };

template <class T>
struct is_reference_to_function
    : mpl::if_<is_reference<T>, is_reference_to_function_aux<T>, mpl::bool_<false> >::type
{
};

template <class T>
struct is_pointer_to_function_aux
{
    static T t;
    BOOST_STATIC_CONSTANT(
        bool, value
        = sizeof(::boost::type_traits::is_function_ptr_tester(t)) == sizeof(::boost::type_traits::yes_type));
    typedef mpl::bool_<value> type;
};

template <class T>
struct is_pointer_to_function
    : mpl::if_<is_pointer<T>, is_pointer_to_function_aux<T>, mpl::bool_<false> >::type
{
    BOOST_MPL_AUX_LAMBDA_SUPPORT(1,is_pointer_to_function,(T))
};

struct false_helper1
{
    template <class T>
    struct apply : mpl::false_
    {
    };
};

template <typename V>
typename is_const_help<V>::type reference_to_const_helper(V&);    
outer_no_type
reference_to_const_helper(...);

struct true_helper1
{
    template <class T>
    struct apply
    {
        static T t;
        BOOST_STATIC_CONSTANT(
            bool, value
            = sizeof(reference_to_const_helper(t)) == sizeof(inner_yes_type));
        typedef mpl::bool_<value> type;
    };
};

template <bool ref = true>
struct is_reference_to_const_helper1 : true_helper1
{
};

template <>
struct is_reference_to_const_helper1<false> : false_helper1
{
};


template <class T>
struct is_reference_to_const
    : is_reference_to_const_helper1<is_reference<T>::value>::template apply<T>
{
};


template <bool ref = true>
struct is_reference_to_non_const_helper1
{
    template <class T>
    struct apply
    {
        static T t;
        BOOST_STATIC_CONSTANT(
            bool, value
            = sizeof(reference_to_const_helper(t)) == sizeof(inner_no_type));
        
        typedef mpl::bool_<value> type;
    };
};

template <>
struct is_reference_to_non_const_helper1<false> : false_helper1
{
};


template <class T>
struct is_reference_to_non_const
    : is_reference_to_non_const_helper1<is_reference<T>::value>::template apply<T>
{
    BOOST_MPL_AUX_LAMBDA_SUPPORT(1,is_reference_to_non_const,(T))
};


template <typename V>
typename is_volatile_help<V>::type reference_to_volatile_helper(V&);    
outer_no_type
reference_to_volatile_helper(...);

template <bool ref = true>
struct is_reference_to_volatile_helper1
{
    template <class T>
    struct apply
    {
        static T t;
        BOOST_STATIC_CONSTANT(
            bool, value
            = sizeof(reference_to_volatile_helper(t)) == sizeof(inner_yes_type));
        typedef mpl::bool_<value> type;
    };
};

template <>
struct is_reference_to_volatile_helper1<false> : false_helper1
{
};


template <class T>
struct is_reference_to_volatile
    : is_reference_to_volatile_helper1<is_reference<T>::value>::template apply<T>
{
};

template <typename V>
typename is_pointer_help<V>::type reference_to_pointer_helper(V&);
outer_no_type reference_to_pointer_helper(...);

template <class T>
struct reference_to_pointer_impl
{
    static T t;
    BOOST_STATIC_CONSTANT(
        bool, value
        = (sizeof((reference_to_pointer_helper)(t)) == sizeof(inner_yes_type))
        );
    
    typedef mpl::bool_<value> type;
};
    
template <class T>
struct is_reference_to_pointer
  : mpl::eval_if<is_reference<T>, reference_to_pointer_impl<T>, mpl::false_>::type
{   
    BOOST_MPL_AUX_LAMBDA_SUPPORT(1,is_reference_to_pointer,(T))
};

template <class T>
struct is_reference_to_function_pointer
  : mpl::eval_if<is_reference<T>, is_pointer_to_function_aux<T>, mpl::false_>::type
{
    BOOST_MPL_AUX_LAMBDA_SUPPORT(1,is_reference_to_function_pointer,(T))
};


template <class T>
struct is_member_function_pointer_help
    : mpl::if_<is_member_function_pointer<T>, inner_yes_type, inner_no_type>
{};

template <typename V>
typename is_member_function_pointer_help<V>::type member_function_pointer_helper(V&);
outer_no_type member_function_pointer_helper(...);

template <class T>
struct is_pointer_to_member_function_aux
{
    static T t;
    BOOST_STATIC_CONSTANT(
        bool, value
        = sizeof((member_function_pointer_helper)(t)) == sizeof(inner_yes_type));
    typedef mpl::bool_<value> type;
};

template <class T>
struct is_reference_to_member_function_pointer
    : mpl::if_<
        is_reference<T>
        , is_pointer_to_member_function_aux<T>
        , mpl::bool_<false>
     >::type
{
    BOOST_MPL_AUX_LAMBDA_SUPPORT(1,is_reference_to_member_function_pointer,(T))
};

template <typename V>
typename is_class_help<V>::type reference_to_class_helper(V const volatile&);
outer_no_type reference_to_class_helper(...);

template <class T>
struct is_reference_to_class
{
    static T t;
    BOOST_STATIC_CONSTANT(
        bool, value
        = (is_reference<T>::value
           & (sizeof(reference_to_class_helper(t)) == sizeof(inner_yes_type)))
        );
    typedef mpl::bool_<value> type;
    BOOST_MPL_AUX_LAMBDA_SUPPORT(1,is_reference_to_class,(T))
};

template <typename V>
typename is_class_help<V>::type pointer_to_class_helper(V const volatile*);
outer_no_type pointer_to_class_helper(...);

template <class T>
struct is_pointer_to_class
{
    static T t;
    BOOST_STATIC_CONSTANT(
        bool, value
        = (is_pointer<T>::value
           && sizeof(pointer_to_class_helper(t)) == sizeof(inner_yes_type))
        );
    typedef mpl::bool_<value> type;
};
#  endif // BOOST_NO_TEMPLATE_PARTIAL_SPECIALIZATION 

}

using namespace indirect_traits;

}} // namespace boost::python::detail

#endif // INDIRECT_TRAITS_DWA2002131_HPP
