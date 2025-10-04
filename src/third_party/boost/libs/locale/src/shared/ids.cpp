//
// Copyright (c) 2009-2011 Artyom Beilis (Tonkikh)
// Copyright (c) 2024 Alexander Grund
//
// Distributed under the Boost Software License, Version 1.0.
// https://www.boost.org/LICENSE_1_0.txt

#include <boost/locale/boundary.hpp>
#include <boost/locale/collator.hpp>
#include <boost/locale/conversion.hpp>
#include <boost/locale/date_time_facet.hpp>
#include <boost/locale/info.hpp>
#include <boost/locale/message.hpp>
#include "../util/foreach_char.hpp"
#include <boost/core/ignore_unused.hpp>

namespace boost { namespace locale {
    namespace detail {
        template<class Derived>
        std::locale::id facet_id<Derived>::id;
    } // namespace detail
#define BOOST_LOCALE_DEFINE_ID(CLASS) template struct detail::facet_id<CLASS>

    BOOST_LOCALE_DEFINE_ID(info);
    BOOST_LOCALE_DEFINE_ID(calendar_facet);

#define BOOST_LOCALE_INSTANTIATE(CHARTYPE)            \
    BOOST_LOCALE_DEFINE_ID(collator<CHARTYPE>);       \
    BOOST_LOCALE_DEFINE_ID(converter<CHARTYPE>);      \
    BOOST_LOCALE_DEFINE_ID(message_format<CHARTYPE>); \
    BOOST_LOCALE_DEFINE_ID(boundary::boundary_indexing<CHARTYPE>);

    BOOST_LOCALE_FOREACH_CHAR(BOOST_LOCALE_INSTANTIATE)
#undef BOOST_LOCALE_INSTANTIATE

    namespace {
        // Initialize each facet once to avoid issues where doing so
        // in a multi threaded environment could cause problems (races)
        struct init_all {
            init_all()
            {
                const std::locale& l = std::locale::classic();
#define BOOST_LOCALE_INIT_BY(CHAR) init_by<CHAR>(l);
                BOOST_LOCALE_FOREACH_CHAR(BOOST_LOCALE_INIT_BY)

                init_facet<info>(l);
                init_facet<calendar_facet>(l);
            }
            template<typename Char>
            void init_by(const std::locale& l)
            {
                init_facet<boundary::boundary_indexing<Char>>(l);
                init_facet<collator<Char>>(l);
                init_facet<converter<Char>>(l);
                init_facet<message_format<Char>>(l);
            }
            template<typename Facet>
            void init_facet(const std::locale& l)
            {
                // Use the facet to initialize e.g. their std::locale::id
                ignore_unused(std::has_facet<Facet>(l));
            }
        } facet_initializer;
    } // namespace

}} // namespace boost::locale
