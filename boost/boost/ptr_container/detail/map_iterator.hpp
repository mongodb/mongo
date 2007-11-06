//
// Boost.Pointer Container
//
//  Copyright Thorsten Ottosen 2003-2005. Use, modification and
//  distribution is subject to the Boost Software License, Version
//  1.0. (See accompanying file LICENSE_1_0.txt or copy at
//  http://www.boost.org/LICENSE_1_0.txt)
//
// For more information, see http://www.boost.org/libs/ptr_container/
//

#ifndef BOOST_PTR_CONTAINER_MAP_ITERATOR_HPP
#define BOOST_PTR_CONTAINER_MAP_ITERATOR_HPP

#if defined(_MSC_VER) && (_MSC_VER >= 1200)
# pragma once
#endif

#include <boost/config.hpp>
#include <boost/iterator/iterator_adaptor.hpp>
#include <utility>

namespace boost
{ 
    namespace ptr_container_detail
    {
        template< class F, class S >
        struct ref_pair
        {
            typedef F first_type;
            typedef S second_type;

            const F& first;
            S        second;

            template< class F2, class S2 >
            ref_pair( const std::pair<F2,S2>& p )
            : first(p.first), second(static_cast<S>(p.second))
            { }

            template< class RP >
            ref_pair( const RP* rp )
            : first(rp->first), second(rp->second)
            { }
            
            const ref_pair* const operator->() const
            {
                return this;
            }
        };
    }
    
    template< 
              class I, // base iterator 
              class F, // first type, key type
              class S  // second type, mapped type
            > 
    class ptr_map_iterator : 
        public boost::iterator_adaptor< ptr_map_iterator<I,F,S>, I, 
                                        ptr_container_detail::ref_pair<F,S>, 
                                        use_default, 
                                        ptr_container_detail::ref_pair<F,S> >
    {
        typedef boost::iterator_adaptor< ptr_map_iterator<I,F,S>, I, 
                                         ptr_container_detail::ref_pair<F,S>,
                                         use_default, 
                                         ptr_container_detail::ref_pair<F,S> > 
            base_type;


    public:
        ptr_map_iterator() : base_type()                                 
        { }
        
        explicit ptr_map_iterator( const I& i ) : base_type(i)
        { }

        template< class I2, class F2, class S2 >
            ptr_map_iterator( const ptr_map_iterator<I2,F2,S2>& r ) 
         : base_type(r.base())
        { }
        
   }; // class 'ptr_map_iterator'

}

#endif
