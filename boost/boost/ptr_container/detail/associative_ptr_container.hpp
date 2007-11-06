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


#ifndef BOOST_PTR_CONTAINER_DETAIL_ASSOCIATIVE_PTR_CONTAINER_HPP
#define BOOST_PTR_CONTAINER_DETAIL_ASSOCIATIVE_PTR_CONTAINER_HPP

#if defined(_MSC_VER) && (_MSC_VER >= 1200)
# pragma once
#endif

#include <boost/ptr_container/detail/reversible_ptr_container.hpp>

namespace boost
{

namespace ptr_container_detail
{
    template
    <
        class Config,
        class CloneAllocator
    >
    class associative_ptr_container :
        public reversible_ptr_container<Config,CloneAllocator>
    {
        typedef reversible_ptr_container<Config,CloneAllocator>
                                base_type;

        typedef BOOST_DEDUCED_TYPENAME base_type::scoped_deleter
                                scoped_deleter;

    public: // typedefs
        typedef BOOST_DEDUCED_TYPENAME Config::key_type
                                key_type;
        typedef BOOST_DEDUCED_TYPENAME Config::key_compare
                                key_compare;
        typedef BOOST_DEDUCED_TYPENAME Config::value_compare
                                value_compare;
        typedef BOOST_DEDUCED_TYPENAME Config::iterator
                                iterator;
        typedef BOOST_DEDUCED_TYPENAME Config::const_iterator
                                const_iterator;
        typedef BOOST_DEDUCED_TYPENAME base_type::size_type
                                size_type;

    public: // foundation

       template< class Compare, class Allocator >
       associative_ptr_container( const Compare& comp,
                                    const Allocator& a )
         : base_type( comp, a )
       { }

       template< class InputIterator, class Compare, class Allocator >
       associative_ptr_container( InputIterator first, InputIterator last,
                                    const Compare& comp,
                                    const Allocator& a )
         : base_type( first, last, comp, a )
       { }

       template< class PtrContainer >
       associative_ptr_container( std::auto_ptr<PtrContainer> r )
         : base_type( r, key_compare() )
       { }

       template< class PtrContainer >
       void operator=( std::auto_ptr<PtrContainer> r )
       {
           base_type::operator=( r );
       }

    public: // associative container interface
        key_compare key_comp() const
        {
            return this->c_private().key_comp();
        }

        value_compare value_comp() const
        {
            return this->c_private().value_comp();
        }

        iterator erase( iterator before ) // nothrow
        {
            BOOST_ASSERT( !this->empty() );
            BOOST_ASSERT( before != this->end() );

            this->remove( before );                      // nothrow
            iterator res( before );                      // nothrow
            ++res;                                       // nothrow
            this->c_private().erase( before.base() );    // nothrow
            return res;                                  // nothrow
        }

        size_type erase( const key_type& x ) // nothrow
        {
            iterator i( this->c_private().find( x ) );  // nothrow
            if( i == this->end() )                      // nothrow
                return 0u;                              // nothrow
            this->remove( i );                          // nothrow
            return this->c_private().erase( x );        // nothrow
        }

        iterator erase( iterator first,
                        iterator last ) // nothrow
        {
            iterator res( last );                                // nothrow
            if( res != this->end() )
                ++res;                                           // nothrow

            this->remove( first, last );                         // nothrow
            this->c_private().erase( first.base(), last.base() );// nothrow
            return res;                                          // nothrow
        }

    protected:

        template< class AssociatePtrCont >
        void multi_transfer( BOOST_DEDUCED_TYPENAME AssociatePtrCont::iterator object,
                             AssociatePtrCont& from ) // strong
        {
            BOOST_ASSERT( (void*)&from != (void*)this );
            BOOST_ASSERT( !from.empty() && "Cannot transfer from empty container" );

            this->c_private().insert( *object.base() );     // strong
            from.c_private().erase( object.base() );        // nothrow
        }

        template< class AssociatePtrCont >
        size_type multi_transfer( BOOST_DEDUCED_TYPENAME AssociatePtrCont::iterator first,
                                  BOOST_DEDUCED_TYPENAME AssociatePtrCont::iterator last,
                                  AssociatePtrCont& from ) // basic
        {
            BOOST_ASSERT( (void*)&from != (void*)this );
            BOOST_ASSERT( !from.empty() && "Cannot transfer from empty container" );

            size_type res = 0;
            for( ; first != last; )
            {
                BOOST_ASSERT( first != from.end() );
                this->c_private().insert( *first.base() );     // strong
                BOOST_DEDUCED_TYPENAME AssociatePtrCont::iterator 
                    to_delete( first );
                ++first;
                from.c_private().erase( to_delete.base() );    // nothrow
                ++res;
            }

            return res;
        }

        template< class AssociatePtrCont >
        bool single_transfer( BOOST_DEDUCED_TYPENAME AssociatePtrCont::iterator object,
                              AssociatePtrCont& from ) // strong
        {
            BOOST_ASSERT( (void*)&from != (void*)this );
            BOOST_ASSERT( !from.empty() && "Cannot transfer from empty container" );

            std::pair<BOOST_DEDUCED_TYPENAME base_type::ptr_iterator,bool> p =
                this->c_private().insert( *object.base() );     // strong
            if( p.second )
                from.c_private().erase( object.base() );        // nothrow

            return p.second;
        }

        template< class AssociatePtrCont >
        size_type single_transfer( BOOST_DEDUCED_TYPENAME AssociatePtrCont::iterator first,
                                   BOOST_DEDUCED_TYPENAME AssociatePtrCont::iterator last,
                                   AssociatePtrCont& from ) // basic
        {
            BOOST_ASSERT( (void*)&from != (void*)this );
            BOOST_ASSERT( !from.empty() && "Cannot transfer from empty container" );

            size_type res = 0;
            for( ; first != last; )
            {
                BOOST_ASSERT( first != from.end() );
                std::pair<BOOST_DEDUCED_TYPENAME base_type::ptr_iterator,bool> p =
                    this->c_private().insert( *first.base() );     // strong
                BOOST_DEDUCED_TYPENAME AssociatePtrCont::iterator 
                    to_delete( first );
                ++first;
                if( p.second )
                {
                    from.c_private().erase( to_delete.base() );   // nothrow
                    ++res;
                }
            }
            return res;
        }

     }; // class 'associative_ptr_container'
    
} // namespace 'ptr_container_detail'
    
} // namespace 'boost'


#endif
