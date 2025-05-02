//
// Copyright (c) 2009-2011 Artyom Beilis (Tonkikh)
//
// Distributed under the Boost Software License, Version 1.0.
// https://www.boost.org/LICENSE_1_0.txt

#ifndef BOOST_LOCALE_BOUNDARY_FACETS_HPP_INCLUDED
#define BOOST_LOCALE_BOUNDARY_FACETS_HPP_INCLUDED

#include <boost/locale/boundary/types.hpp>
#include <boost/locale/detail/facet_id.hpp>
#include <boost/locale/detail/is_supported_char.hpp>
#include <locale>
#include <vector>

#ifdef BOOST_MSVC
#    pragma warning(push)
#    pragma warning(disable : 4275 4251 4231 4660)
#endif

namespace boost { namespace locale {

    /// \brief This namespace contains all operations required for boundary analysis of text
    namespace boundary {
        /// \addtogroup boundary
        ///
        /// @{

        /// \brief This structure is used for representing boundary points
        /// that follow the offset.
        struct break_info {
            /// Create empty break point at beginning
            break_info() : offset(0), rule(0) {}

            /// Create an empty break point at offset v.
            /// it is useful for order comparison with other points.
            break_info(size_t v) : offset(v), rule(0) {}

            /// Offset from the beginning of the text where a break occurs.
            size_t offset;
            /// The identification of this break point according to
            /// various break types
            rule_type rule;

            /// Compare two break points' offset. Allows to search with
            /// standard algorithms over the index.
            bool operator<(const break_info& other) const { return offset < other.offset; }
        };

        /// This type holds the analysis of the text - all its break points
        /// with marks
        typedef std::vector<break_info> index_type;

        /// \brief This facet generates an index for boundary analysis of a given text.
        ///
        /// It is implemented for supported character types, at least \c char, \c wchar_t
        template<typename Char>
        class BOOST_SYMBOL_VISIBLE boundary_indexing : public std::locale::facet,
                                                       public boost::locale::detail::facet_id<boundary_indexing<Char>> {
            BOOST_LOCALE_ASSERT_IS_SUPPORTED(Char);

        public:
            /// Default constructor typical for facets
            boundary_indexing(size_t refs = 0) : std::locale::facet(refs) {}

            /// Create index for boundary type \a t for text in range [begin,end)
            ///
            /// The returned value is an index of type \ref index_type. Note that this
            /// index is never empty, even if the range [begin,end) is empty it consists
            /// of at least one boundary point with the offset 0.
            virtual index_type map(boundary_type t, const Char* begin, const Char* end) const = 0;
        };

        /// @}
    } // namespace boundary

}} // namespace boost::locale

#ifdef BOOST_MSVC
#    pragma warning(pop)
#endif

#endif
