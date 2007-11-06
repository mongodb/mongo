//  Boost string_algo library find_format.hpp header file  ---------------------------//

//  Copyright Pavol Droba 2002-2003. Use, modification and
//  distribution is subject to the Boost Software License, Version
//  1.0. (See accompanying file LICENSE_1_0.txt or copy at
//  http://www.boost.org/LICENSE_1_0.txt)

//  See http://www.boost.org for updates, documentation, and revision history.

#ifndef BOOST_STRING_FIND_FORMAT_DETAIL_HPP
#define BOOST_STRING_FIND_FORMAT_DETAIL_HPP

#include <boost/algorithm/string/config.hpp>
#include <boost/range/iterator_range.hpp>
#include <boost/range/const_iterator.hpp>
#include <boost/range/iterator.hpp>
#include <boost/algorithm/string/detail/find_format_store.hpp>
#include <boost/algorithm/string/detail/replace_storage.hpp>

namespace boost {
    namespace algorithm {
        namespace detail {

// find_format_copy (iterator variant) implementation -------------------------------//

            template< 
                typename OutputIteratorT,
                typename InputT,
                typename FormatterT,
                typename FindResultT >
            inline OutputIteratorT find_format_copy_impl(
                OutputIteratorT Output,
                const InputT& Input,
                FormatterT Formatter,
                const FindResultT& FindResult )
            {       
                return find_format_copy_impl2( 
                    Output,
                    Input,
                    Formatter,
                    FindResult,
                    Formatter(FindResult) );
            }

            template< 
                typename OutputIteratorT,
                typename InputT,
                typename FormatterT,
                typename FindResultT,
                typename FormatResultT >
            inline OutputIteratorT find_format_copy_impl2(
                OutputIteratorT Output,
                const InputT& Input,
                FormatterT Formatter,
                const FindResultT& FindResult,
                const FormatResultT& FormatResult )
            {       
                typedef find_format_store<
                    BOOST_STRING_TYPENAME 
                        range_const_iterator<InputT>::type, 
                        FormatterT,
                        FormatResultT > store_type;

                // Create store for the find result
                store_type M( FindResult, FormatResult, Formatter );

                if ( !M )
                {
                    // Match not found - return original sequence
                    std::copy( begin(Input), end(Input), Output );
                    return Output;
                }

                // Copy the beginning of the sequence
                std::copy( begin(Input), begin(M), Output );
                // Format find result
                // Copy formated result
                std::copy( begin(M.format_result()), end(M.format_result()), Output );
                // Copy the rest of the sequence
                std::copy( M.end(), end(Input), Output );

                return Output;
            }

// find_format_copy implementation --------------------------------------------------//

            template< 
                typename InputT, 
                typename FormatterT,
                typename FindResultT >
            inline InputT find_format_copy_impl(
                const InputT& Input,
                FormatterT Formatter,
                const FindResultT& FindResult)
            {
                return find_format_copy_impl2(
                    Input,
                    Formatter,
                    FindResult,
                    Formatter(FindResult) );
            }

            template< 
                typename InputT, 
                typename FormatterT,
                typename FindResultT,
                typename FormatResultT >
            inline InputT find_format_copy_impl2(
                const InputT& Input,
                FormatterT Formatter,
                const FindResultT& FindResult,
                const FormatResultT& FormatResult)
            {
                typedef find_format_store<
                    BOOST_STRING_TYPENAME 
                        range_const_iterator<InputT>::type, 
                        FormatterT,
                        FormatResultT > store_type;

                // Create store for the find result
                store_type M( FindResult, FormatResult, Formatter );

                if ( !M )
                {
                    // Match not found - return original sequence
                    return InputT( Input );
                }

                InputT Output;
                // Copy the beginning of the sequence
                insert( Output, end(Output), begin(Input), M.begin() );
                // Copy formated result
                insert( Output, end(Output), M.format_result() );
                // Copy the rest of the sequence
                insert( Output, end(Output), M.end(), end(Input) );

                return Output;
            }

// replace implementation ----------------------------------------------------//
        
            template<
                typename InputT,
                typename FormatterT,
                typename FindResultT >
            inline void find_format_impl( 
                InputT& Input,
                FormatterT Formatter,
                const FindResultT& FindResult)
            {
                find_format_impl2(
                    Input,
                    Formatter,
                    FindResult,
                    Formatter(FindResult) );
            }

            template<
                typename InputT,
                typename FormatterT,
                typename FindResultT,
                typename FormatResultT >
            inline void find_format_impl2( 
                InputT& Input,
                FormatterT Formatter,
                const FindResultT& FindResult,
                const FormatResultT& FormatResult)
            {
                typedef find_format_store<
                    BOOST_STRING_TYPENAME 
                        range_iterator<InputT>::type, 
                        FormatterT,
                        FormatResultT > store_type;

                // Create store for the find result
                store_type M( FindResult, FormatResult, Formatter );

                if ( !M )
                {
                    // Search not found - return original sequence
                    return;
                }

                // Replace match
                replace( Input, M.begin(), M.end(), M.format_result() );
            }

        } // namespace detail
    } // namespace algorithm
} // namespace boost

#endif  // BOOST_STRING_FIND_FORMAT_DETAIL_HPP
