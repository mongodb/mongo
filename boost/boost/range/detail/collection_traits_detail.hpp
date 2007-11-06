//  Boost string_algo library collection_traits.hpp header file  -----------------------//

//  Copyright Pavol Droba 2002-2003. Use, modification and
//  distribution is subject to the Boost Software License, Version
//  1.0. (See accompanying file LICENSE_1_0.txt or copy at
//  http://www.boost.org/LICENSE_1_0.txt)

//  See http://www.boost.org for updates, documentation, and revision history.

#ifndef BOOST_RANGE_STRING_DETAIL_COLLECTION_TRAITS_HPP
#define BOOST_RANGE_STRING_DETAIL_COLLECTION_TRAITS_HPP

#include <boost/algorithm/string/config.hpp>
#include <cstddef>
#include <string>
#include <boost/type_traits/is_array.hpp>
#include <boost/type_traits/is_pointer.hpp>
#include <boost/type_traits/is_const.hpp>
#include <boost/type_traits/is_convertible.hpp>
#include <boost/type_traits/remove_pointer.hpp>
#include <boost/type_traits/remove_cv.hpp>
#include <boost/mpl/eval_if.hpp>
#include <boost/mpl/identity.hpp>
#include <boost/mpl/vector.hpp>
#include <boost/mpl/fold.hpp>
#include <boost/detail/iterator.hpp>
#include <boost/algorithm/string/yes_no_type.hpp>

// Container traits implementation ---------------------------------------------------------

namespace boost {
    namespace algorithm {
        namespace detail {

// Default collection traits -----------------------------------------------------------------

            // Default collection helper 
            /*
                Wraps std::container compliant containers
            */
            template< typename ContainerT >     
            struct default_container_traits
            {
                typedef BOOST_STRING_TYPENAME ContainerT::value_type value_type;
                typedef BOOST_STRING_TYPENAME ContainerT::iterator iterator;
                typedef BOOST_STRING_TYPENAME ContainerT::const_iterator const_iterator;
                typedef BOOST_STRING_TYPENAME 
                    ::boost::mpl::if_< ::boost::is_const<ContainerT>,
                        const_iterator,
                        iterator 
                    >::type result_iterator;
                typedef BOOST_STRING_TYPENAME ContainerT::difference_type difference_type;
                typedef BOOST_STRING_TYPENAME ContainerT::size_type size_type;
                
                // static operations
                template< typename C >
                static size_type size( const C& c )
                {
                    return c.size();
                }

                template< typename C >
                static bool empty( const C& c )
                {
                    return c.empty();
                }

#ifndef BOOST_NO_FUNCTION_TEMPLATE_ORDERING

                template< typename C >
                static iterator begin( C& c )
                {
                    return c.begin();
                }

                template< typename C >
                static const_iterator begin( const C& c )
                {
                    return c.begin();
                }

                template< typename C >
                static iterator end( C& c )
                {
                    return c.end();
                }

                template< typename C >
                static const_iterator end( const C& c )
                {
                    return c.end();
                }

#else // BOOST_NO_FUNCTION_TEMPLATE_ORDERING

                template< typename C >
                static result_iterator begin( C& c )
                {
                    return c.begin();
                }

                template< typename C >
                static result_iterator end( C& c )
                {
                    return c.end();
                }

#endif // BOOST_NO_FUNCTION_TEMPLATE_ORDERING    

            }; 

            template<typename T>
            struct default_container_traits_selector
            {
                typedef default_container_traits<T> type;
            };

// Pair container traits ---------------------------------------------------------------------
                    
            // pair selector
            template< typename T, typename U >
            yes_type is_pair_impl( const std::pair<T,U>* );
            no_type is_pair_impl( ... );

            template<typename T> struct is_pair
            {
            private:
                static T* t;
            public:
                BOOST_STATIC_CONSTANT( bool, value=
                    sizeof(is_pair_impl(t))==sizeof(yes_type) );
            };

            // pair helper
            template< typename PairT >
            struct pair_container_traits
            {
                typedef BOOST_STRING_TYPENAME PairT::first_type element_type;

                typedef BOOST_STRING_TYPENAME ::boost::detail::
                    iterator_traits<element_type>::value_type value_type;
                typedef std::size_t size_type;
                typedef BOOST_STRING_TYPENAME ::boost::detail::
                    iterator_traits<element_type>::difference_type difference_type;

                typedef element_type iterator;
                typedef element_type const_iterator;
                typedef element_type result_iterator;

                // static operations
                template< typename P >
                static size_type size( const P& p )
                {
                    difference_type diff = std::distance( p.first, p.second );
                    if ( diff < 0 ) 
                        return 0;
                    else
                        return diff;
                }

                template< typename P >
                static bool empty( const P& p )
                {
                    return p.first==p.second;
                }

                template< typename P > 
                static const_iterator begin( const P& p )
                {
                    return p.first;
                }

                template< typename P >
                static const_iterator end( const P& p )
                {
                    return p.second;
                }
            }; // 'pair_container_helper'

            template<typename T>
            struct pair_container_traits_selector
            {
                typedef pair_container_traits<T> type;
            };

// Array container traits ---------------------------------------------------------------

#ifndef BOOST_NO_TEMPLATE_PARTIAL_SPECIALIZATION
            // array traits ( partial specialization )
            template< typename T >
            struct array_traits;

            template< typename T, std::size_t sz >
            struct array_traits<T[sz]>
            {
                // typedef
                typedef T* iterator;
                typedef const T* const_iterator;
                typedef T value_type;
                typedef std::size_t size_type;
                typedef std::ptrdiff_t difference_type;

                // size of the array ( static );
                BOOST_STATIC_CONSTANT( size_type, array_size = sz );
            };

#else // BOOST_NO_TEMPLATE_PARTIAL_SPECIALIZATION

            // array traits ( no partial specialization )
            /*
                without parial specialization we are able to
                provide support only for a limited number of
                types. Currently the primitive numeric types 
                are supported
            */
            template< typename T, typename BaseT >
            struct array_traits_impl
            {
                typedef BaseT value_type;
                typedef BaseT* iterator;
                typedef const BaseT* const_iterator;
                typedef std::size_t size_type;
                typedef std::ptrdiff_t difference_type;

                // size of the array
                BOOST_STATIC_CONSTANT( size_type, array_size = sizeof(T)/sizeof(BaseT) );
            };
            
            template< typename T, typename BaseT >
            struct array_traits_impl_selector
            {
                typedef array_traits_impl<T,BaseT> type;
            };

            struct array_traits_void
            {
                typedef void type;
            };

            template< typename T, typename BaseT >
            struct array_traits_cv_selector
            {
                typedef BOOST_STRING_TYPENAME 
                    ::boost::mpl::eval_if< 
                        ::boost::is_convertible<T,BaseT*>,
                        array_traits_impl_selector<T,BaseT>,
                        ::boost::mpl::eval_if< 
                            ::boost::is_convertible<T,const BaseT*>,
                                array_traits_impl_selector<T, const BaseT>,
                                ::boost::mpl::eval_if< 
                                    ::boost::is_convertible<T, volatile BaseT*>,
                                    array_traits_impl_selector<T, volatile BaseT>,
                                    array_traits_impl_selector<T, const volatile BaseT>
                                >
                            >
                    >::type type;
            };

            template< typename T >
            struct array_traits_select
            {
                template< typename T1, typename T2 >
                struct apply
                {
                    typedef BOOST_STRING_TYPENAME
                        ::boost::mpl::eval_if< 
                            ::boost::is_convertible<T,const volatile T2*>,
                            array_traits_cv_selector<T,T2>,
                            ::boost::mpl::identity<T1> >::type type;
                };
            };

            template< typename T >
            struct array_traits_selector 
            {
            private:
                // supported array base types
#ifndef BOOST_NO_INTRINSIC_WCHAR_T
                typedef BOOST_STRING_TYPENAME
                    ::boost::mpl::vector10<
                        wchar_t,
#else // BOOST_NO_INTRINSIC_WCHAR_T
                typedef BOOST_STRING_TYPENAME
                    ::boost::mpl::vector9<
#endif // BOOST_NO_INTRINSIC_WCHAR_T
                        char,
                        signed char,
                        unsigned char,
                        signed short,
                        unsigned short,
                        signed int,
                        unsigned int,
                        signed long,
                        unsigned long
                >::type array_base_types;

            public:
                typedef BOOST_STRING_TYPENAME
                    ::boost::mpl::fold<
                        array_base_types,
                        ::boost::algorithm::detail::array_traits_void,
                        ::boost::algorithm::detail::array_traits_select<T> >::type type;
            };

            template< typename T >
            struct array_traits
            {
                typedef BOOST_STRING_TYPENAME
                    array_traits_selector<T>::type traits_type;

                typedef BOOST_STRING_TYPENAME
                    traits_type::value_type value_type;
                typedef BOOST_STRING_TYPENAME
                    traits_type::iterator iterator;
                typedef BOOST_STRING_TYPENAME
                    traits_type::const_iterator const_iterator;
                typedef BOOST_STRING_TYPENAME
                    traits_type::size_type size_type;
                typedef BOOST_STRING_TYPENAME
                    traits_type::difference_type difference_type;

                BOOST_STATIC_CONSTANT( size_type, array_size = traits_type::array_size );
            };

#endif // BOOST_NO_TEMPLATE_PARTIAL_SPECIALIZATION
            
            // array lenght resolving
            /*
                Lenght of string contained in a static array could
                be different from the size of the array.
                For string processing we need the lenght without
                terminating 0.

                Therefore, the lenght is calulated for char and wchar_t
                using char_traits, rather then simply returning
                the array size.
            */
            template< typename T >
            struct array_length_selector
            {
                template< typename TraitsT >
                struct array_length
                {
                    typedef BOOST_STRING_TYPENAME 
                        TraitsT::size_type size_type;

                    BOOST_STATIC_CONSTANT(
                        size_type,
                        array_size=TraitsT::array_size );

                    template< typename A >
                    static size_type length( const A& )
                    {
                        return array_size;
                    }

                    template< typename A >
                    static bool empty( const A& )
                    {
                        return array_size==0;
                    }
                };
            };

            // specialization for char
            template<>
            struct array_length_selector<char>
            {
                template< typename TraitsT >
                struct array_length
                {
                    typedef BOOST_STRING_TYPENAME 
                        TraitsT::size_type size_type;

                    template< typename A >
                    static size_type length( const A& a )
                    {
                        if ( a==0 ) 
                            return 0;
                        else
                            return std::char_traits<char>::length(a);
                    }
                    
                    template< typename A >
                    static bool empty( const A& a )
                    {
                        return a==0 || a[0]==0;
                    }
                };
            };

            // specialization for wchar_t
            template<>
            struct array_length_selector<wchar_t>
            {
                template< typename TraitsT >
                struct array_length
                {
                    typedef BOOST_STRING_TYPENAME 
                        TraitsT::size_type size_type;

                    template< typename A >
                    static size_type length( const A& a )
                    {
                        if ( a==0 ) 
                            return 0;
                        else
                            return std::char_traits<wchar_t>::length(a);
                    }

                    template< typename A >
                    static bool empty( const A& a )
                    {
                        return a==0 || a[0]==0;
                    }
                };
            };

            template< typename T >
            struct array_container_traits
            {
            private:
                // resolve array traits
                typedef array_traits<T> traits_type;

            public:
                typedef BOOST_STRING_TYPENAME
                    traits_type::value_type value_type;
                typedef BOOST_STRING_TYPENAME
                    traits_type::iterator iterator;
                typedef BOOST_STRING_TYPENAME
                    traits_type::const_iterator const_iterator;
                typedef BOOST_STRING_TYPENAME
                    traits_type::size_type size_type;
                typedef BOOST_STRING_TYPENAME
                    traits_type::difference_type difference_type;

                typedef BOOST_STRING_TYPENAME
                    ::boost::mpl::if_< ::boost::is_const<T>,
                        const_iterator,
                        iterator 
                    >::type result_iterator;
                
            private:
                // resolve array size
                typedef BOOST_STRING_TYPENAME
                    ::boost::remove_cv<value_type>::type char_type;
                typedef BOOST_STRING_TYPENAME
                    array_length_selector<char_type>::
                        BOOST_NESTED_TEMPLATE array_length<traits_type> array_length_type;

            public:
                BOOST_STATIC_CONSTANT( size_type, array_size = traits_type::array_size );

                // static operations
                template< typename A >
                static size_type size( const A& a )
                {
                    return array_length_type::length(a);
                }

                template< typename A >
                static bool empty( const A& a )
                {
                    return array_length_type::empty(a);
                }
                

#ifndef BOOST_NO_FUNCTION_TEMPLATE_ORDERING

                template< typename A >
                static iterator begin( A& a )
                {
                    return a;
                }

                template< typename A >
                static const_iterator begin( const A& a )
                {
                    return a;
                }

                template< typename A >
                static iterator end( A& a )
                {
                    return a+array_length_type::length(a);
                }

                template< typename A >
                static const_iterator end( const A& a )
                {
                    return a+array_length_type::length(a);
                }

#else // BOOST_NO_FUNCTION_TEMPLATE_ORDERING

                template< typename A >
                static result_iterator begin( A& a )
                {
                    return a;
                }

                template< typename A >
                static result_iterator end( A& a )
                {
                    return a+array_length_type::length(a);
                }

#endif // BOOST_NO_FUNCTION_TEMPLATE_ORDERING    

            }; 

            template<typename T>
            struct array_container_traits_selector
            {
                typedef array_container_traits<T> type;
            };

// Pointer container traits ---------------------------------------------------------------

            template<typename T>
            struct pointer_container_traits
            {
                typedef BOOST_STRING_TYPENAME
                    ::boost::remove_pointer<T>::type value_type;

                typedef BOOST_STRING_TYPENAME
                    ::boost::remove_cv<value_type>::type char_type;
                typedef ::std::char_traits<char_type> char_traits;

                typedef value_type* iterator;
                typedef const value_type* const_iterator;
                typedef std::ptrdiff_t difference_type;
                typedef std::size_t size_type;

                typedef BOOST_STRING_TYPENAME
                    ::boost::mpl::if_< ::boost::is_const<T>,
                        const_iterator,
                        iterator 
                    >::type result_iterator;

                // static operations
                template< typename P >
                static size_type size( const P& p )
                {
                    if ( p==0 ) 
                        return 0;
                    else
                        return char_traits::length(p);
                }

                template< typename P >
                static bool empty( const P& p )
                {
                    return p==0 || p[0]==0;
                }

#ifndef BOOST_NO_FUNCTION_TEMPLATE_ORDERING

                template< typename P >
                static iterator begin( P& p )
                {
                    return p;
                }

                template< typename P >
                static const_iterator begin( const P& p )
                {
                    return p;
                }

                template< typename P >
                static iterator end( P& p )
                {
                    if ( p==0 )
                        return p;
                    else
                        return p+char_traits::length(p);
                }

                template< typename P >
                static const_iterator end( const P& p )
                {
                    if ( p==0 )
                        return p;
                    else
                        return p+char_traits::length(p);
                }

#else // BOOST_NO_FUNCTION_TEMPLATE_ORDERING

                template< typename P >
                static result_iterator begin( P& p )
                {
                    return p;
                }

                template< typename P >
                static result_iterator end( P& p )
                {
                    if ( p==0 )
                        return p;
                    else
                        return p+char_traits::length(p);
                }

#endif // BOOST_NO_FUNCTION_TEMPLATE_ORDERING    
            }; 

            template<typename T>
            struct pointer_container_traits_selector
            {
                typedef pointer_container_traits<T> type;
            };

        } // namespace detail
    } // namespace algorithm
} // namespace boost


#endif  // BOOST_STRING_DETAIL_COLLECTION_HPP
