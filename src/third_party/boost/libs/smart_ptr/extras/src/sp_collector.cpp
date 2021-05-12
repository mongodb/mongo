//
//  sp_collector.cpp
//
//  Copyright (c) 2002, 2003 Peter Dimov
//
// Distributed under the Boost Software License, Version 1.0. (See
// accompanying file LICENSE_1_0.txt or copy at
// http://www.boost.org/LICENSE_1_0.txt)
//

#if defined(BOOST_SP_ENABLE_DEBUG_HOOKS)

#include <boost/assert.hpp>
#include <boost/shared_ptr.hpp>
#include <boost/detail/lightweight_mutex.hpp>
#include <cstdlib>
#include <map>
#include <deque>
#include <iostream>

typedef std::map< void const *, std::pair<void *, size_t> > map_type;

static map_type & get_map()
{
    static map_type m;
    return m;
}

typedef boost::detail::lightweight_mutex mutex_type;

static mutex_type & get_mutex()
{
    static mutex_type m;
    return m;
}

static void * init_mutex_before_main = &get_mutex();

namespace
{
    class X;

    struct count_layout
    {
        boost::detail::sp_counted_base * pi;
        int id;
    };

    struct shared_ptr_layout
    {
        X * px;
        count_layout pn;
    };
}

// assume 4 byte alignment for pointers when scanning
size_t const pointer_align = 4;

typedef std::map<void const *, long> map2_type;

static void scan_and_count(void const * area, size_t size, map_type const & m, map2_type & m2)
{
    unsigned char const * p = static_cast<unsigned char const *>(area);

    for(size_t n = 0; n + sizeof(shared_ptr_layout) <= size; p += pointer_align, n += pointer_align)
    {
        shared_ptr_layout const * q = reinterpret_cast<shared_ptr_layout const *>(p);

        if(q->pn.id == boost::detail::shared_count_id && q->pn.pi != 0 && m.count(q->pn.pi) != 0)
        {
            ++m2[q->pn.pi];
        }
    }
}

typedef std::deque<void const *> open_type;

static void scan_and_mark(void const * area, size_t size, map2_type & m2, open_type & open)
{
    unsigned char const * p = static_cast<unsigned char const *>(area);

    for(size_t n = 0; n + sizeof(shared_ptr_layout) <= size; p += pointer_align, n += pointer_align)
    {
        shared_ptr_layout const * q = reinterpret_cast<shared_ptr_layout const *>(p);

        if(q->pn.id == boost::detail::shared_count_id && q->pn.pi != 0 && m2.count(q->pn.pi) != 0)
        {
            open.push_back(q->pn.pi);
            m2.erase(q->pn.pi);
        }
    }
}

static void find_unreachable_objects_impl(map_type const & m, map2_type & m2)
{
    // scan objects for shared_ptr members, compute internal counts

    {
        std::cout << "... " << m.size() << " objects in m.\n";

        for(map_type::const_iterator i = m.begin(); i != m.end(); ++i)
        {
            boost::detail::sp_counted_base const * p = static_cast<boost::detail::sp_counted_base const *>(i->first);

            BOOST_ASSERT(p->use_count() != 0); // there should be no inactive counts in the map

            m2[ i->first ];

            scan_and_count(i->second.first, i->second.second, m, m2);
        }

        std::cout << "... " << m2.size() << " objects in m2.\n";
    }

    // mark reachable objects

    {
        open_type open;

        for(map2_type::iterator i = m2.begin(); i != m2.end(); ++i)
        {
            boost::detail::sp_counted_base const * p = static_cast<boost::detail::sp_counted_base const *>(i->first);
            if(p->use_count() != i->second) open.push_back(p);
        }

        std::cout << "... " << open.size() << " objects in open.\n";

        for(open_type::iterator j = open.begin(); j != open.end(); ++j)
        {
            m2.erase(*j);
        }

        while(!open.empty())
        {
            void const * p = open.front();
            open.pop_front();

            map_type::const_iterator i = m.find(p);
            BOOST_ASSERT(i != m.end());

            scan_and_mark(i->second.first, i->second.second, m2, open);
        }
    }

    // m2 now contains the unreachable objects
}

std::size_t find_unreachable_objects(bool report)
{
    map2_type m2;

#ifdef BOOST_HAS_THREADS

    // This will work without the #ifdef, but some compilers warn
    // that lock is not referenced

    mutex_type::scoped_lock lock(get_mutex());

#endif

    map_type const & m = get_map();

    find_unreachable_objects_impl(m, m2);

    if(report)
    {
        for(map2_type::iterator j = m2.begin(); j != m2.end(); ++j)
        {
            map_type::const_iterator i = m.find(j->first);
            BOOST_ASSERT(i != m.end());
            std::cout << "Unreachable object at " << i->second.first << ", " << i->second.second << " bytes long.\n";
        }
    }

    return m2.size();
}

typedef std::deque< boost::shared_ptr<X> > free_list_type;

static void scan_and_free(void * area, size_t size, map2_type const & m2, free_list_type & free)
{
    unsigned char * p = static_cast<unsigned char *>(area);

    for(size_t n = 0; n + sizeof(shared_ptr_layout) <= size; p += pointer_align, n += pointer_align)
    {
        shared_ptr_layout * q = reinterpret_cast<shared_ptr_layout *>(p);

        if(q->pn.id == boost::detail::shared_count_id && q->pn.pi != 0 && m2.count(q->pn.pi) != 0 && q->px != 0)
        {
            boost::shared_ptr<X> * ppx = reinterpret_cast< boost::shared_ptr<X> * >(p);
            free.push_back(*ppx);
            ppx->reset();
        }
    }
}

void free_unreachable_objects()
{
    free_list_type free;

    {
        map2_type m2;

#ifdef BOOST_HAS_THREADS

        mutex_type::scoped_lock lock(get_mutex());

#endif

        map_type const & m = get_map();

        find_unreachable_objects_impl(m, m2);

        for(map2_type::iterator j = m2.begin(); j != m2.end(); ++j)
        {
            map_type::const_iterator i = m.find(j->first);
            BOOST_ASSERT(i != m.end());
            scan_and_free(i->second.first, i->second.second, m2, free);
        }
    }

    std::cout << "... about to free " << free.size() << " objects.\n";
}

// debug hooks

namespace boost
{

void sp_scalar_constructor_hook(void *)
{
}

void sp_scalar_constructor_hook(void * px, std::size_t size, void * pn)
{
#ifdef BOOST_HAS_THREADS

    mutex_type::scoped_lock lock(get_mutex());

#endif

    get_map()[pn] = std::make_pair(px, size);
}

void sp_scalar_destructor_hook(void *)
{
}

void sp_scalar_destructor_hook(void *, std::size_t, void * pn)
{
#ifdef BOOST_HAS_THREADS

    mutex_type::scoped_lock lock(get_mutex());

#endif

    get_map().erase(pn);
}

void sp_array_constructor_hook(void *)
{
}

void sp_array_destructor_hook(void *)
{
}

} // namespace boost

#endif // defined(BOOST_SP_ENABLE_DEBUG_HOOKS)
