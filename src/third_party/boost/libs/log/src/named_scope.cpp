/*
 *          Copyright Andrey Semashev 2007 - 2015.
 * Distributed under the Boost Software License, Version 1.0.
 *    (See accompanying file LICENSE_1_0.txt or copy at
 *          http://www.boost.org/LICENSE_1_0.txt)
 */
/*!
 * \file   named_scope.cpp
 * \author Andrey Semashev
 * \date   24.06.2007
 *
 * \brief  This header is the Boost.Log library implementation, see the library documentation
 *         at http://www.boost.org/doc/libs/release/libs/log/doc/html/index.html.
 */

#include <boost/log/detail/config.hpp>
#include <utility>
#include <algorithm>
#include <boost/type_index.hpp>
#include <boost/optional/optional.hpp>
#include <boost/log/attributes/attribute.hpp>
#include <boost/log/attributes/attribute_value.hpp>
#include <boost/log/attributes/named_scope.hpp>
#include <boost/log/utility/type_dispatch/type_dispatcher.hpp>
#include <boost/log/detail/allocator_traits.hpp>
#include <boost/log/detail/singleton.hpp>
#if !defined(BOOST_LOG_NO_THREADS)
#include <boost/thread/tss.hpp>
#endif
#include "unique_ptr.hpp"
#include <boost/log/detail/header.hpp>

namespace boost {

BOOST_LOG_OPEN_NAMESPACE

namespace attributes {

BOOST_LOG_ANONYMOUS_NAMESPACE {

    //! Actual implementation of the named scope list
    class writeable_named_scope_list :
        public named_scope_list
    {
        //! Base type
        typedef named_scope_list base_type;

    public:
        //! Const reference type
        typedef base_type::const_reference const_reference;

    public:
        //! The method pushes the scope to the back of the list
        BOOST_FORCEINLINE void push_back(const_reference entry) BOOST_NOEXCEPT
        {
            aux::named_scope_list_node* top = this->m_RootNode._m_pPrev;
            entry._m_pPrev = top;
            entry._m_pNext = &this->m_RootNode;

            this->m_RootNode._m_pPrev = top->_m_pNext =
                const_cast< aux::named_scope_list_node* >(
                    static_cast< const aux::named_scope_list_node* >(&entry));

            ++this->m_Size;
        }
        //! The method removes the top scope entry from the list
        BOOST_FORCEINLINE void pop_back() BOOST_NOEXCEPT
        {
            aux::named_scope_list_node* top = this->m_RootNode._m_pPrev;
            top->_m_pPrev->_m_pNext = top->_m_pNext;
            top->_m_pNext->_m_pPrev = top->_m_pPrev;
            --this->m_Size;
        }
    };

    //! Named scope attribute value
    class named_scope_value :
        public attribute_value::impl
    {
        //! Scope names stack
        typedef named_scope_list scope_stack;

        //! Pointer to the actual scope value
        scope_stack* m_pValue;
        //! A thread-independent value
        optional< scope_stack > m_DetachedValue;

    public:
        //! Constructor
        explicit named_scope_value(scope_stack* p) : m_pValue(p) {}

        //! The method dispatches the value to the given object. It returns true if the
        //! object was capable to consume the real attribute value type and false otherwise.
        bool dispatch(type_dispatcher& dispatcher) BOOST_OVERRIDE
        {
            type_dispatcher::callback< scope_stack > callback =
                dispatcher.get_callback< scope_stack >();
            if (callback)
            {
                callback(*m_pValue);
                return true;
            }
            else
                return false;
        }

        /*!
         * \return The attribute value type
         */
        typeindex::type_index get_type() const BOOST_OVERRIDE { return typeindex::type_id< scope_stack >(); }

        //! The method is called when the attribute value is passed to another thread (e.g.
        //! in case of asynchronous logging). The value should ensure it properly owns all thread-specific data.
        intrusive_ptr< attribute_value::impl > detach_from_thread() BOOST_OVERRIDE
        {
            if (!m_DetachedValue)
            {
                m_DetachedValue = *m_pValue;
                m_pValue = m_DetachedValue.get_ptr();
            }

            return this;
        }
    };

} // namespace

//! Named scope attribute implementation
struct BOOST_SYMBOL_VISIBLE named_scope::impl :
    public attribute::impl,
    public log::aux::singleton<
        impl,
        intrusive_ptr< impl >
    >
{
    //! Singleton base type
    typedef log::aux::singleton<
        impl,
        intrusive_ptr< impl >
    > singleton_base_type;

    //! Writable scope list type
    typedef writeable_named_scope_list scope_list;

#if !defined(BOOST_LOG_NO_THREADS)
    //! Pointer to the thread-specific scope stack
    thread_specific_ptr< scope_list > pScopes;

#if defined(BOOST_LOG_USE_COMPILER_TLS)
    //! Cached pointer to the thread-specific scope stack
    static BOOST_LOG_TLS scope_list* pScopesCache;
#endif

#else
    //! Pointer to the scope stack
    log::aux::unique_ptr< scope_list > pScopes;
#endif

    //! The method returns current thread scope stack
    scope_list& get_scope_list()
    {
#if defined(BOOST_LOG_USE_COMPILER_TLS)
        scope_list* p = pScopesCache;
#else
        scope_list* p = pScopes.get();
#endif
        if (!p)
        {
            log::aux::unique_ptr< scope_list > pNew(new scope_list());
            pScopes.reset(pNew.get());
#if defined(BOOST_LOG_USE_COMPILER_TLS)
            pScopesCache = p = pNew.release();
#else
            p = pNew.release();
#endif
        }

        return *p;
    }

    //! Instance initializer
    static void init_instance()
    {
        singleton_base_type::get_instance().reset(new impl());
    }

    //! The method returns the actual attribute value. It must not return NULL.
    attribute_value get_value() BOOST_OVERRIDE
    {
        return attribute_value(new named_scope_value(&get_scope_list()));
    }

private:
    impl() {}
};

#if defined(BOOST_LOG_USE_COMPILER_TLS)
//! Cached pointer to the thread-specific scope stack
BOOST_LOG_TLS named_scope::impl::scope_list*
named_scope::impl::pScopesCache = NULL;
#endif // defined(BOOST_LOG_USE_COMPILER_TLS)


//! Copy constructor
BOOST_LOG_API named_scope_list::named_scope_list(named_scope_list const& that) :
    allocator_type(static_cast< allocator_type const& >(that)),
    m_Size(that.size()),
    m_fNeedToDeallocate(!that.empty())
{
    if (m_Size > 0)
    {
        // Copy the container contents
        pointer p = log::aux::allocator_traits< allocator_type >::allocate(*static_cast< allocator_type* >(this), that.size());
        aux::named_scope_list_node* prev = &m_RootNode;
        for (const_iterator src = that.begin(), end = that.end(); src != end; ++src, ++p)
        {
            log::aux::allocator_traits< allocator_type >::construct(*static_cast< allocator_type* >(this), p, *src); // won't throw
            p->_m_pPrev = prev;
            prev->_m_pNext = p;
            prev = p;
        }
        m_RootNode._m_pPrev = prev;
        prev->_m_pNext = &m_RootNode;
    }
}

//! Destructor
BOOST_LOG_API named_scope_list::~named_scope_list()
{
    if (m_fNeedToDeallocate)
    {
        iterator it(m_RootNode._m_pNext);
        iterator end(&m_RootNode);
        while (it != end)
            log::aux::allocator_traits< allocator_type >::destroy(*static_cast< allocator_type* >(this), &*(it++));
        log::aux::allocator_traits< allocator_type >::deallocate(*static_cast< allocator_type* >(this), static_cast< pointer >(m_RootNode._m_pNext), m_Size);
    }
}

//! Swaps two instances of the container
BOOST_LOG_API void named_scope_list::swap(named_scope_list& that)
{
    if (!this->empty())
    {
        if (!that.empty())
        {
            // both containers are not empty
            std::swap(m_RootNode._m_pNext->_m_pPrev, that.m_RootNode._m_pNext->_m_pPrev);
            std::swap(m_RootNode._m_pPrev->_m_pNext, that.m_RootNode._m_pPrev->_m_pNext);
            std::swap(m_RootNode, that.m_RootNode);
            std::swap(m_Size, that.m_Size);
            std::swap(m_fNeedToDeallocate, that.m_fNeedToDeallocate);
        }
        else
        {
            // this is not empty
            m_RootNode._m_pNext->_m_pPrev = m_RootNode._m_pPrev->_m_pNext = &that.m_RootNode;
            that.m_RootNode = m_RootNode;
            m_RootNode._m_pNext = m_RootNode._m_pPrev = &m_RootNode;
            std::swap(m_Size, that.m_Size);
            std::swap(m_fNeedToDeallocate, that.m_fNeedToDeallocate);
        }
    }
    else if (!that.empty())
    {
        // that is not empty
        that.m_RootNode._m_pNext->_m_pPrev = that.m_RootNode._m_pPrev->_m_pNext = &m_RootNode;
        m_RootNode = that.m_RootNode;
        that.m_RootNode._m_pNext = that.m_RootNode._m_pPrev = &that.m_RootNode;
        std::swap(m_Size, that.m_Size);
        std::swap(m_fNeedToDeallocate, that.m_fNeedToDeallocate);
    }
}

//! Constructor
named_scope::named_scope() :
    attribute(impl::instance)
{
}

//! Constructor for casting support
named_scope::named_scope(cast_source const& source) :
    attribute(source.as< impl >())
{
}

//! The method pushes the scope to the stack
void named_scope::push_scope(scope_entry const& entry) BOOST_NOEXCEPT
{
    impl::scope_list& s = impl::instance->get_scope_list();
    s.push_back(entry);
}

//! The method pops the top scope
void named_scope::pop_scope() BOOST_NOEXCEPT
{
    impl::scope_list& s = impl::instance->get_scope_list();
    s.pop_back();
}

//! Returns the current thread's scope stack
named_scope::value_type const& named_scope::get_scopes()
{
    return impl::instance->get_scope_list();
}

} // namespace attributes

BOOST_LOG_CLOSE_NAMESPACE // namespace log

} // namespace boost

#include <boost/log/detail/footer.hpp>
