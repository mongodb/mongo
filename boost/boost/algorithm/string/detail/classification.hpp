//  Boost string_algo library classification.hpp header file  ---------------------------//

//  Copyright Pavol Droba 2002-2003. Use, modification and
//  distribution is subject to the Boost Software License, Version
//  1.0. (See accompanying file LICENSE_1_0.txt or copy at
//  http://www.boost.org/LICENSE_1_0.txt)

//  See http://www.boost.org for updates, documentation, and revision history.

#ifndef BOOST_STRING_CLASSIFICATION_DETAIL_HPP
#define BOOST_STRING_CLASSIFICATION_DETAIL_HPP

#include <boost/algorithm/string/config.hpp>
#include <algorithm>
#include <functional>
#include <locale>
#include <set>

#include <boost/range/begin.hpp>
#include <boost/range/end.hpp>

#include <boost/algorithm/string/predicate_facade.hpp>
#include <boost/type_traits/remove_const.hpp>

namespace boost {
    namespace algorithm {
        namespace detail {

//  classification functors -----------------------------------------------//

            // is_classified functor
            struct is_classifiedF :
                public predicate_facade<is_classifiedF>
            {
                // Boost.Lambda support
                template <class Args> struct sig { typedef bool type; };

                // Constructor from a locale
                is_classifiedF(std::ctype_base::mask Type, std::locale const & Loc = std::locale()) :
                    m_Type(Type), m_Locale(Loc) {}

                // Operation
                template<typename CharT>
                bool operator()( CharT Ch ) const
                {
                    return std::use_facet< std::ctype<CharT> >(m_Locale).is( m_Type, Ch );
                }

                #if defined(__BORLANDC__) && (__BORLANDC__ >= 0x560) && (__BORLANDC__ <= 0x582) && !defined(_USE_OLD_RW_STL)
                    template<>
                    bool operator()( char const Ch ) const
                    {
                        return std::use_facet< std::ctype<char> >(m_Locale).is( m_Type, Ch );
                    }
                #endif

            private:
                const std::ctype_base::mask m_Type;
                const std::locale m_Locale;
            };

            // is_any_of functor
            /*
                returns true if the value is from the specified set
            */
            template<typename CharT>
            struct is_any_ofF :
                public predicate_facade<is_any_ofF<CharT> >
            {
                // Boost.Lambda support
                template <class Args> struct sig { typedef bool type; };

                // Constructor
                template<typename RangeT>
                is_any_ofF( const RangeT& Range ) :
                    m_Set( begin(Range), end(Range) ) {}

                // Operation
                template<typename Char2T>
                bool operator()( Char2T Ch ) const
                {
                    return m_Set.find(Ch)!=m_Set.end();
                }

            private:
                // set cannot operate on const value-type
                typedef typename remove_const<CharT>::type set_value_type;
                std::set<set_value_type> m_Set;
            };

            // is_from_range functor
            /*
                returns true if the value is from the specified range.
                (i.e. x>=From && x>=To)
            */
            template<typename CharT>
            struct is_from_rangeF :
                public predicate_facade< is_from_rangeF<CharT> >
            {
                // Boost.Lambda support
                template <class Args> struct sig { typedef bool type; };

                // Constructor
                is_from_rangeF( CharT From, CharT To ) : m_From(From), m_To(To) {}

                // Operation
                template<typename Char2T>
                bool operator()( Char2T Ch ) const
                {
                    return ( m_From <= Ch ) && ( Ch <= m_To );
                }

            private:
                CharT m_From;
                CharT m_To;
            };

            // class_and composition predicate
            template<typename Pred1T, typename Pred2T>
            struct pred_andF :
                public predicate_facade< pred_andF<Pred1T,Pred2T> >
            {
            public:

                // Boost.Lambda support
                template <class Args> struct sig { typedef bool type; };

                // Constructor
                pred_andF( Pred1T Pred1, Pred2T Pred2 ) :
                    m_Pred1(Pred1), m_Pred2(Pred2) {}

                // Operation
                template<typename CharT>
                bool operator()( CharT Ch ) const
                {
                    return m_Pred1(Ch) && m_Pred2(Ch);
                }

            private:
                Pred1T m_Pred1;
                Pred2T m_Pred2;
            };

            // class_or composition predicate
            template<typename Pred1T, typename Pred2T>
            struct pred_orF :
                public predicate_facade< pred_orF<Pred1T,Pred2T> >
            {
            public:
                // Boost.Lambda support
                template <class Args> struct sig { typedef bool type; };

                // Constructor
                pred_orF( Pred1T Pred1, Pred2T Pred2 ) :
                    m_Pred1(Pred1), m_Pred2(Pred2) {}

                // Operation
                template<typename CharT>
                bool operator()( CharT Ch ) const
                {
                    return m_Pred1(Ch) || m_Pred2(Ch);
                }

            private:
                Pred1T m_Pred1;
                Pred2T m_Pred2;
            };

            // class_not composition predicate
            template< typename PredT >
            struct pred_notF :
                public predicate_facade< pred_notF<PredT> >
            {
            public:
                // Boost.Lambda support
                template <class Args> struct sig { typedef bool type; };

                // Constructor
                pred_notF( PredT Pred ) : m_Pred(Pred) {}

                // Operation
                template<typename CharT>
                bool operator()( CharT Ch ) const
                {
                    return !m_Pred(Ch);
                }

            private:
                PredT m_Pred;
            };

        } // namespace detail
    } // namespace algorithm
} // namespace boost


#endif  // BOOST_STRING_CLASSIFICATION_DETAIL_HPP
