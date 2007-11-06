//  Boost string_algo library string_funct.hpp header file  ---------------------------//

//  Copyright Pavol Droba 2002-2003. Use, modification and
//  distribution is subject to the Boost Software License, Version
//  1.0. (See accompanying file LICENSE_1_0.txt or copy at
//  http://www.boost.org/LICENSE_1_0.txt)

//  See http://www.boost.org for updates, documentation, and revision history.

#ifndef BOOST_STRING_CASE_CONV_DETAIL_HPP
#define BOOST_STRING_CASE_CONV_DETAIL_HPP

#include <boost/algorithm/string/config.hpp>
#include <locale>
#include <functional>

namespace boost {
    namespace algorithm {
        namespace detail {

//  case conversion functors -----------------------------------------------//

            // a tolower functor
            template<typename CharT>
            struct to_lowerF : public std::unary_function<CharT, CharT>
            {
                // Constructor
                to_lowerF( const std::locale& Loc ) : m_Loc( Loc ) {}

                // Operation
                CharT operator ()( CharT Ch ) const
                {
                    #if defined(__BORLANDC__) && (__BORLANDC__ >= 0x560) && (__BORLANDC__ <= 0x564) && !defined(_USE_OLD_RW_STL)
                        return std::tolower( Ch);
                    #else
                        return std::tolower<CharT>( Ch, m_Loc );
                    #endif
                }
            private:
                const std::locale& m_Loc;
            };

            // a toupper functor
            template<typename CharT>
            struct to_upperF : public std::unary_function<CharT, CharT>
            {
                // Constructor
                to_upperF( const std::locale& Loc ) : m_Loc( Loc ) {}

                // Operation
                CharT operator ()( CharT Ch ) const
                {
                    #if defined(__BORLANDC__) && (__BORLANDC__ >= 0x560) && (__BORLANDC__ <= 0x564) && !defined(_USE_OLD_RW_STL)
                        return std::toupper( Ch);
                    #else
                        return std::toupper<CharT>( Ch, m_Loc );
                    #endif
                }
            private:
                const std::locale& m_Loc;
            };

        } // namespace detail
    } // namespace algorithm
} // namespace boost


#endif  // BOOST_STRING_CASE_CONV_DETAIL_HPP
