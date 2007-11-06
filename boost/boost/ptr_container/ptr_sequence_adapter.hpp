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

#ifndef BOOST_ptr_container_PTR_SEQUENCE_ADAPTER_HPP
#define BOOST_ptr_container_PTR_SEQUENCE_ADAPTER_HPP

#if defined(_MSC_VER) && (_MSC_VER >= 1200)
# pragma once
#endif


#include <boost/ptr_container/detail/reversible_ptr_container.hpp>
#include <boost/ptr_container/indirect_fun.hpp>
#include <boost/ptr_container/detail/void_ptr_iterator.hpp>
#include <boost/type_traits/remove_pointer.hpp>
#include <boost/type_traits/is_same.hpp>
#include <boost/type_traits/is_pointer.hpp>
#include <boost/type_traits/is_integral.hpp>
#include <boost/iterator/iterator_categories.hpp>


namespace boost
{   
namespace ptr_container_detail
{


    
    
    template
    < 
        class T, 
        class VoidPtrSeq
    >
    struct sequence_config
    {
        typedef BOOST_DEDUCED_TYPENAME remove_nullable<T>::type
                    U;
        typedef VoidPtrSeq
                    void_container_type;

        typedef BOOST_DEDUCED_TYPENAME VoidPtrSeq::allocator_type
                    allocator_type;
        
        typedef U   value_type;

        typedef void_ptr_iterator<
                        BOOST_DEDUCED_TYPENAME VoidPtrSeq::iterator, U > 
                    iterator;
       
        typedef void_ptr_iterator<
                        BOOST_DEDUCED_TYPENAME VoidPtrSeq::const_iterator, const U >
                    const_iterator;

#ifdef BOOST_NO_SFINAE

        template< class Iter >
        static U* get_pointer( Iter i )
        {
            return static_cast<U*>( *i.base() );
        }
        
#else
        template< class Iter >
        static U* get_pointer( void_ptr_iterator<Iter,U> i )
        {
            return static_cast<U*>( *i.base() );
        }

        template< class Iter >
        static U* get_pointer( Iter i )
        {
            return &*i;
        }
#endif        

#if defined(BOOST_NO_SFINAE) && !BOOST_WORKAROUND(__MWERKS__, <= 0x3003)

        template< class Iter >
        static const U* get_const_pointer( Iter i )
        {
            return static_cast<const U*>( *i.base() );
        }
        
#else // BOOST_NO_SFINAE

#if BOOST_WORKAROUND(__MWERKS__, <= 0x3003)
        template< class Iter >
        static const U* get_const_pointer( void_ptr_iterator<Iter,U> i )
        {
            return static_cast<const U*>( *i.base() );
        }
#else // BOOST_WORKAROUND
        template< class Iter >
        static const U* get_const_pointer( void_ptr_iterator<Iter,const U> i )
        {
            return static_cast<const U*>( *i.base() );
        }
#endif // BOOST_WORKAROUND

        template< class Iter >
        static const U* get_const_pointer( Iter i )
        {
            return &*i;
        }
#endif // BOOST_NO_SFINAE

        BOOST_STATIC_CONSTANT(bool, allow_null = boost::is_nullable<T>::value );
    };
    
} // ptr_container_detail


    template< class Iterator, class T >
    inline bool is_null( void_ptr_iterator<Iterator,T> i )
    {
        return *i.base() == 0;
    }


    
    template
    < 
        class T,
        class VoidPtrSeq, 
        class CloneAllocator = heap_clone_allocator
    >
    class ptr_sequence_adapter : public 
        ptr_container_detail::reversible_ptr_container< ptr_container_detail::sequence_config<T,VoidPtrSeq>, 
                                            CloneAllocator >
    {
        typedef ptr_container_detail::reversible_ptr_container< ptr_container_detail::sequence_config<T,VoidPtrSeq>,
                                                    CloneAllocator >
             base_type;
        
        typedef BOOST_DEDUCED_TYPENAME base_type::scoped_deleter scoped_deleter;

        typedef ptr_sequence_adapter<T,VoidPtrSeq,CloneAllocator>                         
            this_type;
         
    public:
        typedef BOOST_DEDUCED_TYPENAME base_type::value_type  value_type; 
        typedef BOOST_DEDUCED_TYPENAME base_type::reference   reference; 
        typedef BOOST_DEDUCED_TYPENAME base_type::auto_type   auto_type;
         
        BOOST_PTR_CONTAINER_DEFINE_CONSTRUCTORS( ptr_sequence_adapter, 
                                                 base_type )
    
        template< class PtrContainer >
        ptr_sequence_adapter( std::auto_ptr<PtrContainer> clone )
          : base_type( clone )
        { }

        template< class PtrContainer >
        void operator=( std::auto_ptr<PtrContainer> clone )    
        {
            base_type::operator=( clone );
        }

        /////////////////////////////////////////////////////////////
        // modifiers
        /////////////////////////////////////////////////////////////

        void push_back( value_type x )  // strong               
        {
            this->enforce_null_policy( x, "Null pointer in 'push_back()'" );

            auto_type ptr( x );                // notrow
            this->c_private().push_back( x );  // strong, commit
            ptr.release();                     // nothrow
        }

        template< class U >
        void push_back( std::auto_ptr<U> x )
        {
            push_back( x.release() );
        }
        
        void push_front( value_type x )                
        {
            this->enforce_null_policy( x, "Null pointer in 'push_front()'" );

            auto_type ptr( x );                // nothrow
            this->c_private().push_front( x ); // strong, commit
            ptr.release();                     // nothrow
        }

        template< class U >
        void push_front( std::auto_ptr<U> x )
        {
            push_front( x.release() );
        }

        auto_type pop_back()
        {
            BOOST_PTR_CONTAINER_THROW_EXCEPTION( this->empty(), 
                                                 bad_ptr_container_operation,
                                          "'pop_back()' on empty container" );
            auto_type ptr( static_cast<value_type>( 
                         this->c_private().back() ) ); // nothrow
            this->c_private().pop_back();              // nothrow
            return ptr_container_detail::move( ptr );  // nothrow
        }

        auto_type pop_front()
        {
            BOOST_PTR_CONTAINER_THROW_EXCEPTION( this->empty(),
                                                 bad_ptr_container_operation,
                                         "'pop_front()' on empty container" ); 
            auto_type ptr( static_cast<value_type>(
                        this->c_private().front() ) ); // nothrow 
            this->c_private().pop_front();             // nothrow
            return ptr_container_detail::move( ptr ); 
        }
        
        reference front()        
        { 
            BOOST_PTR_CONTAINER_THROW_EXCEPTION( this->empty(), 
                                                 bad_ptr_container_operation,
                                    "accessing 'front()' on empty container" );
            BOOST_ASSERT( !::boost::is_null( this->begin() ) );
            return *this->begin(); 
        }

        const_reference front() const  
        {
            BOOST_PTR_CONTAINER_THROW_EXCEPTION( this->empty(), 
                                                 bad_ptr_container_operation, 
                                   "accessing 'front()' on empty container" );
            BOOST_ASSERT( !::boost::is_null( this->begin() ) );
            return *this->begin(); 
        }

        reference back()
        {
            BOOST_PTR_CONTAINER_THROW_EXCEPTION( this->empty(),
                                                 bad_ptr_container_operation,
                                    "accessing 'back()' on empty container" );
            BOOST_ASSERT( !::boost::is_null( --this->end() ) );
            return *--this->end(); 
        }

        const_reference back() const
        {
            BOOST_PTR_CONTAINER_THROW_EXCEPTION( this->empty(),
                                                 bad_ptr_container_operation,
                                    "accessing 'back()' on empty container" );
            BOOST_ASSERT( !::boost::is_null( --this->end() ) );
            return *--this->end(); 
        }

    public: // deque/vector inerface
        
        reference operator[]( size_type n ) // nothrow 
        {
            BOOST_ASSERT( n < this->size() );
            BOOST_ASSERT( !this->is_null( n ) );
            return *static_cast<value_type>( this->c_private()[n] ); 
        }
        
        const_reference operator[]( size_type n ) const // nothrow  
        { 
            BOOST_ASSERT( n < this->size() ); 
            BOOST_ASSERT( !this->is_null( n ) );
            return *static_cast<value_type>( this->c_private()[n] );
        }
        
        reference at( size_type n )
        {
            BOOST_PTR_CONTAINER_THROW_EXCEPTION( n >= this->size(), bad_index, 
                                                 "'at()' out of bounds" );
            BOOST_ASSERT( !this->is_null( n ) );
            return (*this)[n];
        }
        
        const_reference at( size_type n ) const
        {
            BOOST_PTR_CONTAINER_THROW_EXCEPTION( n >= this->size(), bad_index, 
                                                 "'at()' out of bounds" );
            BOOST_ASSERT( !this->is_null( n ) );
            return (*this)[n]; 
        }
        
    public: // vector interface
        
        size_type capacity() const
        {
            return this->c_private().capacity();
        }
        
        void reserve( size_type n )
        {
            this->c_private().reserve( n ); 
        }

        void reverse()
        {
            this->c_private().reverse(); 
        }

    public: // assign, insert, transfer

        // overhead: 1 heap allocation (very cheap compared to cloning)
        template< class InputIterator >
        void assign( InputIterator first, InputIterator last ) // strong
        { 
            base_type temp( first, last );
            this->swap( temp );
        }

        template< class Range >
        void assign( const Range& r )
        {
            assign( boost::begin(r), boost::end(r ) );
        }

    private:
        template< class I >
        void insert_impl( iterator before, I first, I last, std::input_iterator_tag ) // strong
        {
            ptr_sequence_adapter temp(first,last);  // strong
            transfer( before, temp );               // strong, commit
        }

        template< class I >
        void insert_impl( iterator before, I first, I last, std::forward_iterator_tag ) // strong
        {
            if( first == last ) 
                return;
            scoped_deleter sd( first, last );                // strong
            this->insert_clones_and_release( sd, before );   // strong, commit 
        }

    public:

        using base_type::insert;
        
        template< class InputIterator >
        void insert( iterator before, InputIterator first, InputIterator last ) // strong
        {
            insert_impl( before, first, last, BOOST_DEDUCED_TYPENAME
                         iterator_category<InputIterator>::type() );
        } 

#if defined(BOOST_NO_SFINAE) || BOOST_WORKAROUND(__SUNPRO_CC, <= 0x580)
#else
        template< class Range >
        BOOST_DEDUCED_TYPENAME
        boost::disable_if< ptr_container_detail::is_pointer_or_integral<Range> >::type
        insert( iterator before, const Range& r )
        {
            insert( before, boost::begin(r), boost::end(r) );
        }

#endif

        template< class PtrSeqAdapter >
        void transfer( iterator before, 
                       BOOST_DEDUCED_TYPENAME PtrSeqAdapter::iterator first, 
                       BOOST_DEDUCED_TYPENAME PtrSeqAdapter::iterator last, 
                       PtrSeqAdapter& from ) // strong
        {
            BOOST_ASSERT( (void*)&from != (void*)this );
            if( from.empty() )
                return;
            this->c_private().
                insert( before.base(), 
                        first.base(), last.base() ); // strong
            from.c_private().erase( first.base(),
                                    last.base() );   // nothrow
        }

        template< class PtrSeqAdapter >
        void transfer( iterator before, 
                       BOOST_DEDUCED_TYPENAME PtrSeqAdapter::iterator object, 
                       PtrSeqAdapter& from ) // strong
        {
            BOOST_ASSERT( (void*)&from != (void*)this );
            if( from.empty() )
                return;
            this->c_private().
                insert( before.base(),
                        *object.base() );                 // strong
            from.c_private().erase( object.base() );      // nothrow
        }

#if defined(BOOST_NO_SFINAE) || BOOST_WORKAROUND(__SUNPRO_CC, <= 0x580)
#else
        
        template< class PtrSeqAdapter, class Range >
        BOOST_DEDUCED_TYPENAME boost::disable_if< boost::is_same< Range,
                      BOOST_DEDUCED_TYPENAME PtrSeqAdapter::iterator > >::type
        transfer( iterator before, const Range& r, PtrSeqAdapter& from ) // strong
        {
            transfer( before, boost::begin(r), boost::end(r), from );
        }

#endif
        template< class PtrSeqAdapter >
        void transfer( iterator before, PtrSeqAdapter& from ) // strong
        {
            BOOST_ASSERT( (void*)&from != (void*)this );
            if( from.empty() )
                return;
            this->c_private().
                insert( before.base(),
                        from.begin().base(), from.end().base() ); // strong
            from.c_private().clear();                             // nothrow
        }

    public: // null functions
         
        bool is_null( size_type idx ) const
        {
            BOOST_ASSERT( idx < this->size() );
            return this->c_private()[idx] == 0;
        }

    public: // algorithms

        void sort( iterator first, iterator last )
        {
            sort( first, last, std::less<T>() );
        }
        
        void sort()
        {
            sort( this->begin(), this->end() );
        }

        template< class Compare >
        void sort( iterator first, iterator last, Compare comp )
        {
            BOOST_ASSERT( first <= last && "out of range sort()" );
            BOOST_ASSERT( this->begin() <= first && "out of range sort()" );
            BOOST_ASSERT( last <= this->end() && "out of range sort()" ); 
            // some static assert on the arguments of the comparison
            std::sort( first.base(), last.base(), 
                       void_ptr_indirect_fun<Compare,T>(comp) );
        }
        
        template< class Compare >
        void sort( Compare comp )
        {
            sort( this->begin(), this->end(), comp );
        }
        
        void unique( iterator first, iterator last )
        {
            unique( first, last, std::equal_to<T>() );
        }
        
        void unique()
        {
            unique( this->begin(), this->end() );
        }

    private:
        struct is_not_zero_ptr
        {
            template< class U >
            bool operator()( const U* r ) const
            {
                return r != 0;
            }
        };

        void compact_and_erase_nulls( iterator first, iterator last ) // nothrow
        {
            
            typename base_type::ptr_iterator p = std::stable_partition( 
                                                    first.base(), 
                                                    last.base(), 
                                                    is_not_zero_ptr() );
            this->c_private().erase( p, this->end().base() );
            
        }

        void range_check_impl( iterator first, iterator last, 
                               std::bidirectional_iterator_tag )
        { /* do nothing */ }

        void range_check_impl( iterator first, iterator last,
                               std::random_access_iterator_tag )
        {
            BOOST_ASSERT( first <= last && "out of range unique()/erase_if()" );
            BOOST_ASSERT( this->begin() <= first && "out of range unique()/erase_if()" );
            BOOST_ASSERT( last <= this->end() && "out of range unique()/erase_if)(" );             
        }
        
        void range_check( iterator first, iterator last )
        {
            range_check_impl( first, last, 
                              BOOST_DEDUCED_TYPENAME iterator_category<iterator>::type() );
        }
        
    public:
        
        template< class Compare >
        void unique( iterator first, iterator last, Compare comp )
        {
            range_check(first,last);
            
            iterator prev = first;
            iterator next = first;
            ++next;
            for( ; next != last; ++next )
            {
                BOOST_ASSERT( !::boost::is_null(prev) );
                BOOST_ASSERT( !::boost::is_null(next) );
                if( comp( *prev, *next ) )
                {
                    this->remove( next ); // delete object
                    *next.base() = 0;     // mark pointer as deleted
                }
                else
                {
                    prev = next;
                }
                // ++next
            }

            compact_and_erase_nulls( first, last );
        }
        
        template< class Compare >
        void unique( Compare comp )
        {
            unique( this->begin(), this->end(), comp );
        }

        template< class Pred >
        void erase_if( iterator first, iterator last, Pred pred )
        {
            range_check(first,last);

            iterator next = first; 
            for( ; next != last; ++next )
            {
                BOOST_ASSERT( !::boost::is_null(next) );
                if( pred( *next ) )
                {
                    this->remove( next ); // delete object
                    *next.base() = 0;     // mark pointer as deleted
                }
            }

            compact_and_erase_nulls( first, last );
        }
        
        template< class Pred >
        void erase_if( Pred pred )
        {
            erase_if( this->begin(), this->end(), pred );
        }


        void merge( iterator first, iterator last, 
                    ptr_sequence_adapter& from )
        {
             merge( first, last, from, std::less<T>() );
        }
        
        template< class BinPred >
        void merge( iterator first, iterator last, 
                    ptr_sequence_adapter& from, BinPred pred )
        {
            void_ptr_indirect_fun<BinPred,T>  bin_pred(pred);
            size_type                         current_size = this->size(); 
            this->transfer( this->end(), first, last, from );
            typename base_type::ptr_iterator middle = this->begin().base();
            std::advance(middle,current_size); 
            std::inplace_merge( this->begin().base(),
                                middle,
                                this->end().base(),
                                bin_pred );
        }
        
        void merge( ptr_sequence_adapter& r )
        {
            merge( r, std::less<T>() );
            BOOST_ASSERT( r.empty() );
        }
        
        template< class BinPred >
        void merge( ptr_sequence_adapter& r, BinPred pred )
        {
            merge( r.begin(), r.end(), r, pred );
            BOOST_ASSERT( r.empty() );    
        }
        
    };


} // namespace 'boost'  

#endif
