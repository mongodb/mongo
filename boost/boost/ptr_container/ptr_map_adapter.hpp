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

#ifndef BOOST_PTR_CONTAINER_DETAIL_PTR_MAP_ADAPTER_HPP
#define BOOST_PTR_CONTAINER_DETAIL_PTR_MAP_ADAPTER_HPP

#if defined(_MSC_VER) && (_MSC_VER >= 1200)
# pragma once
#endif

#include <boost/ptr_container/detail/map_iterator.hpp>
#include <boost/ptr_container/detail/associative_ptr_container.hpp>
#include <boost/static_assert.hpp>
#include <boost/range/iterator_range.hpp>

namespace boost
{
namespace ptr_container_detail
{

    template
    < 
        class T,
        class VoidPtrMap
    >
    struct map_config
    {
        typedef BOOST_DEDUCED_TYPENAME remove_nullable<T>::type
                     U;
        typedef VoidPtrMap 
                     void_container_type;
        
        typedef BOOST_DEDUCED_TYPENAME VoidPtrMap::allocator_type
                     allocator_type;
        
        typedef BOOST_DEDUCED_TYPENAME VoidPtrMap::key_compare
                     key_compare;
        
        typedef BOOST_DEDUCED_TYPENAME VoidPtrMap::value_compare
                     value_compare;
        
        typedef BOOST_DEDUCED_TYPENAME VoidPtrMap::key_type
                     key_type;
        
        typedef U    value_type;

        typedef ptr_map_iterator< BOOST_DEDUCED_TYPENAME VoidPtrMap::iterator, key_type, U* const >
                     iterator;
        
        typedef ptr_map_iterator< BOOST_DEDUCED_TYPENAME VoidPtrMap::const_iterator, key_type, const U* const>
                     const_iterator;
  
        template< class Iter >
        static U* get_pointer( Iter i )
        {
            return i->second;
        }

        template< class Iter >
        static const U* get_const_pointer( Iter i )
        {
            return i->second;
        }

        BOOST_STATIC_CONSTANT( bool, allow_null = boost::is_nullable<T>::value );
    };
    
    

    template
    < 
        class T,
        class VoidPtrMap, 
        class CloneAllocator
    >
    class ptr_map_adapter_base : 
        public ptr_container_detail::associative_ptr_container< map_config<T,VoidPtrMap>,
                                                    CloneAllocator >
    {
        typedef ptr_container_detail::associative_ptr_container< map_config<T,VoidPtrMap>,
                                                     CloneAllocator > 
            base_type;

        typedef map_config<T,VoidPtrMap>                           config;

        typedef ptr_map_adapter_base<T,VoidPtrMap,CloneAllocator>  this_type;
        
    public:

        typedef BOOST_DEDUCED_TYPENAME base_type::allocator_type
                    allocator_type;
        typedef BOOST_DEDUCED_TYPENAME base_type::iterator
                    iterator;
        typedef BOOST_DEDUCED_TYPENAME base_type::const_iterator
                    const_iterator;
        typedef BOOST_DEDUCED_TYPENAME base_type::size_type
                    size_type;
        typedef BOOST_DEDUCED_TYPENAME base_type::key_type
                    key_type;
        typedef BOOST_DEDUCED_TYPENAME base_type::auto_type
                    auto_type;
        typedef BOOST_DEDUCED_TYPENAME base_type::value_type 
                    mapped_type;
        typedef BOOST_DEDUCED_TYPENAME base_type::reference
                    mapped_reference;
        typedef BOOST_DEDUCED_TYPENAME base_type::const_reference
                    const_mapped_reference;
        typedef BOOST_DEDUCED_TYPENAME iterator_value<iterator>::type
                    value_type;
        typedef value_type
                    reference;
        typedef BOOST_DEDUCED_TYPENAME iterator_value<const_iterator>::type
                    const_reference;
        typedef value_type 
                    pointer;
        typedef const_reference 
                    const_pointer;

    private:
        const_mapped_reference lookup( const key_type& key ) const
        {
           const_iterator i = this->find( key );
           if( i != this->end() )
               return *i->second;
           else                                           
               BOOST_PTR_CONTAINER_THROW_EXCEPTION( true, bad_ptr_container_operation,
                                                    "'ptr_map/multimap::at()' could"
                                                    " not find key" );
        }

        struct eraser // scope guard
        {
            bool            released_;
            VoidPtrMap*     m_;
            const key_type& key_;

            eraser( VoidPtrMap* m, const key_type& key ) 
              : released_(false), m_(m), key_(key)
            {}

            ~eraser() 
            {
                if( !released_ )
                    m_->erase(key_);
            }

            void release() { released_ = true; }
        };

        mapped_reference insert_lookup( const key_type& key )
        {
            void*& ref = this->c_private()[key];
            if( ref )
            {
                return *static_cast<mapped_type>(ref);
            }
            else
            {
                eraser e(&this->c_private(),key); // nothrow
                mapped_type res = new T();        // strong 
                ref = res;                        // nothrow
                e.release();                      // nothrow
                return *res;
            }
          }
        
    public:

        ptr_map_adapter_base( const allocator_type& a = allocator_type() )
        : base_type(a)
        { }

        template< class InputIterator >
        ptr_map_adapter_base( InputIterator first, InputIterator last,
                              const allocator_type& a = allocator_type() )
        : base_type( first, last, a )
        { }
 
        template< class Compare, class Allocator >
        explicit ptr_map_adapter_base( const Compare& comp,
                                       const Allocator& a ) 
        : base_type( comp, a ) 
        { }

        template< class PtrContainer >
        ptr_map_adapter_base( std::auto_ptr<PtrContainer> clone ) 
        : base_type( clone )
        { }
        
        template< typename PtrContainer >
        void operator=( std::auto_ptr<PtrContainer> clone )    
        {
            base_type::operator=( clone );
        }        

        iterator find( const key_type& x )                                                
        {                                                                            
            return iterator( this->c_private().find( x ) );                                
        }                                                                            

        const_iterator find( const key_type& x ) const                                    
        {                                                                            
            return const_iterator( this->c_private().find( x ) );                          
        }                                                                            

        size_type count( const key_type& x ) const                                        
        {                                                                            
            return this->c_private().count( x );                                           
        }                                                                            
                                                                                     
        iterator lower_bound( const key_type& x )                                         
        {                                                                            
            return iterator( this->c_private().lower_bound( x ) );                         
        }                                                                            
                                                                                     
        const_iterator lower_bound( const key_type& x ) const                             
        {                                                                            
            return const_iterator( this->c_private().lower_bound( x ) );                   
        }                                                                            
                                                                                     
        iterator upper_bound( const key_type& x )                                         
        {                                                                            
            return iterator( this->c_private().upper_bound( x ) );                         
        }                                                                            
                                                                                     
        const_iterator upper_bound( const key_type& x ) const                             
        {                                                                            
            return const_iterator( this->c_private().upper_bound( x ) );                   
        }                                                                            
                                                                                     
        iterator_range<iterator> equal_range( const key_type& x )                    
        {                                                                            
            std::pair<BOOST_DEDUCED_TYPENAME base_type::ptr_iterator,
                      BOOST_DEDUCED_TYPENAME base_type::ptr_iterator>
                 p = this->c_private().equal_range( x );   
            return make_iterator_range( iterator( p.first ), iterator( p.second ) );      
        }                                                                            
                                                                                     
        iterator_range<const_iterator> equal_range( const key_type& x ) const  
        {                                                                            
            std::pair<BOOST_DEDUCED_TYPENAME base_type::ptr_const_iterator,
                      BOOST_DEDUCED_TYPENAME base_type::ptr_const_iterator> 
                p = this->c_private().equal_range( x ); 
            return make_iterator_range( const_iterator( p.first ), 
                                        const_iterator( p.second ) );    
        }                                                                            
                                                                                     
        mapped_reference at( const key_type& key )  
        {   
            return const_cast<mapped_reference>( lookup( key ) ); 
        }
                                                                                     
        const_mapped_reference at( const key_type& key ) const
        {                                                                            
            return lookup( key );
        }

        mapped_reference operator[]( const key_type& key )
        {
            return insert_lookup( key );
        }              

        auto_type replace( iterator where, mapped_type x ) // strong  
        { 
            BOOST_ASSERT( where != this->end() );

            this->enforce_null_policy( x, "Null pointer in 'replace()'" );

            auto_type ptr( x );

            BOOST_PTR_CONTAINER_THROW_EXCEPTION( this->empty(),
                                                 bad_ptr_container_operation,
                                                 "'replace()' on empty container" );

            auto_type old( where->second );       // nothrow
            where.base()->second = ptr.release(); // nothrow, commit
            return move( old );
        }

        template< class U >
        auto_type replace( iterator where, std::auto_ptr<U> x )
        {
            return replace( where, x.release() );
        }

    public: // serialization

        template< class Archive >
        void save( Archive& ar, const unsigned ) const
        {
            ar & ptr_container_detail::serialize_as_const( this->size() );

            const_iterator i = this->begin(), e = this->end();
            for( ; i != e; ++i )
            {
                ar & i->first;
                ar & ptr_container_detail::serialize_as_const( i->second );
            }
        }
    };
    
} // ptr_container_detail

    /////////////////////////////////////////////////////////////////////////
    // ptr_map_adapter
    /////////////////////////////////////////////////////////////////////////
    
    template
    < 
        class T,
        class VoidPtrMap, 
        class CloneAllocator = heap_clone_allocator
    >
    class ptr_map_adapter : 
        public ptr_container_detail::ptr_map_adapter_base<T,VoidPtrMap,CloneAllocator>
    {
        typedef ptr_container_detail::ptr_map_adapter_base<T,VoidPtrMap,CloneAllocator> 
            base_type;
    
    public:    
        typedef BOOST_DEDUCED_TYPENAME base_type::iterator 
                     iterator;       
        typedef BOOST_DEDUCED_TYPENAME base_type::const_iterator
                     const_iterator;
        typedef BOOST_DEDUCED_TYPENAME base_type::size_type
                    size_type;
        typedef BOOST_DEDUCED_TYPENAME base_type::key_type
                    key_type;
        typedef BOOST_DEDUCED_TYPENAME base_type::const_reference
                    const_reference;
        typedef BOOST_DEDUCED_TYPENAME base_type::auto_type
                    auto_type;
        typedef BOOST_DEDUCED_TYPENAME VoidPtrMap::key_compare 
                    key_compare;
        typedef BOOST_DEDUCED_TYPENAME VoidPtrMap::allocator_type 
                    allocator_type;
        typedef BOOST_DEDUCED_TYPENAME base_type::mapped_type
                    mapped_type;
    private:

        void safe_insert( const key_type& key, auto_type ptr ) // strong
        {
            std::pair<BOOST_DEDUCED_TYPENAME base_type::ptr_iterator,bool>
                res = 
                this->c_private().insert( std::make_pair( key, ptr.get() ) ); // strong, commit      
            if( res.second )                                                  // nothrow     
                ptr.release();                                                // nothrow
        }
    
        template< class II >                                               
        void map_basic_clone_and_insert( II first, II last )                  
        {       
            while( first != last )                                            
            {                                            
                if( this->find( first->first ) == this->end() )
                {
                    const_reference p = *first.base();     // nothrow                    
                    auto_type ptr( this->null_policy_allocate_clone( p.second ) ); 
                                                              // strong 
                    this->safe_insert( p.first, ptr_container_detail::
                                                move( ptr ) );// strong, commit
                }
                ++first;                                                      
            }                                                                 
        }
    
    public:

        explicit ptr_map_adapter( const key_compare& comp = key_compare(),
                                  const allocator_type& a = allocator_type()  ) 
          : base_type( comp, a ) { }
    
        template< class InputIterator >
        ptr_map_adapter( InputIterator first, InputIterator last, 
                         const key_compare& comp = key_compare(),
                         const allocator_type& a = allocator_type() )
          : base_type( comp, a ) 
        {
            map_basic_clone_and_insert( first, last );
        }

        template< class U >
        ptr_map_adapter( std::auto_ptr<U> r ) : base_type( r )
        { }

        template< class U >
        void operator=( std::auto_ptr<U> r )
        {  
            base_type::operator=( r );
        }

        using base_type::release;

        template< typename InputIterator >
        void insert( InputIterator first, InputIterator last ) // basic
        {
            map_basic_clone_and_insert( first, last );
        }

        template< class Range >
        void insert( const Range& r )
        {
            insert( boost::begin(r), boost::end(r) );
        }

    private:
        std::pair<iterator,bool> insert_impl( const key_type& key, mapped_type x ) // strong
        {
            this->enforce_null_policy( x, "Null pointer in ptr_map_adapter::insert()" );
            auto_type ptr( x );                                              // nothrow

            std::pair<BOOST_DEDUCED_TYPENAME base_type::ptr_iterator,bool>
                 res = this->c_private().insert( std::make_pair( key, x ) ); // strong, commit      
            if( res.second )                                             // nothrow     
                ptr.release();                                           // nothrow
            return std::make_pair( iterator( res.first ), res.second );  // nothrow        
        }
        
    public:
        
        std::pair<iterator,bool> insert( key_type& key, mapped_type x )
        {
            return insert_impl( key, x );
        }

        template< class U >
        std::pair<iterator,bool> insert( const key_type& key, std::auto_ptr<U> x )
        {
            return insert_impl( key, x.release() );
        }

        template< class PtrMapAdapter >
        bool transfer( BOOST_DEDUCED_TYPENAME PtrMapAdapter::iterator object, 
                       PtrMapAdapter& from ) // strong
        {
            return this->single_transfer( object, from );
        }

        template< class PtrMapAdapter >
        size_type transfer( BOOST_DEDUCED_TYPENAME PtrMapAdapter::iterator first, 
                            BOOST_DEDUCED_TYPENAME PtrMapAdapter::iterator last, 
                            PtrMapAdapter& from ) // basic
        {
            return this->single_transfer( first, last, from );
        }

#if defined(BOOST_NO_SFINAE) || BOOST_WORKAROUND(__SUNPRO_CC, <= 0x580)
#else    

        template< class PtrMapAdapter, class Range >
        BOOST_DEDUCED_TYPENAME boost::disable_if< boost::is_same< Range,
                            BOOST_DEDUCED_TYPENAME PtrMapAdapter::iterator >,
                                                            size_type >::type
        transfer( const Range& r, PtrMapAdapter& from ) // basic
        {
            return transfer( boost::begin(r), boost::end(r), from );
        }
        
#endif

        template< class PtrMapAdapter >
        size_type transfer( PtrMapAdapter& from ) // basic
        {
            return transfer( from.begin(), from.end(), from );
        }

    public: // serialization

        template< class Archive >
        void load( Archive& ar, const unsigned ) // strong
        {
            this->clear();
            size_type n;
            ar & n;

            for( size_type i = 0u; i != n; ++i )
            {
                key_type  key;
                T*        value;
                ar & key;
                ar & value;
                std::pair<iterator,bool> p = this->insert( key, value );
                ar.reset_object_address( &p.first->first, &key ); 
            }
        }

        BOOST_SERIALIZATION_SPLIT_MEMBER()
        
  };
  
  /////////////////////////////////////////////////////////////////////////
  // ptr_multimap_adapter
  /////////////////////////////////////////////////////////////////////////

    template
    < 
        class T,
        class VoidPtrMultiMap, 
        class CloneAllocator = heap_clone_allocator
    >
    class ptr_multimap_adapter : 
        public ptr_container_detail::ptr_map_adapter_base<T,VoidPtrMultiMap,CloneAllocator>
    {
        typedef ptr_container_detail::ptr_map_adapter_base<T,VoidPtrMultiMap,CloneAllocator>
             base_type;

    public: // typedefs
        typedef BOOST_DEDUCED_TYPENAME base_type::iterator           
                       iterator;                 
        typedef BOOST_DEDUCED_TYPENAME base_type::const_iterator     
                       const_iterator;           
        typedef BOOST_DEDUCED_TYPENAME base_type::size_type
                       size_type;
        typedef BOOST_DEDUCED_TYPENAME base_type::key_type
                       key_type;
        typedef BOOST_DEDUCED_TYPENAME base_type::const_reference
                       const_reference;
        typedef BOOST_DEDUCED_TYPENAME base_type::mapped_type
                    mapped_type;
        typedef BOOST_DEDUCED_TYPENAME base_type::auto_type
                    auto_type;            
        typedef BOOST_DEDUCED_TYPENAME VoidPtrMultiMap::key_compare 
                    key_compare;
        typedef BOOST_DEDUCED_TYPENAME VoidPtrMultiMap::allocator_type 
                    allocator_type;
    private:

        void safe_insert( const key_type& key, auto_type ptr ) // strong
        {
            this->c_private().insert( 
                           std::make_pair( key, ptr.get() ) ); // strong, commit      
            ptr.release();                                     // nothrow
        }
        
        template< typename II >                                               
        void map_basic_clone_and_insert( II first, II last )                  
        {                                                         
            while( first != last )                                            
            {                                            
                const_reference pair = *first.base();     // nothrow                     
                auto_type ptr( this->null_policy_allocate_clone( pair.second ) );    
                                                          // strong
                safe_insert( pair.first, ptr_container_detail::
                                         move( ptr ) );   // strong, commit
                ++first;                                                      
            }                                                                 
        }
        
    public:
        
        explicit ptr_multimap_adapter( const key_compare& comp = key_compare(),
                                       const allocator_type& a = allocator_type() )
          : base_type( comp, a ) { }
        
        template< class InputIterator >
        ptr_multimap_adapter( InputIterator first, InputIterator last,
                              const key_compare& comp = key_compare(),
                              const allocator_type& a = allocator_type() )
          : base_type( comp, a )
        {
            map_basic_clone_and_insert( first, last );
        }

        template< class U >
        ptr_multimap_adapter( std::auto_ptr<U> r ) : base_type( r )
        { }

        template< class U >
        void operator=( std::auto_ptr<U> r )
        {  
            base_type::operator=( r );
        }

        using base_type::release;
        
        template< typename InputIterator >
        void insert( InputIterator first, InputIterator last ) // basic
        {
            map_basic_clone_and_insert( first, last );
        }

        template< class Range >
        void insert( const Range& r )
        {
            insert( boost::begin(r), boost::end(r) );
        }

        iterator insert( key_type& key, mapped_type x ) // strong
        {
            this->enforce_null_policy( x, 
                  "Null pointer in 'ptr_multimap_adapter::insert()'" );

            auto_type ptr( x );         // nothrow
            BOOST_DEDUCED_TYPENAME base_type::ptr_iterator
                res = this->c_private().insert( std::make_pair( key, x ) );
                                        // strong, commit        
            ptr.release();              // notrow
            return iterator( res );           
        }

        template< class U >
        iterator insert( key_type& key, std::auto_ptr<U> x )
        {
            return insert( key, x.release() );
        }

        template< class PtrMapAdapter >
        void transfer( BOOST_DEDUCED_TYPENAME PtrMapAdapter::iterator object, 
                       PtrMapAdapter& from ) // strong
        {
            this->multi_transfer( object, from );
        }

        template< class PtrMapAdapter >
        size_type transfer( BOOST_DEDUCED_TYPENAME PtrMapAdapter::iterator first, 
                            BOOST_DEDUCED_TYPENAME PtrMapAdapter::iterator last, 
                            PtrMapAdapter& from ) // basic
        {
            return this->multi_transfer( first, last, from );
        }

#if defined(BOOST_NO_SFINAE) || BOOST_WORKAROUND(__SUNPRO_CC, <= 0x580)        
#else    

        template<  class PtrMapAdapter, class Range >
        BOOST_DEDUCED_TYPENAME boost::disable_if< boost::is_same< Range,
                            BOOST_DEDUCED_TYPENAME PtrMapAdapter::iterator >,
                                                            size_type >::type
        transfer( const Range& r, PtrMapAdapter& from ) // basic
        {
            return transfer( boost::begin(r), boost::end(r), from );
        }

#endif        
        template< class PtrMapAdapter >
        void transfer( PtrMapAdapter& from ) // basic
        {
            transfer( from.begin(), from.end(), from );
            BOOST_ASSERT( from.empty() );
        }

    public: // serialization

        template< class Archive >
        void load( Archive& ar, const unsigned ) // basic
        {
            this->clear();
            size_type n;
            ar & n;

            for( size_type i = 0u; i != n; ++i )
            {
                key_type  key;
                T*        value;
                ar & key;
                ar & value;
                iterator p = this->insert( key, value );
                ar.reset_object_address( &p->first, &key );
            }
        }

        BOOST_SERIALIZATION_SPLIT_MEMBER()

    };

    template< class I, class F, class S >
    inline bool is_null( const ptr_map_iterator<I,F,S>& i )
    {
        return i->second == 0;
    }
    
} // namespace 'boost'  

#endif
