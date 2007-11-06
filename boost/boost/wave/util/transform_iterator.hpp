/*=============================================================================
    Boost.Wave: A Standard compliant C++ preprocessor library

    http://www.boost.org/

    Copyright (c) 2001-2007 Hartmut Kaiser. Distributed under the Boost
    Software License, Version 1.0. (See accompanying file
    LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
=============================================================================*/

#if !defined(TRANSFORM_ITERATOR_HPP_D492C659_88C7_4258_8C42_192F9AE80EC0_INCLUDED)
#define TRANSFORM_ITERATOR_HPP_D492C659_88C7_4258_8C42_192F9AE80EC0_INCLUDED

#include <boost/config.hpp>
#include <boost/iterator_adaptors.hpp>
#if BOOST_ITERATOR_ADAPTORS_VERSION >= 0x0200
#include <boost/iterator/transform_iterator.hpp>
#endif // BOOST_ITERATOR_ADAPTORS_VERSION >= 0x0200

#include <boost/assert.hpp>

// this must occur after all of the includes and before any code appears
#ifdef BOOST_HAS_ABI_HEADERS
#include BOOST_ABI_PREFIX
#endif

///////////////////////////////////////////////////////////////////////////////
namespace boost {
namespace wave { 
namespace impl {

#if BOOST_ITERATOR_ADAPTORS_VERSION < 0x0200

///////////////////////////////////////////////////////////////////////////////
// 
//  Transform Iterator Adaptor
//
//  Upon deference, apply some unary function object and return the
//  result by reference.
//
//  This class is adapted from the Boost.Iterator library, where a similar 
//  class exists, which returns the next item by value
//
///////////////////////////////////////////////////////////////////////////////
    template <class AdaptableUnaryFunctionT>
    struct ref_transform_iterator_policies 
    :   public boost::default_iterator_policies
    {
        ref_transform_iterator_policies() 
        {}
        ref_transform_iterator_policies(const AdaptableUnaryFunctionT &f) 
        : m_f(f) {}

        template <class IteratorAdaptorT>
        typename IteratorAdaptorT::reference
        dereference(const IteratorAdaptorT &iter) const
        { return m_f(*iter.base()); }

        AdaptableUnaryFunctionT m_f;
    };

    template <class AdaptableUnaryFunctionT, class IteratorT>
    class ref_transform_iterator_generator
    {
        typedef typename AdaptableUnaryFunctionT::result_type value_type;
        
    public:
        typedef boost::iterator_adaptor<
                IteratorT,
                ref_transform_iterator_policies<AdaptableUnaryFunctionT>,
                value_type, value_type const &, value_type const *, 
                std::input_iterator_tag>
            type;
    };

    template <class AdaptableUnaryFunctionT, class IteratorT>
    inline 
    typename ref_transform_iterator_generator<
        AdaptableUnaryFunctionT, IteratorT>::type
    make_ref_transform_iterator(
        IteratorT base,
        const AdaptableUnaryFunctionT &f = AdaptableUnaryFunctionT())
    {
        typedef typename ref_transform_iterator_generator<
                    AdaptableUnaryFunctionT, IteratorT>::type 
            result_t;
        return result_t(base, f);
    }

    //  Retrieve the token value given a parse node
    //  This is used in conjunction with the ref_transform_iterator above, to
    //  get the token values while iterating directly over the parse tree.
    template <typename TokenT, typename ParseTreeNodeT>
    struct get_token_value {

        typedef TokenT result_type;
        
        TokenT const &operator()(ParseTreeNodeT const &node) const
        {
            BOOST_ASSERT(1 == std::distance(node.value.begin(), 
                node.value.end()));
            return *node.value.begin();
        }
    };

#else // BOOST_ITERATOR_ADAPTORS_VERSION < 0x0200

///////////////////////////////////////////////////////////////////////////////
//
//  The new Boost.Iterator library already conatins a transform_iterator usable 
//  for our needs. The code below wraps this up.
//
///////////////////////////////////////////////////////////////////////////////
    template <class AdaptableUnaryFunctionT, class IteratorT>
    class ref_transform_iterator_generator
    {
        typedef typename AdaptableUnaryFunctionT::result_type   return_type;
        typedef typename AdaptableUnaryFunctionT::argument_type argument_type;
        
    public:
        typedef boost::transform_iterator<
                return_type (*)(argument_type), IteratorT, return_type>
            type;
    };

    template <class AdaptableUnaryFunctionT, class IteratorT>
    inline 
    typename ref_transform_iterator_generator<
        AdaptableUnaryFunctionT, IteratorT>::type
    make_ref_transform_iterator(
        IteratorT base, AdaptableUnaryFunctionT const &f)
    {
        typedef typename ref_transform_iterator_generator<
                AdaptableUnaryFunctionT, IteratorT>::type
            iterator_type;
        return iterator_type(base, f.transform);
    }

    //  Retrieve the token value given a parse node
    //  This is used in conjunction with the ref_transform_iterator above, to
    //  get the token values while iterating directly over the parse tree.
    template <typename TokenT, typename ParseTreeNodeT>
    struct get_token_value {

        typedef TokenT const &result_type;
        typedef ParseTreeNodeT const &argument_type;
        
        static result_type
        transform (argument_type node) 
        {
            BOOST_ASSERT(1 == std::distance(node.value.begin(), 
                node.value.end()));
            return *node.value.begin();
        }
    };

#endif // BOOST_ITERATOR_ADAPTORS_VERSION < 0x0200
    
///////////////////////////////////////////////////////////////////////////////
}   // namespace impl
}   // namespace wave
}   // namespace boost

// the suffix header occurs after all of the code
#ifdef BOOST_HAS_ABI_HEADERS
#include BOOST_ABI_SUFFIX
#endif

#endif // !defined(TRANSFORM_ITERATOR_HPP_D492C659_88C7_4258_8C42_192F9AE80EC0_INCLUDED)
