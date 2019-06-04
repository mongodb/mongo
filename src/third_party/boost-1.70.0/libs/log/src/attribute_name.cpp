/*
 *          Copyright Andrey Semashev 2007 - 2015.
 * Distributed under the Boost Software License, Version 1.0.
 *    (See accompanying file LICENSE_1_0.txt or copy at
 *          http://www.boost.org/LICENSE_1_0.txt)
 */
/*!
 * \file   attribute_name.cpp
 * \author Andrey Semashev
 * \date   28.06.2010
 *
 * \brief  This header is the Boost.Log library implementation, see the library documentation
 *         at http://www.boost.org/doc/libs/release/libs/log/doc/html/index.html.
 */

#include <boost/log/detail/config.hpp>
#include <cstring>
#include <deque>
#include <ostream>
#include <stdexcept>
#include <boost/assert.hpp>
#include <boost/throw_exception.hpp>
#include <boost/smart_ptr/shared_ptr.hpp>
#include <boost/smart_ptr/make_shared_object.hpp>
#include <boost/intrusive/set.hpp>
#include <boost/intrusive/set_hook.hpp>
#include <boost/intrusive/options.hpp>
#include <boost/log/exceptions.hpp>
#include <boost/log/detail/singleton.hpp>
#include <boost/log/attributes/attribute_name.hpp>
#if !defined(BOOST_LOG_NO_THREADS)
#include <boost/log/detail/locks.hpp>
#include <boost/log/detail/light_rw_mutex.hpp>
#endif
#include <boost/log/detail/header.hpp>

namespace boost {

BOOST_LOG_OPEN_NAMESPACE

//! A global container of all known attribute names
class attribute_name::repository :
    public log::aux::lazy_singleton<
        repository,
        shared_ptr< repository >
    >
{
    typedef log::aux::lazy_singleton<
        repository,
        shared_ptr< repository >
    > base_type;

#if !defined(BOOST_LOG_BROKEN_FRIEND_TEMPLATE_SPECIALIZATIONS)
    friend class log::aux::lazy_singleton<
        repository,
        shared_ptr< repository >
    >;
#else
    friend class base_type;
#endif

public:
    //  Import types from the basic_attribute_name template
    typedef attribute_name::id_type id_type;
    typedef attribute_name::string_type string_type;

    //! A base hook for arranging the attribute names into a set
    typedef intrusive::set_base_hook<
        intrusive::link_mode< intrusive::safe_link >,
        intrusive::optimize_size< true >
    > node_by_name_hook;

private:
    //! An element of the attribute names repository
    struct node :
        public node_by_name_hook
    {
        typedef node_by_name_hook base_type;

    public:
        //! A predicate for name-based ordering
        struct order_by_name
        {
            typedef bool result_type;

            bool operator() (node const& left, node const& right) const
            {
                return std::strcmp(left.m_name.c_str(), right.m_name.c_str()) < 0;
            }
            bool operator() (node const& left, const char* right) const
            {
                return std::strcmp(left.m_name.c_str(), right) < 0;
            }
            bool operator() (const char* left, node const& right) const
            {
                return std::strcmp(left, right.m_name.c_str()) < 0;
            }
        };

    public:
        id_type m_id;
        string_type m_name;

    public:
        node() : m_id(0), m_name() {}
        node(id_type i, string_type const& n) :
            base_type(),
            m_id(i),
            m_name(n)
        {
        }
        node(node const& that) :
            base_type(),
            m_id(that.m_id),
            m_name(that.m_name)
        {
        }
    };

    //! The container that provides storage for nodes
    typedef std::deque< node > node_list;
    //! The container that provides name-based lookup
    typedef intrusive::set<
        node,
        intrusive::base_hook< node_by_name_hook >,
        intrusive::constant_time_size< false >,
        intrusive::compare< node::order_by_name >
    > node_set;

private:
#if !defined(BOOST_LOG_NO_THREADS)
    typedef log::aux::light_rw_mutex mutex_type;
    log::aux::light_rw_mutex m_Mutex;
#endif
    node_list m_NodeList;
    node_set m_NodeSet;

public:
    //! Converts attribute name string to id
    id_type get_id_from_string(const char* name)
    {
        BOOST_ASSERT(name != NULL);

#if !defined(BOOST_LOG_NO_THREADS)
        {
            // Do a non-blocking lookup first
            log::aux::shared_lock_guard< mutex_type > _(m_Mutex);
            node_set::const_iterator it =
                m_NodeSet.find(name, node::order_by_name());
            if (it != m_NodeSet.end())
                return it->m_id;
        }
#endif // !defined(BOOST_LOG_NO_THREADS)

        BOOST_LOG_EXPR_IF_MT(log::aux::exclusive_lock_guard< mutex_type > _(m_Mutex);)
        node_set::iterator it =
            m_NodeSet.lower_bound(name, node::order_by_name());
        if (it == m_NodeSet.end() || it->m_name != name)
        {
            const std::size_t new_id = m_NodeList.size();
            if (new_id >= static_cast< id_type >(attribute_name::uninitialized))
                BOOST_THROW_EXCEPTION(limitation_error("Too many log attribute names"));

            m_NodeList.push_back(node(static_cast< id_type >(new_id), name));
            it = m_NodeSet.insert(it, m_NodeList.back());
        }
        return it->m_id;
    }

    //! Converts id to the attribute name string
    string_type const& get_string_from_id(id_type id)
    {
        BOOST_LOG_EXPR_IF_MT(log::aux::shared_lock_guard< mutex_type > _(m_Mutex);)
        BOOST_ASSERT(id < m_NodeList.size());
        return m_NodeList[id].m_name;
    }

private:
    //! Initializes the singleton instance
    static void init_instance()
    {
        base_type::get_instance() = boost::make_shared< repository >();
    }
};

BOOST_LOG_API attribute_name::id_type
attribute_name::get_id_from_string(const char* name)
{
    return repository::get()->get_id_from_string(name);
}

BOOST_LOG_API attribute_name::string_type const&
attribute_name::get_string_from_id(id_type id)
{
    return repository::get()->get_string_from_id(id);
}

template< typename CharT, typename TraitsT >
BOOST_LOG_API std::basic_ostream< CharT, TraitsT >& operator<< (
    std::basic_ostream< CharT, TraitsT >& strm,
    attribute_name const& name)
{
    if (!!name)
        strm << name.string().c_str();
    else
        strm << "[uninitialized]";
    return strm;
}

//  Explicitly instantiate attribute name implementation
#ifdef BOOST_LOG_USE_CHAR
template BOOST_LOG_API std::basic_ostream< char, std::char_traits< char > >&
    operator<< < char, std::char_traits< char > >(
        std::basic_ostream< char, std::char_traits< char > >& strm,
        attribute_name const& name);
#endif
#ifdef BOOST_LOG_USE_WCHAR_T
template BOOST_LOG_API std::basic_ostream< wchar_t, std::char_traits< wchar_t > >&
    operator<< < wchar_t, std::char_traits< wchar_t > >(
        std::basic_ostream< wchar_t, std::char_traits< wchar_t > >& strm,
        attribute_name const& name);
#endif

BOOST_LOG_CLOSE_NAMESPACE // namespace log

} // namespace boost

#include <boost/log/detail/footer.hpp>
