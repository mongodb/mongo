// Boost.Range library
//
//  Copyright Thorsten Ottosen & Pavol Droba 2003-2004. Use, modification and
//  distribution is subject to the Boost Software License, Version
//  1.0. (See accompanying file LICENSE_1_0.txt or copy at
//  http://www.boost.org/LICENSE_1_0.txt)
//
// For more information, see http://www.boost.org/libs/range/
//

#ifndef BOOST_RANGE_ITERATOR_RANGE_HPP
#define BOOST_RANGE_ITERATOR_RANGE_HPP

// From boost/dynamic_bitset.hpp; thanks to Matthias Troyer for Cray X1 patch.
#include <boost/config.hpp> // Define __STL_CONFIG_H, if appropriate.
#ifndef BOOST_OLD_IOSTREAMS 
# if defined(__STL_CONFIG_H) && \
    !defined (__STL_USE_NEW_IOSTREAMS) && !defined(__crayx1) \
    /**/
#  define BOOST_OLD_IOSTREAMS
# endif
#endif // #ifndef BOOST_OLD_IOSTREAMS

#include <boost/detail/workaround.hpp>
#include <boost/range/functions.hpp>
#include <boost/range/result_iterator.hpp>
#include <boost/range/difference_type.hpp>
#include <boost/iterator/iterator_traits.hpp>    
#include <boost/assert.hpp>
#include <iterator>
#include <algorithm>
#ifndef BOOST_OLD_IOSTREAMS
# include <ostream>
#else
# include <ostream.h>
#endif
#include <cstddef>


/*! \file
    Defines the \c iterator_class and related functions. 
    \c iterator_range is a simple wrapper of iterator pair idiom. It provides
    a rich subset of Container interface.
*/


namespace boost 
{
    namespace iterator_range_detail
    {
        //
        // The functions adl_begin and adl_end are implemented in a separate
        // class for gcc-2.9x
        //
        template<typename IteratorT>
        struct iterator_range_impl {
            template< class ForwardRange >
            static IteratorT adl_begin( ForwardRange& r )
            {
                return IteratorT( boost::begin( r ) );
            }
            
            template< class ForwardRange >
            static IteratorT adl_end( ForwardRange& r )
            {
                return IteratorT( boost::end( r ) );
            }
        };
 
        template< class Left, class Right >
        inline bool equal( const Left& l, const Right& r )
        {
            typedef BOOST_DEDUCED_TYPENAME boost::range_size<Left>::type sz_type;

            sz_type l_size = boost::size( l ),
                    r_size = boost::size( r );

            if( l_size != r_size )
                return false;

            return std::equal( boost::begin(l), boost::end(l), 
                               boost::begin(r) );                
        }

        template< class Left, class Right >
        inline bool less_than( const Left& l, const Right& r )
        {                
            return std::lexicographical_compare( boost::begin(l), 
                                                 boost::end(l), 
                                                 boost::begin(r), 
                                                 boost::end(r) );                
        }
           
        struct range_tag { };
        struct const_range_tag { };

    }

//  iterator range template class -----------------------------------------//

        //! iterator_range class
        /*!
            An \c iterator_range delimits a range in a sequence by beginning and ending iterators. 
            An iterator_range can be passed to an algorithm which requires a sequence as an input. 
            For example, the \c toupper() function may be used most frequently on strings, 
            but can also be used on iterator_ranges: 
            
            \code
                boost::tolower( find( s, "UPPERCASE STRING" ) );
            \endcode

            Many algorithms working with sequences take a pair of iterators, 
            delimiting a working range, as an arguments. The \c iterator_range class is an 
            encapsulation of a range identified by a pair of iterators. 
            It provides a collection interface, 
            so it is possible to pass an instance to an algorithm requiring a collection as an input. 
        */
        template<typename IteratorT> 
        class iterator_range
        {
        protected: // Used by sub_range
            //! implementation class
            typedef iterator_range_detail::iterator_range_impl<IteratorT> impl;
        public:

            //! this type
            typedef iterator_range<IteratorT> type;
            //BOOST_BROKEN_COMPILER_TYPE_TRAITS_SPECIALIZATION(value_type);
        
            //! Encapsulated value type
            typedef BOOST_DEDUCED_TYPENAME 
                iterator_value<IteratorT>::type value_type;

            //! Difference type
            typedef BOOST_DEDUCED_TYPENAME 
                iterator_difference<IteratorT>::type difference_type;
            
            //! Size type
            typedef std::size_t size_type; // note: must be unsigned

            //! This type
            typedef iterator_range<IteratorT> this_type;

            //! Refence type
            //
            // Needed because value-type is the same for 
            // const and non-const iterators
            //
            typedef BOOST_DEDUCED_TYPENAME
                iterator_reference<IteratorT>::type reference;
            
            //! const_iterator type
            /*! 
                There is no distinction between const_iterator and iterator.
                These typedefs are provides to fulfill container interface
            */ 
            typedef IteratorT const_iterator;
            //! iterator type
            typedef IteratorT iterator;

            iterator_range() : m_Begin( iterator() ), m_End( iterator() ), 
                               singular( true )
            { }
/*
#if BOOST_WORKAROUND(__BORLANDC__, BOOST_TESTED_AT(0x564))  
            iterator_range( this_type r ) :
            : m_Begin(r.begin()), m_End(r.end())
            { }

            this_type& operator=( this_type r )
            {
                m_Begin = r.begin();
                m_End   = r.end();
                return *this;
            }
#endif
*/            
            //! Constructor from a pair of iterators
            template< class Iterator >
            iterator_range( Iterator Begin, Iterator End ) : 
                m_Begin(Begin), m_End(End), singular(false) {}

            //! Constructor from a Range
            template< class Range >
            iterator_range( const Range& r ) : 
                m_Begin( impl::adl_begin( r ) ), m_End( impl::adl_end( r ) ), 
                singular(false) {}

            //! Constructor from a Range
            template< class Range >
            iterator_range( Range& r ) : 
                m_Begin( impl::adl_begin( r ) ), m_End( impl::adl_end( r ) ), 
                singular(false) {}

            //! Constructor from a Range
            template< class Range >
            iterator_range( const Range& r, iterator_range_detail::const_range_tag ) : 
                m_Begin( impl::adl_begin( r ) ), m_End( impl::adl_end( r ) ), 
                singular(false) {}

            //! Constructor from a Range
            template< class Range >
            iterator_range( Range& r, iterator_range_detail::range_tag ) : 
                m_Begin( impl::adl_begin( r ) ), m_End( impl::adl_end( r ) ), 
                singular(false) {}

            #if !BOOST_WORKAROUND(BOOST_MSVC, < 1300)
            this_type& operator=( const this_type& r )    
            {
                m_Begin  = r.begin(); 
                m_End    = r.end();
                //
                // remark: this need not necessarily be true, but it does no harm
                //
                singular = r.singular;
                return *this;
            }
            #endif
                
            template< class Iterator >
            iterator_range& operator=( const iterator_range<Iterator>& r )    
            {
                m_Begin  = r.begin(); 
                m_End    = r.end();
                //
                // remark: this need not necessarily be true, but it does no harm
                //
                singular = r.empty();
                return *this;
            }
                                      
            template< class ForwardRange >
            iterator_range& operator=( ForwardRange& r )
            {
                m_Begin  = impl::adl_begin( r ); 
                m_End    = impl::adl_end( r );
                singular = false;
                return *this;
            }

            template< class ForwardRange >
            iterator_range& operator=( const ForwardRange& r )
            {
                m_Begin  = impl::adl_begin( r ); 
                m_End    = impl::adl_end( r );
                singular = false;
                return *this;
            }

            IteratorT begin() const 
            { 
                return m_Begin; 
            }

            IteratorT end() const 
            { 
                return m_End; 
            } 

            size_type size() const
            { 
                if( singular )
                    return 0;

#if BOOST_WORKAROUND(__BORLANDC__, BOOST_TESTED_AT(0x582))
                return std::distance<IteratorT>( m_Begin, m_End );
#else                
                return std::distance( m_Begin, m_End );
#endif                
            }
            
            bool empty() const
            {
                if( singular )
                    return true;
                
                return m_Begin == m_End;
            }

#if BOOST_WORKAROUND(__BORLANDC__, BOOST_TESTED_AT(0x564))  
            operator bool() const
            {
                return !empty();
            }                                    
#else            
            typedef iterator (iterator_range::*unspecified_bool_type) () const;
            operator unspecified_bool_type() const
            {
                return empty() ? 0: &iterator_range::end;
            }
#endif

            bool equal( const iterator_range& r ) const
            {
                return singular == r.singular && m_Begin == r.m_Begin && m_End == r.m_End;
            }


#ifdef BOOST_NO_FUNCTION_TEMPLATE_ORDERING

            bool operator==( const iterator_range& r ) const
            {
                return iterator_range_detail::equal( *this, r );
            }

            bool operator!=( const iterator_range& r ) const
            {
                return !operator==(r);
            }

           bool operator<( const iterator_range& r ) const
           {
                return iterator_range_detail::less_than( *this, r );
           }

#endif            

        public: // convenience
           reference front() const
           {
               BOOST_ASSERT( !empty() );
               return *m_Begin;
           }
    
           reference back() const
           {
               BOOST_ASSERT( !empty() );
               IteratorT last( m_End );
               return *--last;
           }
    
           reference operator[]( size_type sz ) const
           {
               //BOOST_STATIC_ASSERT( is_random_access );
               BOOST_ASSERT( sz < size() );
               return m_Begin[sz];
           }

           iterator_range& advance_begin( difference_type n )
           {
               std::advance( m_Begin, n );
               return *this;
           }
           
           iterator_range& advance_end( difference_type n )
           {
               std::advance( m_End, n );
               return *this;
           }
           
        private:
            // begin and end iterators
            IteratorT m_Begin;
            IteratorT m_End;
            bool      singular;
        };

//  iterator range free-standing operators ---------------------------//

#ifdef BOOST_NO_FUNCTION_TEMPLATE_ORDERING
#else
        template< class Iterator >
        inline bool empty( const iterator_range<Iterator>& r )
        {
            //
            // this will preserve the well-defined empty() even 
            // though 'r' is singular.
            //
            return r.empty();
        }
#endif

#ifndef BOOST_OLD_IOSTREAMS   

        //! iterator_range output operator
        /*!
            Output the range to an ostream. Elements are outputed
            in a sequence without separators.
        */
        template< typename IteratorT, typename Elem, typename Traits >
        inline std::basic_ostream<Elem,Traits>& operator<<( 
                    std::basic_ostream<Elem, Traits>& Os,
                    const iterator_range<IteratorT>& r )
        {
            std::copy( r.begin(), r.end(), 
                       std::ostream_iterator< BOOST_DEDUCED_TYPENAME 
                                            iterator_value<IteratorT>::type, 
                                              Elem, Traits>(Os) );
            return Os;
        }

#else

        //! iterator_range output operator
        /*!
            Output the range to an ostream. Elements are outputed
            in a sequence without separators.
        */
        template< typename IteratorT >
        inline std::ostream& operator<<( 
                    std::ostream& Os,
                    const iterator_range<IteratorT>& r )
        {
            std::copy( r.begin(), r.end(), std::ostream_iterator<char>(Os));
            return Os;
        }

#endif

        /////////////////////////////////////////////////////////////////////
        // comparison operators
        /////////////////////////////////////////////////////////////////////

        template< class IteratorT, class ForwardRange >
        inline bool operator==( const ForwardRange& l, 
                                const iterator_range<IteratorT>& r )
        {
            return iterator_range_detail::equal( l, r );
        }

        template< class IteratorT, class ForwardRange >
        inline bool operator!=( const ForwardRange& l, 
                                const iterator_range<IteratorT>& r )
        {
            return !iterator_range_detail::equal( l, r );
        }

        template< class IteratorT, class ForwardRange >
        inline bool operator<( const ForwardRange& l, 
                               const iterator_range<IteratorT>& r )
        {
            return iterator_range_detail::less_than( l, r );
        }

#ifdef BOOST_NO_FUNCTION_TEMPLATE_ORDERING
#else
        template< class Iterator1T, class Iterator2T >
        inline bool operator==( const iterator_range<Iterator1T>& l, 
                                const iterator_range<Iterator2T>& r )
        {
            return iterator_range_detail::equal( l, r );
        }

        template< class IteratorT, class ForwardRange >
        inline bool operator==( const iterator_range<IteratorT>& l, 
                                const ForwardRange& r )
        {
            return iterator_range_detail::equal( l, r );
        }


        template< class Iterator1T, class Iterator2T >
        inline bool operator!=( const iterator_range<Iterator1T>& l, 
                                const iterator_range<Iterator2T>& r )
        {
            return !iterator_range_detail::equal( l, r );
        }
        
        template< class IteratorT, class ForwardRange >
        inline bool operator!=( const iterator_range<IteratorT>& l, 
                                const ForwardRange& r )
        {
            return !iterator_range_detail::equal( l, r );
        }

        
        template< class Iterator1T, class Iterator2T >
        inline bool operator<( const iterator_range<Iterator1T>& l, 
                               const iterator_range<Iterator2T>& r )
        {
            return iterator_range_detail::less_than( l, r );
        }

        template< class IteratorT, class ForwardRange >
        inline bool operator<( const iterator_range<IteratorT>& l, 
                               const ForwardRange& r )
        {            
            return iterator_range_detail::less_than( l, r );
        }

#endif // BOOST_NO_FUNCTION_TEMPLATE_ORDERING
                    
//  iterator range utilities -----------------------------------------//

        //! iterator_range construct helper 
        /*!
            Construct an \c iterator_range from a pair of iterators

            \param Begin A begin iterator
            \param End An end iterator
            \return iterator_range object
        */
        template< typename IteratorT >
        inline iterator_range< IteratorT > 
        make_iterator_range( IteratorT Begin, IteratorT End ) 
        {   
            return iterator_range<IteratorT>( Begin, End );
        }
                     
#ifdef BOOST_NO_FUNCTION_TEMPLATE_ORDERING

        template< typename Range >
        inline iterator_range< BOOST_DEDUCED_TYPENAME range_result_iterator<Range>::type >
        make_iterator_range( Range& r ) 
        {   
            return iterator_range< BOOST_DEDUCED_TYPENAME range_result_iterator<Range>::type >
                ( boost::begin( r ), boost::end( r ) );
        }
        
#else
        //! iterator_range construct helper
        /*!
            Construct an \c iterator_range from a \c Range containing the begin
            and end iterators.
        */
        template< class ForwardRange >
        inline iterator_range< BOOST_DEDUCED_TYPENAME range_iterator<ForwardRange>::type >
        make_iterator_range( ForwardRange& r ) 
        {   
           return iterator_range< BOOST_DEDUCED_TYPENAME range_iterator<ForwardRange>::type >
                ( r, iterator_range_detail::range_tag() );
        }

        template< class ForwardRange >
        inline iterator_range< BOOST_DEDUCED_TYPENAME range_const_iterator<ForwardRange>::type >
        make_iterator_range( const ForwardRange& r ) 
        {   
           return iterator_range< BOOST_DEDUCED_TYPENAME range_const_iterator<ForwardRange>::type >
                ( r, iterator_range_detail::const_range_tag() );
        }

#endif // BOOST_NO_FUNCTION_TEMPLATE_ORDERING

        namespace iterator_range_detail
        {    
            template< class Range >
            inline iterator_range< BOOST_DEDUCED_TYPENAME range_result_iterator<Range>::type >
            make_range_impl( Range& r, 
                             BOOST_DEDUCED_TYPENAME range_difference<Range>::type advance_begin,
                             BOOST_DEDUCED_TYPENAME range_difference<Range>::type advance_end )
            {
                if( advance_begin == 0 && advance_end == 0 )
                    return make_iterator_range( r );

                BOOST_DEDUCED_TYPENAME range_result_iterator<Range>::type 
                    new_begin = boost::begin( r ),
                    new_end   = boost::end( r );
                std::advance( new_begin, advance_begin );
                std::advance( new_end, advance_end );
                return make_iterator_range( new_begin, new_end );
            }
        }
        
#ifdef BOOST_NO_FUNCTION_TEMPLATE_ORDERING

        template< class Range >
        inline iterator_range< BOOST_DEDUCED_TYPENAME range_result_iterator<Range>::type >
        make_iterator_range( Range& r, 
                    BOOST_DEDUCED_TYPENAME range_difference<Range>::type advance_begin,
                    BOOST_DEDUCED_TYPENAME range_difference<Range>::type advance_end )
        {
            //BOOST_ASSERT( advance_begin - advance_end <= size(r) && "creating invalid range" );
            return iterator_range_detail::make_range_impl( r, advance_begin, advance_end );
        }

#else

        template< class Range >
        inline iterator_range< BOOST_DEDUCED_TYPENAME range_iterator<Range>::type >
        make_iterator_range( Range& r, 
                    BOOST_DEDUCED_TYPENAME range_difference<Range>::type advance_begin,
                    BOOST_DEDUCED_TYPENAME range_difference<Range>::type advance_end )
        {
            //BOOST_ASSERT( advance_begin - advance_end <= size(r) && "creating invalid range" );
            return iterator_range_detail::make_range_impl( r, advance_begin, advance_end );
        }

        template< class Range >
        inline iterator_range< BOOST_DEDUCED_TYPENAME range_const_iterator<Range>::type >
        make_iterator_range( const Range& r, 
                    BOOST_DEDUCED_TYPENAME range_difference<Range>::type advance_begin,
                    BOOST_DEDUCED_TYPENAME range_difference<Range>::type advance_end )
        {
            //BOOST_ASSERT( advance_begin - advance_end <= size(r) && "creating invalid range" );
            return iterator_range_detail::make_range_impl( r, advance_begin, advance_end );
        }

#endif // BOOST_NO_FUNCTION_TEMPLATE_ORDERING

        //! copy a range into a sequence
        /*!
            Construct a new sequence of the specified type from the elements
            in the given range

            \param Range An input range
            \return New sequence
        */
        template< typename SeqT, typename Range >
        inline SeqT copy_range( const Range& r )
        {
            return SeqT( boost::begin( r ), boost::end( r ) );
        }

} // namespace 'boost'

#undef BOOST_OLD_IOSTREAMS

#endif
