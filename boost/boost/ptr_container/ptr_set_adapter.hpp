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

#ifndef BOOST_PTR_CONTAINER_PTR_SET_ADAPTER_HPP
#define BOOST_PTR_CONTAINER_PTR_SET_ADAPTER_HPP

#if defined(_MSC_VER) && (_MSC_VER >= 1200)
# pragma once
#endif

#include <boost/ptr_container/detail/associative_ptr_container.hpp>
#include <boost/ptr_container/detail/void_ptr_iterator.hpp>
#include <boost/range/iterator_range.hpp>

namespace boost
{
namespace ptr_container_detail
{
    template
    < 
        class Key,
        class VoidPtrSet
    >
    struct set_config
    {
       typedef VoidPtrSet 
                    void_container_type;

       typedef BOOST_DEDUCED_TYPENAME VoidPtrSet::allocator_type 
                    allocator_type;

       typedef Key  value_type;

       typedef value_type 
                    key_type;

       typedef BOOST_DEDUCED_TYPENAME VoidPtrSet::value_compare
                    value_compare;

       typedef value_compare 
                    key_compare;

       typedef void_ptr_iterator<
                       BOOST_DEDUCED_TYPENAME VoidPtrSet::iterator, Key > 
                    iterator;

       typedef void_ptr_iterator<
                        BOOST_DEDUCED_TYPENAME VoidPtrSet::const_iterator, const Key >
                    const_iterator;

       template< class Iter >
       static Key* get_pointer( Iter i )
       {
           return static_cast<Key*>( *i.base() );
       }

       template< class Iter >
       static const Key* get_const_pointer( Iter i )
       {
           return static_cast<const Key*>( *i.base() );
       }

       BOOST_STATIC_CONSTANT(bool, allow_null = false );
    };

 
    
    template
    < 
        class Key,
        class VoidPtrSet, 
        class CloneAllocator = heap_clone_allocator
    >
    class ptr_set_adapter_base 
        : public ptr_container_detail::associative_ptr_container< set_config<Key,VoidPtrSet>,
                                                      CloneAllocator >
    {
        typedef ptr_container_detail::associative_ptr_container< set_config<Key,VoidPtrSet>,
                                                     CloneAllocator >
              base_type;
    public:  
        typedef BOOST_DEDUCED_TYPENAME base_type::iterator 
                      iterator;
        typedef BOOST_DEDUCED_TYPENAME base_type::const_iterator 
                      const_iterator;
        typedef Key   key_type;
        typedef BOOST_DEDUCED_TYPENAME base_type::size_type
                      size_type;

    private:
        ptr_set_adapter_base() 
          : base_type( BOOST_DEDUCED_TYPENAME VoidPtrSet::key_compare(),
                       BOOST_DEDUCED_TYPENAME VoidPtrSet::allocator_type() )
        { }

    public:
        
       template< class Compare, class Allocator >
       ptr_set_adapter_base( const Compare& comp,
                             const Allocator& a ) 
         : base_type( comp, a ) 
       { }

       template< class InputIterator, class Compare, class Allocator >
       ptr_set_adapter_base( InputIterator first, InputIterator last,
                             const Compare& comp,
                             const Allocator& a ) 
         : base_type( first, last, comp, a ) 
       { }

        template< class PtrContainer >
        ptr_set_adapter_base( std::auto_ptr<PtrContainer> clone )
         : base_type( clone )
        { }
        
        template< typename PtrContainer >
        void operator=( std::auto_ptr<PtrContainer> clone )    
        {
            base_type::operator=( clone );
        }
                 
        iterator find( const key_type& x )                                                
        {                                                                            
            return iterator( this->c_private().
                             find( const_cast<key_type*>(&x) ) );            
        }                                                                            

        const_iterator find( const key_type& x ) const                                    
        {                                                                            
            return const_iterator( this->c_private().
                                   find( const_cast<key_type*>(&x) ) );                  
        }                                                                            

        size_type count( const key_type& x ) const                                        
        {                                                                            
            return this->c_private().count( const_cast<key_type*>(&x) );                      
        }                                                                            
                                                                                     
        iterator lower_bound( const key_type& x )                                         
        {                                                                            
            return iterator( this->c_private().
                             lower_bound( const_cast<key_type*>(&x) ) );                   
        }                                                                            
                                                                                     
        const_iterator lower_bound( const key_type& x ) const                             
        {                                                                            
            return const_iterator( this->c_private().
                                   lower_bound( const_cast<key_type*>(&x) ) );       
        }                                                                            
                                                                                     
        iterator upper_bound( const key_type& x )                                         
        {                                                                            
            return iterator( this->c_private().
                             upper_bound( const_cast<key_type*>(&x) ) );           
        }                                                                            
                                                                                     
        const_iterator upper_bound( const key_type& x ) const                             
        {                                                                            
            return const_iterator( this->c_private().
                                   upper_bound( const_cast<key_type*>(&x) ) );             
        }                                                                            
                                                                                     
        iterator_range<iterator> equal_range( const key_type& x )                    
        {                                                                            
            std::pair<BOOST_DEDUCED_TYPENAME base_type::ptr_iterator,
                      BOOST_DEDUCED_TYPENAME base_type::ptr_iterator> 
                p = this->c_private().
                equal_range( const_cast<key_type*>(&x) );   
            return make_iterator_range( iterator( p.first ), 
                                        iterator( p.second ) );      
        }                                                                            
                                                                                     
        iterator_range<const_iterator> equal_range( const key_type& x ) const  
        {                                                                            
            std::pair<BOOST_DEDUCED_TYPENAME base_type::ptr_const_iterator,
                      BOOST_DEDUCED_TYPENAME base_type::ptr_const_iterator> 
                p = this->c_private().
                equal_range( const_cast<key_type*>(&x) ); 
            return make_iterator_range( const_iterator( p.first ), 
                                        const_iterator( p.second ) );    
        }                                                                            

    };

} // ptr_container_detail

    /////////////////////////////////////////////////////////////////////////
    // ptr_set_adapter
    /////////////////////////////////////////////////////////////////////////
  
    template
    <
        class Key,
        class VoidPtrSet, 
        class CloneAllocator = heap_clone_allocator
    >
    class ptr_set_adapter : 
        public ptr_container_detail::ptr_set_adapter_base<Key,VoidPtrSet,CloneAllocator>
    {
        typedef ptr_container_detail::ptr_set_adapter_base<Key,VoidPtrSet,CloneAllocator> 
            base_type;
    
    public: // typedefs
       
        typedef BOOST_DEDUCED_TYPENAME base_type::iterator
                     iterator;                 
        typedef BOOST_DEDUCED_TYPENAME base_type::const_iterator
                     const_iterator;                 
        typedef BOOST_DEDUCED_TYPENAME base_type::size_type
                     size_type;    
        typedef Key  key_type;
        typedef BOOST_DEDUCED_TYPENAME base_type::auto_type
                     auto_type;
        typedef BOOST_DEDUCED_TYPENAME VoidPtrSet::key_compare
                      key_compare;
        typedef BOOST_DEDUCED_TYPENAME VoidPtrSet::allocator_type
                      allocator_type;        
    private:
        
        template< typename II >                                               
        void set_basic_clone_and_insert( II first, II last ) // basic                 
        {                                                                     
            while( first != last )                                            
            {           
                if( this->find( *first ) == this->end() )
                    insert( CloneAllocator::allocate_clone( *first ) ); // strong, commit
                ++first;                                                      
            }                                                                 
        }                         

    public:
        
        explicit ptr_set_adapter( const key_compare& comp = key_compare(),
                                  const allocator_type& a = allocator_type() ) 
          : base_type( comp, a ) 
        {
            BOOST_ASSERT( this->empty() ); 
        }
        
        template< class InputIterator, class Compare, class Allocator >
        ptr_set_adapter( InputIterator first, InputIterator last, 
                         const Compare& comp = Compare(),
                         const Allocator a = Allocator() )
          : base_type( comp, a )
        {
            BOOST_ASSERT( this->empty() );
            set_basic_clone_and_insert( first, last );
        }

        template< class T >
        ptr_set_adapter( std::auto_ptr<T> r ) : base_type( r )
        { }

        template< class T >
        void operator=( std::auto_ptr<T> r ) 
        {
            base_type::operator=( r );
        }

        std::pair<iterator,bool> insert( key_type* x ) // strong                      
        {       
            this->enforce_null_policy( x, "Null pointer in 'ptr_set::insert()'" );
            
            auto_type ptr( x );                                
            std::pair<BOOST_DEDUCED_TYPENAME base_type::ptr_iterator,bool>
                 res = this->c_private().insert( x );       
            if( res.second )                                                 
                ptr.release();                                                  
            return std::make_pair( iterator( res.first ), res.second );     
        }

        template< class U >
        std::pair<iterator,bool> insert( std::auto_ptr<U> x )
        {
            return insert( x.release() );
        }

        
        iterator insert( iterator where, key_type* x ) // strong
        {
            this->enforce_null_policy( x, "Null pointer in 'ptr_set::insert()'" );

            auto_type ptr( x );                                
            BOOST_DEDUCED_TYPENAME base_type::ptr_iterator 
                res = this->c_private().insert( where.base(), x );
            if( *res == x )                                                 
                ptr.release();                                                  
            return iterator( res);
        }

        template< class U >
        iterator insert( iterator where, std::auto_ptr<U> x )
        {
            return insert( where, x.release() );
        }
        
        template< typename InputIterator >
        void insert( InputIterator first, InputIterator last ) // basic
        {
            set_basic_clone_and_insert( first, last );
        }

#if defined(BOOST_NO_SFINAE) || BOOST_WORKAROUND(__SUNPRO_CC, <= 0x580)
#else    
        
        template< class Range >
        BOOST_DEDUCED_TYPENAME
        boost::disable_if< ptr_container_detail::is_pointer_or_integral<Range> >::type
        insert( const Range& r )
        {
            insert( boost::begin(r), boost::end(r) );
        }

#endif        

        template< class PtrSetAdapter >
        bool transfer( BOOST_DEDUCED_TYPENAME PtrSetAdapter::iterator object, 
                       PtrSetAdapter& from ) // strong
        {
            return this->single_transfer( object, from );
        }

        template< class PtrSetAdapter >
        size_type 
        transfer( BOOST_DEDUCED_TYPENAME PtrSetAdapter::iterator first, 
                  BOOST_DEDUCED_TYPENAME PtrSetAdapter::iterator last, 
                  PtrSetAdapter& from ) // basic
        {
            return this->single_transfer( first, last, from );
        }

#if defined(BOOST_NO_SFINAE) || BOOST_WORKAROUND(__SUNPRO_CC, <= 0x580)
#else    

        template< class PtrSetAdapter, class Range >
        BOOST_DEDUCED_TYPENAME boost::disable_if< boost::is_same< Range,
                            BOOST_DEDUCED_TYPENAME PtrSetAdapter::iterator >,
                                                            size_type >::type
        transfer( const Range& r, PtrSetAdapter& from ) // basic
        {
            return transfer( boost::begin(r), boost::end(r), from );
        }

#endif

        template< class PtrSetAdapter >
        size_type transfer( PtrSetAdapter& from ) // basic
        {
            return transfer( from.begin(), from.end(), from );
        }

    };
    
    /////////////////////////////////////////////////////////////////////////
    // ptr_multiset_adapter
    /////////////////////////////////////////////////////////////////////////

    template
    < 
        class Key,
        class VoidPtrMultiSet, 
        class CloneAllocator = heap_clone_allocator 
    >
    class ptr_multiset_adapter : 
        public ptr_container_detail::ptr_set_adapter_base<Key,VoidPtrMultiSet,CloneAllocator>
    {
         typedef ptr_container_detail::ptr_set_adapter_base<Key,VoidPtrMultiSet,CloneAllocator> base_type;
    
    public: // typedefs
    
        typedef BOOST_DEDUCED_TYPENAME base_type::iterator   
                       iterator;          
        typedef BOOST_DEDUCED_TYPENAME base_type::size_type
                       size_type;
        typedef Key    key_type;
        typedef BOOST_DEDUCED_TYPENAME base_type::auto_type
                       auto_type;
        typedef BOOST_DEDUCED_TYPENAME VoidPtrMultiSet::key_compare
                       key_compare;
        typedef BOOST_DEDUCED_TYPENAME VoidPtrMultiSet::allocator_type
                       allocator_type;        
    private:
        template< typename II >                                               
        void set_basic_clone_and_insert( II first, II last ) // basic                 
        {               
            while( first != last )                                            
            {           
                insert( CloneAllocator::allocate_clone( *first ) ); // strong, commit                              
                ++first;                                                     
            }                                                                 
        }                         
    
    public:

        explicit ptr_multiset_adapter( const key_compare& comp = key_compare(),
                                       const allocator_type& a = allocator_type() )
        : base_type( comp, a ) 
        { }
    
        template< class InputIterator >
        ptr_multiset_adapter( InputIterator first, InputIterator last,
                              const key_compare& comp = key_compare(),
                              const allocator_type& a = allocator_type() )
        : base_type( comp, a ) 
        {
            set_basic_clone_and_insert( first, last );
        }

        template< class T >
        ptr_multiset_adapter( std::auto_ptr<T> r ) : base_type( r )
        { }

        template< class T >
        void operator=( std::auto_ptr<T> r ) 
        {
            base_type::operator=( r ); 
        }

        iterator insert( iterator before, key_type* x ) // strong  
        {
            return base_type::insert( before, x ); 
        } 

        template< class U >
        iterator insert( iterator before, std::auto_ptr<U> x )
        {
            return insert( before, x.release() );
        }
    
        iterator insert( key_type* x ) // strong                                      
        {   
            this->enforce_null_policy( x, "Null pointer in 'ptr_multiset::insert()'" );
    
            auto_type ptr( x );                                
            BOOST_DEDUCED_TYPENAME base_type::ptr_iterator
                 res = this->c_private().insert( x );                         
            ptr.release();                                                      
            return iterator( res );                                             
        }

        template< class U >
        iterator insert( std::auto_ptr<U> x )
        {
            return insert( x.release() );
        }
    
        template< typename InputIterator >
        void insert( InputIterator first, InputIterator last ) // basic
        {
            set_basic_clone_and_insert( first, last );
        }

#if defined(BOOST_NO_SFINAE) || BOOST_WORKAROUND(__SUNPRO_CC, <= 0x580)
#else    
        
        template< class Range >
        BOOST_DEDUCED_TYPENAME
        boost::disable_if< ptr_container_detail::is_pointer_or_integral<Range> >::type
        insert( const Range& r )
        {
            insert( boost::begin(r), boost::end(r) );
        }

#endif

        template< class PtrSetAdapter >
        void transfer( BOOST_DEDUCED_TYPENAME PtrSetAdapter::iterator object, 
                       PtrSetAdapter& from ) // strong
        {
            this->multi_transfer( object, from );
        }

        template< class PtrSetAdapter >
        size_type transfer( BOOST_DEDUCED_TYPENAME PtrSetAdapter::iterator first, 
                            BOOST_DEDUCED_TYPENAME PtrSetAdapter::iterator last, 
                            PtrSetAdapter& from ) // basic
        {
            return this->multi_transfer( first, last, from );
        }

#if defined(BOOST_NO_SFINAE) || BOOST_WORKAROUND(__SUNPRO_CC, <= 0x580)
#else    
        
        template< class PtrSetAdapter, class Range >
        BOOST_DEDUCED_TYPENAME boost::disable_if< boost::is_same< Range,
                       BOOST_DEDUCED_TYPENAME PtrSetAdapter::iterator >, size_type >::type
        transfer(  const Range& r, PtrSetAdapter& from ) // basic
        {
            return transfer( boost::begin(r), boost::end(r), from );
        }

#endif        

        template< class PtrSetAdapter >
        void transfer( PtrSetAdapter& from ) // basic
        {
            transfer( from.begin(), from.end(), from );
            BOOST_ASSERT( from.empty() );
        }
        
    };

} // namespace 'boost'  

#endif
