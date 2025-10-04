//
// Copyright (c) 2009-2011 Artyom Beilis (Tonkikh)
// Copyright (c) 2024 Alexander Grund
//
// Distributed under the Boost Software License, Version 1.0.
// https://www.boost.org/LICENSE_1_0.txt

#include <boost/locale/encoding.hpp>
#include <boost/locale/generator.hpp>
#include <boost/locale/localization_backend.hpp>
#include <boost/thread/locks.hpp>
#include <boost/thread/mutex.hpp>
#include <algorithm>
#include <map>
#include <vector>

namespace boost { namespace locale {
    struct generator::data {
        data(const localization_backend_manager& mgr) :
            cats(all_categories), chars(all_characters), caching_enabled(false), use_ansi_encoding(false),
            backend_manager(mgr)
        {}

        mutable std::map<std::string, std::locale> cached;
        mutable boost::mutex cached_lock;

        category_t cats;
        char_facet_t chars;

        bool caching_enabled;
        bool use_ansi_encoding;

        std::vector<std::string> paths;
        std::vector<std::string> domains;

        std::map<std::string, std::vector<std::string>> options;

        localization_backend_manager backend_manager;
    };

    generator::generator(const localization_backend_manager& mgr) : d(new generator::data(mgr)) {}
    generator::generator() : d(new generator::data(localization_backend_manager::global())) {}
    generator::~generator() = default;

    category_t generator::categories() const
    {
        return d->cats;
    }
    void generator::categories(category_t t)
    {
        d->cats = t;
    }

    void generator::characters(char_facet_t t)
    {
        d->chars = t;
    }

    char_facet_t generator::characters() const
    {
        return d->chars;
    }

    void generator::add_messages_domain(const std::string& domain)
    {
        if(std::find(d->domains.begin(), d->domains.end(), domain) == d->domains.end())
            d->domains.push_back(domain);
    }

    void generator::set_default_messages_domain(const std::string& domain)
    {
        const auto p = std::find(d->domains.begin(), d->domains.end(), domain);
        if(p != d->domains.end())
            d->domains.erase(p);
        d->domains.insert(d->domains.begin(), domain);
    }

    void generator::clear_domains()
    {
        d->domains.clear();
    }
    void generator::add_messages_path(const std::string& path)
    {
        d->paths.push_back(path);
    }
    void generator::clear_paths()
    {
        d->paths.clear();
    }
    void generator::clear_cache()
    {
        d->cached.clear();
    }

    std::locale generator::generate(const std::string& id) const
    {
        return generate(std::locale::classic(), id);
    }

    std::locale generator::generate(const std::locale& base, const std::string& id) const
    {
        if(d->caching_enabled) {
            boost::unique_lock<boost::mutex> guard(d->cached_lock);
            const auto p = d->cached.find(id);
            if(p != d->cached.end())
                return p->second;
        }
        auto backend = d->backend_manager.create();
        set_all_options(*backend, id);

        std::locale result = base;
        const category_t facets = d->cats;
        const char_facet_t chars = d->chars;

        for(category_t facet = per_character_facet_first; facet <= per_character_facet_last; ++facet) {
            if(!(facets & facet))
                continue;
            for(char_facet_t ch = character_facet_first; ch <= character_facet_last; ++ch) {
                if(ch & chars)
                    result = backend->install(result, facet, ch);
            }
        }
        for(category_t facet = non_character_facet_first; facet <= non_character_facet_last; ++facet) {
            if(facets & facet)
                result = backend->install(result, facet, char_facet_t::nochar);
        }
        if(d->caching_enabled) {
            boost::unique_lock<boost::mutex> guard(d->cached_lock);
            const auto p = d->cached.find(id);
            if(p == d->cached.end())
                d->cached[id] = result;
        }
        return result;
    }

    bool generator::use_ansi_encoding() const
    {
        return d->use_ansi_encoding;
    }

    void generator::use_ansi_encoding(bool v)
    {
        d->use_ansi_encoding = v;
    }

    bool generator::locale_cache_enabled() const
    {
        return d->caching_enabled;
    }
    void generator::locale_cache_enabled(bool enabled)
    {
        d->caching_enabled = enabled;
    }

    void generator::set_all_options(localization_backend& backend, const std::string& id) const
    {
        backend.set_option("locale", id);
        backend.set_option("use_ansi_encoding", d->use_ansi_encoding ? "true" : "false");
        for(const std::string& domain : d->domains)
            backend.set_option("message_application", domain);
        for(const std::string& path : d->paths)
            backend.set_option("message_path", path);
    }

    // Sanity check

    static_assert((char_facet_t::char_f | char_facet_t::wchar_f) & char_facet_t::char_f, "!");
    static_assert((char_facet_t::char_f | char_facet_t::wchar_f) & char_facet_t::wchar_f, "!");
    static_assert(!((all_characters ^ char_facet_t::wchar_f) & char_facet_t::wchar_f), "!");

    static_assert((category_t::calendar | category_t::convert) & category_t::calendar, "!");
    static_assert((category_t::calendar | category_t::convert) & category_t::convert, "!");
    static_assert(!((all_categories ^ category_t::calendar) & category_t::calendar), "!");

#ifndef BOOST_NO_CXX14_CONSTEXPR
    template<typename T>
    constexpr T inc_enum(T v)
    {
        return ++v;
    }
    static_assert(inc_enum(char_facet_t::nochar) == char_facet_t::char_f, "!");
    static_assert(inc_enum(char_facet_t::char_f) == char_facet_t::wchar_f, "!");
    static_assert(inc_enum(category_t::convert) == category_t::collation, "!");
#endif

}} // namespace boost::locale
