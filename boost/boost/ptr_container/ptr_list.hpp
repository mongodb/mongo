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

#ifndef BOOST_PTR_CONTAINER_PTR_LIST_HPP
#define BOOST_PTR_CONTAINER_PTR_LIST_HPP

#if defined(_MSC_VER) && (_MSC_VER >= 1200)
# pragma once
#endif

#include <boost/ptr_container/ptr_sequence_adapter.hpp>
#include <list>

namespace boost
{

    template
    < 
        class T, 
        class CloneAllocator = heap_clone_allocator,
        class Allocator      = std::allocator<void*>
    >
    class ptr_list : public 
        ptr_sequence_adapter< T, 
                              std::list<void*,Allocator>, 
                              CloneAllocator >
    {
        typedef    ptr_sequence_adapter< T, 
                                         std::list<void*,Allocator>, 
                                         CloneAllocator >
            base_class;

        typedef ptr_list<T,CloneAllocator,Allocator> this_type;
        
    public:
        BOOST_PTR_CONTAINER_DEFINE_NON_INHERITED_MEMBERS( ptr_list, 
                                                          base_class,
                                                          this_type );
        
    public:
        using base_class::merge;
        
        void merge( ptr_list& x )                                 
        {
            merge( x, std::less<T>() );
        }

        template< typename Compare > 
        void merge( ptr_list& x, Compare comp )                   
        {
            this->c_private().merge( x.c_private(), void_ptr_indirect_fun<Compare,T>( comp ) );
        }

        void sort()                                                    
        { 
            sort( std::less<T>() ); 
        };

        template< typename Compare > 
        void sort( Compare comp )                             
        {
            this->c_private().sort( void_ptr_indirect_fun<Compare,T>( comp ) );
        }

    }; // class 'ptr_list'

    //////////////////////////////////////////////////////////////////////////////
    // clonability

    template< typename T, typename CA, typename A >
    inline ptr_list<T,CA,A>* new_clone( const ptr_list<T,CA,A>& r )
    {
        return r.clone().release();
    }
    
    /////////////////////////////////////////////////////////////////////////
    // swap

    template< typename T, typename CA, typename A >
    inline void swap( ptr_list<T,CA,A>& l, ptr_list<T,CA,A>& r )
    {
        l.swap(r);
    }
}


#endif
