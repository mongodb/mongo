//
// Copyright (c) 2022-2023 Alexander Grund
//
// Distributed under the Boost Software License, Version 1.0.
// https://www.boost.org/LICENSE_1_0.txt

#ifndef BOOST_LOCALE_DETAIL_FACET_ID_HPP_INCLUDED
#define BOOST_LOCALE_DETAIL_FACET_ID_HPP_INCLUDED

#include <boost/locale/config.hpp>
#include <locale>

/// \cond INTERNAL
namespace boost { namespace locale { namespace detail {
#if BOOST_CLANG_VERSION >= 40900
#    pragma clang diagnostic push
#    pragma clang diagnostic ignored "-Wundefined-var-template"
#endif
    /// CRTP base class to hold the id required for facets
    ///
    /// Required because the id needs to be defined in a CPP file and hence ex/imported for shared libraries.
    /// However the virtual classes need to be declared as BOOST_VISIBLE to combine the VTables because otherwise
    /// casts/virtual-calls might be flagged as invalid by UBSAN
    template<class Derived>
    struct BOOST_LOCALE_DECL facet_id {
        static std::locale::id id;
    };
#if BOOST_CLANG_VERSION >= 40900
#    pragma clang diagnostic pop
#endif
}}} // namespace boost::locale::detail

/// \endcond

#endif
