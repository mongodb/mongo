/*
 *          Copyright Andrey Semashev 2007 - 2015.
 * Distributed under the Boost Software License, Version 1.0.
 *    (See accompanying file LICENSE_1_0.txt or copy at
 *          http://www.boost.org/LICENSE_1_0.txt)
 */
/*!
 * \file   core.cpp
 * \author Andrey Semashev
 * \date   19.04.2007
 *
 * \brief  This header is the Boost.Log library implementation, see the library documentation
 *         at http://www.boost.org/doc/libs/release/libs/log/doc/html/index.html.
 */

#include <boost/log/detail/config.hpp>
#include <cstddef>
#include <new>
#include <vector>
#include <algorithm>
#include <boost/cstdint.hpp>
#include <boost/assert.hpp>
#include <boost/core/swap.hpp>
#include <boost/filesystem/path.hpp>
#include <boost/smart_ptr/weak_ptr.hpp>
#include <boost/smart_ptr/shared_ptr.hpp>
#include <boost/smart_ptr/make_shared_object.hpp>
#include <boost/range/iterator_range_core.hpp>
#include <boost/date_time/posix_time/posix_time_types.hpp>
#include <boost/random/taus88.hpp>
#include <boost/move/core.hpp>
#include <boost/move/utility_core.hpp>
#include <boost/log/core/core.hpp>
#include <boost/log/core/record.hpp>
#include <boost/log/core/record_view.hpp>
#include <boost/log/sinks/sink.hpp>
#include <boost/log/attributes/attribute_value_set.hpp>
#include <boost/log/detail/singleton.hpp>
#if !defined(BOOST_LOG_NO_THREADS)
#include <boost/thread/tss.hpp>
#include <boost/thread/exceptions.hpp>
#include <boost/log/detail/locks.hpp>
#include <boost/log/detail/light_rw_mutex.hpp>
#include <boost/log/detail/thread_id.hpp>
#endif
#include "unique_ptr.hpp"
#include "default_sink.hpp"
#include "stateless_allocator.hpp"
#include "alignment_gap_between.hpp"
#include <boost/log/detail/header.hpp>

namespace boost {

BOOST_LOG_OPEN_NAMESPACE

namespace aux {

BOOST_LOG_ANONYMOUS_NAMESPACE {

//! Sequence shuffling algorithm. Very similar to std::random_shuffle, used for forward portability with compilers that removed it from the standard library (C++17).
template< typename Iterator, typename RandomNumberGenerator >
void random_shuffle(Iterator begin, Iterator end, RandomNumberGenerator& rng)
{
    Iterator it = begin;
    ++it;
    while (it != end)
    {
        Iterator where = begin + rng() % (it - begin + 1u);
        if (where != it)
            boost::swap(*where, *it);
        ++it;
    }
}

} // namespace

} // namespace aux

//! Private record data information, with core-specific structures
struct record_view::private_data :
    public public_data
{
    //! Underlying memory allocator
    typedef boost::log::aux::stateless_allocator< char > stateless_allocator;
    //! Sink pointer type
    typedef weak_ptr< sinks::sink > sink_ptr;
    //! Iterator range with pointers to the accepting sinks
    typedef iterator_range< sink_ptr* > sink_list;

private:
    //! Number of sinks accepting the record
    uint32_t m_accepting_sink_count;
    //! Maximum number of sinks accepting the record
    const uint32_t m_accepting_sink_capacity;
    //! The flag indicates that the record has to be detached from the current thread
    bool m_detach_from_thread_needed;

private:
    //! Initializing constructor
    private_data(BOOST_RV_REF(attribute_value_set) values, uint32_t capacity) BOOST_NOEXCEPT :
        public_data(boost::move(values)),
        m_accepting_sink_count(0),
        m_accepting_sink_capacity(capacity),
        m_detach_from_thread_needed(false)
    {
    }

public:
    //! Creates the object with the specified capacity
    static private_data* create(BOOST_RV_REF(attribute_value_set) values, uint32_t capacity)
    {
        private_data* p = reinterpret_cast< private_data* >(stateless_allocator().allocate
        (
            sizeof(private_data) +
            boost::log::aux::alignment_gap_between< private_data, sink_ptr >::value +
            capacity * sizeof(sink_ptr)
        ));
        new (p) private_data(boost::move(values), capacity);
        return p;
    }

    //! Destroys the object and frees the underlying storage
    void destroy() BOOST_NOEXCEPT
    {
        sink_ptr* psink = begin();
        for (uint32_t i = 0u, n = m_accepting_sink_count; i < n; ++i)
        {
            psink[i].~sink_ptr();
        }

        const uint32_t capacity = m_accepting_sink_capacity;
        this->~private_data();

        stateless_allocator().deallocate
        (
            reinterpret_cast< stateless_allocator::pointer >(this),
            sizeof(private_data) +
                boost::log::aux::alignment_gap_between< private_data, sink_ptr >::value +
                capacity * sizeof(sink_ptr)
        );
    }

    //! Returns iterator range with the pointers to the accepting sinks
    sink_list get_accepting_sinks() BOOST_NOEXCEPT
    {
        sink_ptr* p = begin();
        return sink_list(p, p + m_accepting_sink_count);
    }

    //! Adds an accepting sink
    void push_back_accepting_sink(shared_ptr< sinks::sink > const& sink)
    {
        BOOST_ASSERT(m_accepting_sink_count < m_accepting_sink_capacity);
        sink_ptr* p = begin() + m_accepting_sink_count;
        new (p) sink_ptr(sink);
        ++m_accepting_sink_count;
        m_detach_from_thread_needed |= sink->is_cross_thread();
    }

    //! Returns the number of accepting sinks
    uint32_t accepting_sink_count() const BOOST_NOEXCEPT { return m_accepting_sink_count; }

    //! Returns the flag indicating whether it is needed to detach the record from the current thread
    bool is_detach_from_thread_needed() const BOOST_NOEXCEPT { return m_detach_from_thread_needed; }

    BOOST_DELETED_FUNCTION(private_data(private_data const&))
    BOOST_DELETED_FUNCTION(private_data& operator= (private_data const&))

private:
    //! Returns a pointer to the first accepting sink
    sink_ptr* begin() BOOST_NOEXCEPT
    {
        return reinterpret_cast< sink_ptr* >
        (
            reinterpret_cast< char* >(this) +
                sizeof(private_data) +
                boost::log::aux::alignment_gap_between< private_data, sink_ptr >::value
        );
    }
};

//! Destructor
BOOST_LOG_API void record_view::public_data::destroy(const public_data* p) BOOST_NOEXCEPT
{
    const_cast< private_data* >(static_cast< const private_data* >(p))->destroy();
}

//! The function ensures that the log record does not depend on any thread-specific data.
BOOST_LOG_API record_view record::lock()
{
    BOOST_ASSERT(m_impl != NULL);

    record_view::private_data* const impl = static_cast< record_view::private_data* >(m_impl);
    if (impl->is_detach_from_thread_needed())
    {
        attribute_value_set::const_iterator
            it = impl->m_attribute_values.begin(),
            end = impl->m_attribute_values.end();
        for (; it != end; ++it)
        {
            // Yep, a bit hackish. I'll need a better backdoor to do it gracefully.
            const_cast< attribute_value_set::mapped_type& >(it->second).detach_from_thread();
        }
    }

    // Move the implementation to the view
    m_impl = NULL;
    return record_view(impl);
}

//! Logging system implementation
struct core::implementation :
    public log::aux::lazy_singleton<
        implementation,
        core_ptr
    >
{
public:
    //! Base type of singleton holder
    typedef log::aux::lazy_singleton<
        implementation,
        core_ptr
    > base_type;

#if !defined(BOOST_LOG_NO_THREADS)
    //! Read lock type
    typedef log::aux::shared_lock_guard< log::aux::light_rw_mutex > scoped_read_lock;
    //! Write lock type
    typedef log::aux::exclusive_lock_guard< log::aux::light_rw_mutex > scoped_write_lock;
#endif

    //! Sinks container type
    typedef std::vector< shared_ptr< sinks::sink > > sink_list;

    //! Thread-specific data
    struct thread_data
    {
        //! Thread-specific attribute set
        attribute_set m_thread_attributes;
        //! Random number generator for shuffling
        random::taus88 m_rng;

        thread_data() : m_rng(get_random_seed())
        {
        }

    private:
        //! Creates a seed for RNG
        static uint32_t get_random_seed()
        {
            uint32_t seed = static_cast< uint32_t >(posix_time::microsec_clock::universal_time().time_of_day().ticks());
#if !defined(BOOST_LOG_NO_THREADS)
            seed += static_cast< uint32_t >(log::aux::this_thread::get_id().native_id());
#endif
            return seed;
        }
    };

public:
#if !defined(BOOST_LOG_NO_THREADS)
    //! Synchronization mutex
    log::aux::light_rw_mutex m_mutex;
#endif

    //! List of sinks involved into output
    sink_list m_sinks;
    //! Default sink
    const shared_ptr< sinks::sink > m_default_sink;

    //! Global attribute set
    attribute_set m_global_attributes;
#if !defined(BOOST_LOG_NO_THREADS)
    //! Thread-specific data
    thread_specific_ptr< thread_data > m_thread_data;

#if defined(BOOST_LOG_USE_COMPILER_TLS)
    //! Cached pointer to the thread-specific data
    static BOOST_LOG_TLS thread_data* m_thread_data_cache;
#endif

#else
    //! Thread-specific data
    log::aux::unique_ptr< thread_data > m_thread_data;
#endif

    //! The global state of logging
    volatile bool m_enabled;
    //! Global filter
    filter m_filter;

    //! Exception handler
    exception_handler_type m_exception_handler;

public:
    //! Constructor
    implementation() :
        m_default_sink(boost::make_shared< sinks::aux::default_sink >()),
        m_enabled(true)
    {
    }

    //! Opens a record
    template< typename SourceAttributesT >
    BOOST_FORCEINLINE record open_record(BOOST_FWD_REF(SourceAttributesT) source_attributes)
    {
        record_view::private_data* rec_impl = NULL;
        bool invoke_exception_handler = true;

        // Try a quick win first
        if (m_enabled) try
        {
            thread_data* tsd = get_thread_data();

            // Lock the core to be safe against any attribute or sink set modifications
            BOOST_LOG_EXPR_IF_MT(scoped_read_lock lock(m_mutex);)

            if (m_enabled)
            {
                // Compose a view of attribute values (unfrozen, yet)
                attribute_value_set attr_values(boost::forward< SourceAttributesT >(source_attributes), tsd->m_thread_attributes, m_global_attributes);
                if (m_filter(attr_values))
                {
                    // The global filter passed, trying the sinks
                    attribute_value_set* values = &attr_values;

                    // apply_sink_filter will invoke the exception handler if it has to
                    invoke_exception_handler = false;

                    if (!m_sinks.empty())
                    {
                        uint32_t remaining_capacity = static_cast< uint32_t >(m_sinks.size());
                        sink_list::iterator it = m_sinks.begin(), end = m_sinks.end();
                        for (; it != end; ++it, --remaining_capacity)
                        {
                            apply_sink_filter(*it, rec_impl, values, remaining_capacity);
                        }
                    }
                    else
                    {
                        // Use the default sink
                        apply_sink_filter(m_default_sink, rec_impl, values, 1);
                    }

                    invoke_exception_handler = true;

                    if (rec_impl && rec_impl->accepting_sink_count() == 0)
                    {
                        // No sinks accepted the record
                        rec_impl->destroy();
                        rec_impl = NULL;
                        goto done;
                    }

                    // Some sinks have accepted the record
                    values->freeze();
                }
            }
        }
#if !defined(BOOST_LOG_NO_THREADS)
        catch (thread_interrupted&)
        {
            if (rec_impl)
                rec_impl->destroy();
            throw;
        }
#endif // !defined(BOOST_LOG_NO_THREADS)
        catch (...)
        {
            if (rec_impl)
            {
                rec_impl->destroy();
                rec_impl = NULL;
            }

            if (invoke_exception_handler)
            {
                // Lock the core to be safe against any attribute or sink set modifications
                BOOST_LOG_EXPR_IF_MT(scoped_read_lock lock(m_mutex);)
                if (m_exception_handler.empty())
                    throw;

                m_exception_handler();
            }
            else
                throw;
        }

    done:
        return record(rec_impl);
    }

    //! The method returns the current thread-specific data
    thread_data* get_thread_data()
    {
#if defined(BOOST_LOG_USE_COMPILER_TLS)
        thread_data* p = m_thread_data_cache;
#else
        thread_data* p = m_thread_data.get();
#endif
        if (!p)
        {
            init_thread_data();
#if defined(BOOST_LOG_USE_COMPILER_TLS)
            p = m_thread_data_cache;
#else
            p = m_thread_data.get();
#endif
        }
        return p;
    }

    //! The function initializes the logging system
    static void init_instance()
    {
        base_type::get_instance().reset(new core());
    }

private:
    //! The method initializes thread-specific data
    void init_thread_data()
    {
        BOOST_LOG_EXPR_IF_MT(scoped_write_lock lock(m_mutex);)
        if (!m_thread_data.get())
        {
            log::aux::unique_ptr< thread_data > p(new thread_data());
            m_thread_data.reset(p.get());
#if defined(BOOST_LOG_USE_COMPILER_TLS)
            m_thread_data_cache = p.release();
#else
            p.release();
#endif
        }
    }

    //! Invokes sink-specific filter and adds the sink to the record if the filter passes the log record
    void apply_sink_filter(shared_ptr< sinks::sink > const& sink, record_view::private_data*& rec_impl, attribute_value_set*& attr_values, uint32_t remaining_capacity)
    {
        try
        {
            if (sink->will_consume(*attr_values))
            {
                // If at least one sink accepts the record, it's time to create it
                record_view::private_data* impl = rec_impl;
                if (!impl)
                {
                    rec_impl = impl = record_view::private_data::create(boost::move(*attr_values), remaining_capacity);
                    attr_values = &impl->m_attribute_values;
                }

                impl->push_back_accepting_sink(sink);
            }
        }
#if !defined(BOOST_LOG_NO_THREADS)
        catch (thread_interrupted&)
        {
            throw;
        }
#endif // !defined(BOOST_LOG_NO_THREADS)
        catch (...)
        {
            if (m_exception_handler.empty())
                throw;
            m_exception_handler();
        }
    }
};

#if defined(BOOST_LOG_USE_COMPILER_TLS)
//! Cached pointer to the thread-specific data
BOOST_LOG_TLS core::implementation::thread_data* core::implementation::m_thread_data_cache = NULL;
#endif // defined(BOOST_LOG_USE_COMPILER_TLS)

//! Logging system constructor
core::core() :
    m_impl(new implementation())
{
}

//! Logging system destructor
core::~core()
{
    delete m_impl;
    m_impl = NULL;
}

//! The method returns a pointer to the logging system instance
BOOST_LOG_API core_ptr core::get()
{
    return implementation::get();
}

//! The method enables or disables logging and returns the previous state of logging flag
BOOST_LOG_API bool core::set_logging_enabled(bool enabled)
{
    BOOST_LOG_EXPR_IF_MT(implementation::scoped_write_lock lock(m_impl->m_mutex);)
    const bool old_value = m_impl->m_enabled;
    m_impl->m_enabled = enabled;
    return old_value;
}

//! The method allows to detect if logging is enabled
BOOST_LOG_API bool core::get_logging_enabled() const
{
    // Should have a read barrier here, but for performance reasons it is omitted.
    // The function should be used as a quick check and doesn't need to be reliable.
    return m_impl->m_enabled;
}

//! The method adds a new sink
BOOST_LOG_API void core::add_sink(shared_ptr< sinks::sink > const& s)
{
    BOOST_LOG_EXPR_IF_MT(implementation::scoped_write_lock lock(m_impl->m_mutex);)
    implementation::sink_list::iterator it =
        std::find(m_impl->m_sinks.begin(), m_impl->m_sinks.end(), s);
    if (it == m_impl->m_sinks.end())
        m_impl->m_sinks.push_back(s);
}

//! The method removes the sink from the output
BOOST_LOG_API void core::remove_sink(shared_ptr< sinks::sink > const& s)
{
    BOOST_LOG_EXPR_IF_MT(implementation::scoped_write_lock lock(m_impl->m_mutex);)
    implementation::sink_list::iterator it =
        std::find(m_impl->m_sinks.begin(), m_impl->m_sinks.end(), s);
    if (it != m_impl->m_sinks.end())
        m_impl->m_sinks.erase(it);
}

//! The method removes all registered sinks from the output
BOOST_LOG_API void core::remove_all_sinks()
{
    BOOST_LOG_EXPR_IF_MT(implementation::scoped_write_lock lock(m_impl->m_mutex);)
    m_impl->m_sinks.clear();
}


//! The method adds an attribute to the global attribute set
BOOST_LOG_API std::pair< attribute_set::iterator, bool >
core::add_global_attribute(attribute_name const& name, attribute const& attr)
{
    BOOST_LOG_EXPR_IF_MT(implementation::scoped_write_lock lock(m_impl->m_mutex);)
    return m_impl->m_global_attributes.insert(name, attr);
}

//! The method removes an attribute from the global attribute set
BOOST_LOG_API void core::remove_global_attribute(attribute_set::iterator it)
{
    BOOST_LOG_EXPR_IF_MT(implementation::scoped_write_lock lock(m_impl->m_mutex);)
    m_impl->m_global_attributes.erase(it);
}

//! The method returns the complete set of currently registered global attributes
BOOST_LOG_API attribute_set core::get_global_attributes() const
{
    BOOST_LOG_EXPR_IF_MT(implementation::scoped_read_lock lock(m_impl->m_mutex);)
    return m_impl->m_global_attributes;
}

//! The method replaces the complete set of currently registered global attributes with the provided set
BOOST_LOG_API void core::set_global_attributes(attribute_set const& attrs)
{
    BOOST_LOG_EXPR_IF_MT(implementation::scoped_write_lock lock(m_impl->m_mutex);)
    m_impl->m_global_attributes = attrs;
}

//! The method adds an attribute to the thread-specific attribute set
BOOST_LOG_API std::pair< attribute_set::iterator, bool >
core::add_thread_attribute(attribute_name const& name, attribute const& attr)
{
    implementation::thread_data* p = m_impl->get_thread_data();
    return p->m_thread_attributes.insert(name, attr);
}

//! The method removes an attribute from the thread-specific attribute set
BOOST_LOG_API void core::remove_thread_attribute(attribute_set::iterator it)
{
    implementation::thread_data* p = m_impl->get_thread_data();
    p->m_thread_attributes.erase(it);
}

//! The method returns the complete set of currently registered thread-specific attributes
BOOST_LOG_API attribute_set core::get_thread_attributes() const
{
    implementation::thread_data* p = m_impl->get_thread_data();
    return p->m_thread_attributes;
}
//! The method replaces the complete set of currently registered thread-specific attributes with the provided set
BOOST_LOG_API void core::set_thread_attributes(attribute_set const& attrs)
{
    implementation::thread_data* p = m_impl->get_thread_data();
    p->m_thread_attributes = attrs;
}

//! An internal method to set the global filter
BOOST_LOG_API void core::set_filter(filter const& filter)
{
    BOOST_LOG_EXPR_IF_MT(implementation::scoped_write_lock lock(m_impl->m_mutex);)
    m_impl->m_filter = filter;
}

//! The method removes the global logging filter
BOOST_LOG_API void core::reset_filter()
{
    BOOST_LOG_EXPR_IF_MT(implementation::scoped_write_lock lock(m_impl->m_mutex);)
    m_impl->m_filter.reset();
}

//! The method sets exception handler function
BOOST_LOG_API void core::set_exception_handler(exception_handler_type const& handler)
{
    BOOST_LOG_EXPR_IF_MT(implementation::scoped_write_lock lock(m_impl->m_mutex);)
    m_impl->m_exception_handler = handler;
}

//! The method performs flush on all registered sinks.
BOOST_LOG_API void core::flush()
{
    // Acquire exclusive lock to prevent any logging attempts while flushing
    BOOST_LOG_EXPR_IF_MT(implementation::scoped_write_lock lock(m_impl->m_mutex);)
    implementation::sink_list::iterator it = m_impl->m_sinks.begin(), end = m_impl->m_sinks.end();
    for (; it != end; ++it)
    {
        try
        {
            it->get()->flush();
        }
#if !defined(BOOST_LOG_NO_THREADS)
        catch (thread_interrupted&)
        {
            throw;
        }
#endif // !defined(BOOST_LOG_NO_THREADS)
        catch (...)
        {
            if (m_impl->m_exception_handler.empty())
                throw;
            m_impl->m_exception_handler();
        }
    }
}

//! The method attempts to open a new record to be written
BOOST_LOG_API record core::open_record(attribute_set const& source_attributes)
{
    return m_impl->open_record(source_attributes);
}

//! The method attempts to open a new record to be written
BOOST_LOG_API record core::open_record(attribute_value_set const& source_attributes)
{
    return m_impl->open_record(source_attributes);
}

//! The method attempts to open a new record to be written.
BOOST_LOG_API record core::open_record_move(attribute_value_set& source_attributes)
{
    return m_impl->open_record(boost::move(source_attributes));
}

//! The method pushes the record
BOOST_LOG_API void core::push_record_move(record& rec)
{
    try
    {
        record_view rec_view(rec.lock());
        record_view::private_data* data = static_cast< record_view::private_data* >(rec_view.m_impl.get());

        typedef std::vector< shared_ptr< sinks::sink > > accepting_sinks_t;
        accepting_sinks_t accepting_sinks(data->accepting_sink_count());
        shared_ptr< sinks::sink >* const begin = &*accepting_sinks.begin();
        shared_ptr< sinks::sink >* end = begin;

        // Lock sinks that are willing to consume the record
        record_view::private_data::sink_list weak_sinks = data->get_accepting_sinks();
        record_view::private_data::sink_list::iterator
            weak_it = weak_sinks.begin(),
            weak_end = weak_sinks.end();
        for (; weak_it != weak_end; ++weak_it)
        {
            shared_ptr< sinks::sink >& last = *end;
            weak_it->lock().swap(last);
            if (last.get())
                ++end;
        }

        bool shuffled = (end - begin) <= 1;
        shared_ptr< sinks::sink >* it = begin;
        while (true) try
        {
            // First try to distribute load between different sinks
            bool all_locked = true;
            while (it != end)
            {
                if (it->get()->try_consume(rec_view))
                {
                    --end;
                    end->swap(*it);
                    all_locked = false;
                }
                else
                    ++it;
            }

            it = begin;
            if (begin != end)
            {
                if (all_locked)
                {
                    // If all sinks are busy then block on any
                    if (!shuffled)
                    {
                        implementation::thread_data* tsd = m_impl->get_thread_data();
                        log::aux::random_shuffle(begin, end, tsd->m_rng);
                        shuffled = true;
                    }

                    it->get()->consume(rec_view);
                    --end;
                    end->swap(*it);
                }
            }
            else
                break;
        }
#if !defined(BOOST_LOG_NO_THREADS)
        catch (thread_interrupted&)
        {
            throw;
        }
#endif // !defined(BOOST_LOG_NO_THREADS)
        catch (...)
        {
            // Lock the core to be safe against any attribute or sink set modifications
            BOOST_LOG_EXPR_IF_MT(implementation::scoped_read_lock lock(m_impl->m_mutex);)
            if (m_impl->m_exception_handler.empty())
                throw;

            m_impl->m_exception_handler();

            // Skip the sink that failed to consume the record
            --end;
            end->swap(*it);
        }
    }
#if !defined(BOOST_LOG_NO_THREADS)
    catch (thread_interrupted&)
    {
        throw;
    }
#endif // !defined(BOOST_LOG_NO_THREADS)
    catch (...)
    {
        // Lock the core to be safe against any attribute or sink set modifications
        BOOST_LOG_EXPR_IF_MT(implementation::scoped_read_lock lock(m_impl->m_mutex);)
        if (m_impl->m_exception_handler.empty())
            throw;

        m_impl->m_exception_handler();
    }
}

BOOST_LOG_CLOSE_NAMESPACE // namespace log

} // namespace boost

#include <boost/log/detail/footer.hpp>
