/*
 *                 Copyright Lingxi Li 2015.
 *              Copyright Andrey Semashev 2016.
 * Distributed under the Boost Software License, Version 1.0.
 *    (See accompanying file LICENSE_1_0.txt or copy at
 *          http://www.boost.org/LICENSE_1_0.txt)
 */
/*!
 * \file   posix/ipc_reliable_message_queue.cpp
 * \author Lingxi Li
 * \author Andrey Semashev
 * \date   17.11.2015
 *
 * \brief  This header is the Boost.Log library implementation, see the library documentation
 *         at http://www.boost.org/doc/libs/release/libs/log/doc/html/index.html.
 *
 * This file provides an interprocess message queue implementation on POSIX platforms.
 */

#include <boost/log/detail/config.hpp>
#include <cstddef>
#include <cerrno>
#include <cstring>
#include <ctime>
#include <new>
#include <string>
#include <stdexcept>
#include <algorithm>
#include <unistd.h>
#if defined(BOOST_HAS_SCHED_YIELD)
#include <sched.h>
#elif defined(BOOST_HAS_PTHREAD_YIELD)
#include <pthread.h>
#elif defined(BOOST_HAS_NANOSLEEP)
#include <time.h>
#endif
#include <boost/assert.hpp>
#include <boost/static_assert.hpp>
#include <boost/cstdint.hpp>
#include <boost/memory_order.hpp>
#include <boost/atomic/atomic.hpp>
#include <boost/atomic/ipc_atomic.hpp>
#include <boost/atomic/capabilities.hpp>
#include <boost/throw_exception.hpp>
#include <boost/log/exceptions.hpp>
#include <boost/log/utility/ipc/reliable_message_queue.hpp>
#include <boost/log/support/exception.hpp>
#include <boost/log/detail/pause.hpp>
#include <boost/exception/info.hpp>
#include <boost/exception/enable_error_info.hpp>
#include <boost/interprocess/creation_tags.hpp>
#include <boost/interprocess/exceptions.hpp>
#include <boost/interprocess/permissions.hpp>
#include <boost/interprocess/mapped_region.hpp>
#include <boost/interprocess/shared_memory_object.hpp>
#include <boost/align/align_up.hpp>
#include "ipc_sync_wrappers.hpp"
#include "murmur3.hpp"
#include "bit_tools.hpp"
#include <boost/log/detail/header.hpp>

namespace boost {

BOOST_LOG_OPEN_NAMESPACE

namespace ipc {

//! Message queue implementation data
struct reliable_message_queue::implementation
{
private:
    //! Header of an allocation block within the message queue. Placed at the beginning of the block within the shared memory segment.
    struct block_header
    {
        // Element data alignment, in bytes
        enum { data_alignment = 32u };

        //! Size of the element data, in bytes
        size_type m_size;

        //! Returns the block header overhead, in bytes
        static BOOST_CONSTEXPR size_type get_header_overhead() BOOST_NOEXCEPT
        {
            return static_cast< size_type >(boost::alignment::align_up(sizeof(block_header), data_alignment));
        }

        //! Returns a pointer to the element data
        void* get_data() const BOOST_NOEXCEPT
        {
            return const_cast< unsigned char* >(reinterpret_cast< const unsigned char* >(this)) + get_header_overhead();
        }
    };

    //! Header of the message queue. Placed at the beginning of the shared memory segment.
    struct header
    {
        // Increment this constant whenever you change the binary layout of the queue (apart from this header structure)
        enum { abi_version = 0 };

        // !!! Whenever you add/remove members in this structure, also modify get_abi_tag() function accordingly !!!

        //! A tag value to ensure the correct binary layout of the message queue data structures. Must be placed first and always have a fixed size and alignment.
        uint32_t m_abi_tag;
        //! Padding to protect against alignment changes in Boost.Atomic. Don't use BOOST_ALIGNMENT to ensure portability.
        unsigned char m_padding[BOOST_LOG_CPU_CACHE_LINE_SIZE - sizeof(uint32_t)];
        //! Reference counter. Also acts as a flag indicating that the queue is constructed (i.e. the queue is constructed when the counter is not 0).
        boost::ipc_atomic< uint32_t > m_ref_count;
        //! Number of allocation blocks in the queue.
        const uint32_t m_capacity;
        //! Size of an allocation block, in bytes.
        const size_type m_block_size;
        //! Mutex for protecting queue data structures.
        boost::log::ipc::aux::interprocess_mutex m_mutex;
        //! Condition variable used to block readers when the queue is empty.
        boost::log::ipc::aux::interprocess_condition_variable m_nonempty_queue;
        //! Condition variable used to block writers when the queue is full.
        boost::log::ipc::aux::interprocess_condition_variable m_nonfull_queue;
        //! The current number of allocated blocks in the queue.
        uint32_t m_size;
        //! The current writing position (allocation block index).
        uint32_t m_put_pos;
        //! The current reading position (allocation block index).
        uint32_t m_get_pos;

        header(uint32_t capacity, size_type block_size) :
            m_abi_tag(get_abi_tag()),
            m_capacity(capacity),
            m_block_size(block_size),
            m_size(0u),
            m_put_pos(0u),
            m_get_pos(0u)
        {
            // Must be initialized last. m_ref_count is zero-initialized initially.
            m_ref_count.opaque_add(1u, boost::memory_order_release);
        }

        //! Returns the header structure ABI tag
        static uint32_t get_abi_tag() BOOST_NOEXCEPT
        {
            // This FOURCC identifies the queue type
            boost::log::aux::murmur3_32 hash(boost::log::aux::make_fourcc('r', 'e', 'l', 'q'));

            // This FOURCC identifies the queue implementation
            hash.mix(boost::log::aux::make_fourcc('p', 't', 'h', 'r'));
            hash.mix(abi_version);

            // We will use these constants to align pointers
            hash.mix(BOOST_LOG_CPU_CACHE_LINE_SIZE);
            hash.mix(block_header::data_alignment);

            // The members in the sequence below must be enumerated in the same order as they are declared in the header structure.
            // The ABI tag is supposed change whenever a member changes size or offset from the beginning of the header.

#define BOOST_LOG_MIX_HEADER_MEMBER(name)\
            hash.mix(static_cast< uint32_t >(sizeof(((header*)NULL)->name)));\
            hash.mix(static_cast< uint32_t >(offsetof(header, name)))

            BOOST_LOG_MIX_HEADER_MEMBER(m_abi_tag);
            BOOST_LOG_MIX_HEADER_MEMBER(m_padding);
            BOOST_LOG_MIX_HEADER_MEMBER(m_ref_count);
            BOOST_LOG_MIX_HEADER_MEMBER(m_capacity);
            BOOST_LOG_MIX_HEADER_MEMBER(m_block_size);
            BOOST_LOG_MIX_HEADER_MEMBER(m_mutex);
            BOOST_LOG_MIX_HEADER_MEMBER(m_nonempty_queue);
            BOOST_LOG_MIX_HEADER_MEMBER(m_nonfull_queue);
            BOOST_LOG_MIX_HEADER_MEMBER(m_size);
            BOOST_LOG_MIX_HEADER_MEMBER(m_put_pos);
            BOOST_LOG_MIX_HEADER_MEMBER(m_get_pos);

#undef BOOST_LOG_MIX_HEADER_MEMBER

            return hash.finalize();
        }

        //! Returns an element header at the specified index
        block_header* get_block(uint32_t index) const BOOST_NOEXCEPT
        {
            BOOST_ASSERT(index < m_capacity);
            unsigned char* p = const_cast< unsigned char* >(reinterpret_cast< const unsigned char* >(this)) + boost::alignment::align_up(sizeof(header), BOOST_LOG_CPU_CACHE_LINE_SIZE);
            p += static_cast< std::size_t >(m_block_size) * static_cast< std::size_t >(index);
            return reinterpret_cast< block_header* >(p);
        }

        BOOST_DELETED_FUNCTION(header(header const&))
        BOOST_DELETED_FUNCTION(header& operator=(header const&))
    };

private:
    //! Shared memory object
    boost::interprocess::shared_memory_object m_shared_memory;
    //! Shared memory mapping into the process address space
    boost::interprocess::mapped_region m_region;
    //! Queue overflow handling policy
    const overflow_policy m_overflow_policy;
    //! The mask for selecting bits that constitute size values from 0 to (block_size - 1)
    size_type m_block_size_mask;
    //! The number of the bit set in block_size (i.e. log base 2 of block_size)
    uint32_t m_block_size_log2;
    //! The flag indicates that stop has been requested
    boost::atomic< bool > m_stop;

    //! Queue shared memory object name
    const object_name m_name;

    //! The total number of loop iterations in \c adopt_region for waiting for the region initialization to complete
    static BOOST_CONSTEXPR_OR_CONST unsigned int region_init_wait_loops = 200u;
    //! Threshold of the number of loop iterations in \c adopt_region for using pause instructions for yielding
    static BOOST_CONSTEXPR_OR_CONST unsigned int region_init_wait_loops_pause = 16u;
    //! Threshold of the number of loop iterations in \c adopt_region for using \c short_yield for yielding
    static BOOST_CONSTEXPR_OR_CONST unsigned int region_init_wait_loops_short_yield = 64u;
    //! Timeout, in seconds, for performing shared memory creation/opening loop
    static BOOST_CONSTEXPR_OR_CONST unsigned int region_open_or_create_timeout = 60u;
    //! The number of short yields to perform during the shared memory creation/opening loop
    static BOOST_CONSTEXPR_OR_CONST unsigned int region_open_or_create_short_yield_loops = 64u;

public:
    //! The constructor creates a new shared memory segment
    implementation
    (
        open_mode::create_only_tag,
        object_name const& name,
        uint32_t capacity,
        size_type block_size,
        overflow_policy oflow_policy,
        permissions const& perms
    ) :
        m_shared_memory(),
        m_region(),
        m_overflow_policy(oflow_policy),
        m_block_size_mask(0u),
        m_block_size_log2(0u),
        m_stop(false),
        m_name(name)
    {
        BOOST_ASSERT(block_size >= block_header::get_header_overhead());

        boost::interprocess::permissions ipc_perms(perms.get_native());
        while (true)
        {
            try
            {
                boost::interprocess::shared_memory_object shared_memory(boost::interprocess::create_only, name.c_str(), boost::interprocess::read_write, ipc_perms);
                m_shared_memory.swap(shared_memory);
                break;
            }
            catch (boost::interprocess::interprocess_exception& e)
            {
                // shared_memory_object does not handle EINTR returned from shm_open internally.
                // https://github.com/boostorg/interprocess/issues/152
                if (e.get_native_error() != EINTR)
                    throw;
            }
        }

        create_region(capacity, block_size);
    }

    //! The constructor creates a new shared memory segment or opens the existing one
    implementation
    (
        open_mode::open_or_create_tag,
        object_name const& name,
        uint32_t capacity,
        size_type block_size,
        overflow_policy oflow_policy,
        permissions const& perms
    ) :
        m_shared_memory(),
        m_region(),
        m_overflow_policy(oflow_policy),
        m_block_size_mask(0u),
        m_block_size_log2(0u),
        m_stop(false),
        m_name(name)
    {
        BOOST_ASSERT(block_size >= block_header::get_header_overhead());

        // We need to know for certain whether we create the shared memory segment or open an existing one.
        // This is to ensure that only one thread initializes the segment and all other threads wait until completion.
        // Since shared_memory_object(open_or_create) constructor does not report whether the segment was actually created,
        // we have to loop trying to create or open the segment. https://github.com/boostorg/interprocess/issues/151
        boost::interprocess::permissions ipc_perms(perms.get_native());
        bool created = false;
        unsigned int i = 0u;
        std::time_t start_time = std::time(NULL);
        while (true)
        {
            while (true)
            {
                try
                {
                    boost::interprocess::shared_memory_object shared_memory(boost::interprocess::create_only, name.c_str(), boost::interprocess::read_write, ipc_perms);
                    m_shared_memory.swap(shared_memory);
                    created = true;
                    goto done;
                }
                catch (boost::interprocess::interprocess_exception& e)
                {
                    if (e.get_error_code() == boost::interprocess::already_exists_error)
                        break;

                    // shared_memory_object does not handle EINTR returned from shm_open internally.
                    // https://github.com/boostorg/interprocess/issues/152
                    if (e.get_native_error() != EINTR)
                       throw;
                }
            }

            while (true)
            {
                try
                {
                    boost::interprocess::shared_memory_object shared_memory(boost::interprocess::open_only, name.c_str(), boost::interprocess::read_write);
                    m_shared_memory.swap(shared_memory);
                    created = false;
                    goto done;
                }
                catch (boost::interprocess::interprocess_exception& e)
                {
                    if (e.get_error_code() == boost::interprocess::not_found_error)
                        break;

                    if (e.get_native_error() != EINTR)
                        throw;
                }
            }

            std::time_t now = std::time(NULL);
            if (BOOST_UNLIKELY((now - start_time) >= region_open_or_create_timeout))
                BOOST_LOG_THROW_DESCR(setup_error, "Boost.Log interprocess message queue cannot be created or opened: shared memory segment failed to be created or opened until timeout (possible livelock)");

            if (i < region_open_or_create_short_yield_loops)
                short_yield();
            else
                long_yield();

            ++i;
        }

    done:
        if (created)
            create_region(capacity, block_size);
        else
            adopt_region();
    }

    //! The constructor opens the existing shared memory segment
    implementation
    (
        open_mode::open_only_tag,
        object_name const& name,
        overflow_policy oflow_policy
    ) :
        m_shared_memory(),
        m_region(),
        m_overflow_policy(oflow_policy),
        m_block_size_mask(0u),
        m_block_size_log2(0u),
        m_stop(false),
        m_name(name)
    {
        while (true)
        {
            try
            {
                boost::interprocess::shared_memory_object shared_memory(boost::interprocess::open_only, name.c_str(), boost::interprocess::read_write);
                m_shared_memory.swap(shared_memory);
                break;
            }
            catch (boost::interprocess::interprocess_exception& e)
            {
                // shared_memory_object does not handle EINTR returned from shm_open internally.
                // https://github.com/boostorg/interprocess/issues/152
                if (e.get_native_error() != EINTR)
                    throw;
            }
        }

        adopt_region();
    }

    ~implementation()
    {
        close_region();
    }

    object_name const& name() const BOOST_NOEXCEPT
    {
        return m_name;
    }

    uint32_t capacity() const BOOST_NOEXCEPT
    {
        return get_header()->m_capacity;
    }

    size_type block_size() const BOOST_NOEXCEPT
    {
        return get_header()->m_block_size;
    }

    operation_result send(void const* message_data, size_type message_size)
    {
        const uint32_t block_count = estimate_block_count(message_size);

        header* const hdr = get_header();

        if (BOOST_UNLIKELY(block_count > hdr->m_capacity))
            BOOST_LOG_THROW_DESCR(logic_error, "Message size exceeds the interprocess queue capacity");

        if (m_stop.load(boost::memory_order_relaxed))
            return aborted;

        lock_queue();
        boost::log::ipc::aux::interprocess_mutex::auto_unlock unlock(hdr->m_mutex);

        while (true)
        {
            if (m_stop.load(boost::memory_order_relaxed))
                return aborted;

            if ((hdr->m_capacity - hdr->m_size) >= block_count)
                break;

            const overflow_policy oflow_policy = m_overflow_policy;
            if (oflow_policy == fail_on_overflow)
                return no_space;
            else if (BOOST_UNLIKELY(oflow_policy == throw_on_overflow))
                BOOST_LOG_THROW_DESCR(capacity_limit_reached, "Interprocess queue is full");

            hdr->m_nonfull_queue.wait(hdr->m_mutex);
        }

        enqueue_message(message_data, message_size, block_count);

        return succeeded;
    }

    bool try_send(void const* message_data, size_type message_size)
    {
        const uint32_t block_count = estimate_block_count(message_size);

        header* const hdr = get_header();

        if (BOOST_UNLIKELY(block_count > hdr->m_capacity))
            BOOST_LOG_THROW_DESCR(logic_error, "Message size exceeds the interprocess queue capacity");

        if (m_stop.load(boost::memory_order_relaxed))
            return false;

        lock_queue();
        boost::log::ipc::aux::interprocess_mutex::auto_unlock unlock(hdr->m_mutex);

        if (m_stop.load(boost::memory_order_relaxed))
            return false;

        if ((hdr->m_capacity - hdr->m_size) < block_count)
            return false;

        enqueue_message(message_data, message_size, block_count);

        return true;
    }

    operation_result receive(receive_handler handler, void* state)
    {
        if (m_stop.load(boost::memory_order_relaxed))
            return aborted;

        lock_queue();
        header* const hdr = get_header();
        boost::log::ipc::aux::interprocess_mutex::auto_unlock unlock(hdr->m_mutex);

        while (true)
        {
            if (m_stop.load(boost::memory_order_relaxed))
                return aborted;

            if (hdr->m_size > 0u)
                break;

            hdr->m_nonempty_queue.wait(hdr->m_mutex);
        }

        dequeue_message(handler, state);

        return succeeded;
    }

    bool try_receive(receive_handler handler, void* state)
    {
        if (m_stop.load(boost::memory_order_relaxed))
            return false;

        lock_queue();
        header* const hdr = get_header();
        boost::log::ipc::aux::interprocess_mutex::auto_unlock unlock(hdr->m_mutex);

        if (hdr->m_size == 0u)
            return false;

        dequeue_message(handler, state);

        return true;
    }

    void stop_local()
    {
        if (m_stop.load(boost::memory_order_relaxed))
            return;

        lock_queue();
        header* const hdr = get_header();
        boost::log::ipc::aux::interprocess_mutex::auto_unlock unlock(hdr->m_mutex);

        m_stop.store(true, boost::memory_order_relaxed);

        hdr->m_nonempty_queue.notify_all();
        hdr->m_nonfull_queue.notify_all();
    }

    void reset_local()
    {
        m_stop.store(false, boost::memory_order_relaxed);
    }

    void clear()
    {
        lock_queue();
        header* const hdr = get_header();
        boost::log::ipc::aux::interprocess_mutex::auto_unlock unlock(hdr->m_mutex);
        clear_queue();
    }

private:
    header* get_header() const BOOST_NOEXCEPT
    {
        return static_cast< header* >(m_region.get_address());
    }

    static std::size_t estimate_region_size(uint32_t capacity, size_type block_size) BOOST_NOEXCEPT
    {
        return boost::alignment::align_up(sizeof(header), BOOST_LOG_CPU_CACHE_LINE_SIZE) + static_cast< std::size_t >(capacity) * static_cast< std::size_t >(block_size);
    }

    void create_region(uint32_t capacity, size_type block_size)
    {
        const std::size_t shmem_size = estimate_region_size(capacity, block_size);
        m_shared_memory.truncate(shmem_size);
        boost::interprocess::mapped_region(m_shared_memory, boost::interprocess::read_write, 0u, shmem_size).swap(m_region);

        new (m_region.get_address()) header(capacity, block_size);

        init_block_size(block_size);
    }

    void adopt_region()
    {
        std::size_t shmem_size = 0u;
        unsigned int i = 0u;
        std::time_t start_time = std::time(NULL);
        while (true)
        {
            boost::interprocess::offset_t shm_size = 0;
            const bool get_size_result = m_shared_memory.get_size(shm_size);
            if (BOOST_LIKELY(get_size_result && shm_size > 0))
            {
                shmem_size = static_cast< std::size_t >(shm_size);
                break;
            }

            std::time_t now = std::time(NULL);
            if (BOOST_UNLIKELY((now - start_time) >= region_open_or_create_timeout))
            {
                if (get_size_result)
                    goto shmem_size_too_small;
                BOOST_LOG_THROW_DESCR(setup_error, "Boost.Log interprocess message queue cannot be opened: shared memory segment size could not be determined until timeout");
            }

            if (i < region_open_or_create_short_yield_loops)
                short_yield();
            else
                long_yield();

            ++i;
        }

        if (BOOST_UNLIKELY(shmem_size < sizeof(header)))
        {
        shmem_size_too_small:
            BOOST_LOG_THROW_DESCR(setup_error, "Boost.Log interprocess message queue cannot be opened: shared memory segment size too small");
        }

        boost::interprocess::mapped_region(m_shared_memory, boost::interprocess::read_write, 0, shmem_size).swap(m_region);

        // Wait until the mapped region becomes initialized
        header* const hdr = get_header();
        for (i = 0u; i < region_init_wait_loops; ++i)
        {
            uint32_t ref_count = hdr->m_ref_count.load(boost::memory_order_acquire);
            while (ref_count > 0u)
            {
                if (hdr->m_ref_count.compare_exchange_weak(ref_count, ref_count + 1u, boost::memory_order_acq_rel, boost::memory_order_acquire))
                    goto done;
            }

            if (i < region_init_wait_loops_pause)
                boost::log::aux::pause();
            else if (i < region_init_wait_loops_short_yield)
                short_yield();
            else
                long_yield();
        }

        BOOST_LOG_THROW_DESCR(setup_error, "Boost.Log interprocess message queue cannot be opened: shared memory segment is not initialized by creator for too long");

    done:
        try
        {
            // Check that the queue layout matches the current process ABI
            if (hdr->m_abi_tag != header::get_abi_tag())
                BOOST_LOG_THROW_DESCR(setup_error, "Boost.Log interprocess message queue cannot be opened: the queue ABI is incompatible");

            if (!boost::log::aux::is_power_of_2(hdr->m_block_size))
                BOOST_LOG_THROW_DESCR(setup_error, "Boost.Log interprocess message queue cannot be opened: the queue block size is not a power of 2");

            init_block_size(hdr->m_block_size);
        }
        catch (...)
        {
            close_region();
            throw;
        }
    }

    void close_region() BOOST_NOEXCEPT
    {
        header* const hdr = get_header();

        if (hdr->m_ref_count.fetch_sub(1u, boost::memory_order_acq_rel) == 1u)
        {
            boost::interprocess::shared_memory_object::remove(m_shared_memory.get_name());

            hdr->~header();

            boost::interprocess::mapped_region().swap(m_region);
            boost::interprocess::shared_memory_object().swap(m_shared_memory);

            m_block_size_mask = 0u;
            m_block_size_log2 = 0u;
        }
    }

    void init_block_size(size_type block_size)
    {
        m_block_size_mask = block_size - 1u;

        uint32_t block_size_log2 = 0u;
        if ((block_size & 0x0000ffff) == 0u)
        {
            block_size >>= 16u;
            block_size_log2 += 16u;
        }
        if ((block_size & 0x000000ff) == 0u)
        {
            block_size >>= 8u;
            block_size_log2 += 8u;
        }
        if ((block_size & 0x0000000f) == 0u)
        {
            block_size >>= 4u;
            block_size_log2 += 4u;
        }
        if ((block_size & 0x00000003) == 0u)
        {
            block_size >>= 2u;
            block_size_log2 += 2u;
        }
        if ((block_size & 0x00000001) == 0u)
        {
            ++block_size_log2;
        }
        m_block_size_log2 = block_size_log2;
    }

    void lock_queue()
    {
        header* const hdr = get_header();

#if defined(BOOST_LOG_HAS_PTHREAD_MUTEX_ROBUST)
        try
        {
#endif
            hdr->m_mutex.lock();
#if defined(BOOST_LOG_HAS_PTHREAD_MUTEX_ROBUST)
        }
        catch (boost::log::ipc::aux::lock_owner_dead&)
        {
            // The mutex is locked by the current thread, but the previous owner terminated without releasing the lock
            try
            {
                clear_queue();
                hdr->m_mutex.recover();
            }
            catch (...)
            {
                hdr->m_mutex.unlock();
                throw;
            }
        }
#endif
    }

    void clear_queue()
    {
        header* const hdr = get_header();
        hdr->m_size = 0u;
        hdr->m_put_pos = 0u;
        hdr->m_get_pos = 0u;
        hdr->m_nonfull_queue.notify_all();
    }

    //! Returns the number of allocation blocks that are required to store user's payload of the specified size
    uint32_t estimate_block_count(size_type size) const BOOST_NOEXCEPT
    {
        // ceil((size + get_header_overhead()) / block_size)
        return static_cast< uint32_t >((size + block_header::get_header_overhead() + m_block_size_mask) >> m_block_size_log2);
    }

    //! Puts the message to the back of the queue
    void enqueue_message(void const* message_data, size_type message_size, uint32_t block_count)
    {
        header* const hdr = get_header();

        const uint32_t capacity = hdr->m_capacity;
        const size_type block_size = hdr->m_block_size;
        uint32_t pos = hdr->m_put_pos;
        BOOST_ASSERT(pos < capacity);

        block_header* block = hdr->get_block(pos);
        block->m_size = message_size;

        size_type write_size = (std::min)(static_cast< size_type >((capacity - pos) * block_size - block_header::get_header_overhead()), message_size);
        std::memcpy(block->get_data(), message_data, write_size);

        pos += block_count;
        if (BOOST_UNLIKELY(pos >= capacity))
        {
            // Write the rest of the message at the beginning of the queue
            pos -= capacity;
            message_data = static_cast< const unsigned char* >(message_data) + write_size;
            write_size = message_size - write_size;
            if (write_size > 0u)
                std::memcpy(hdr->get_block(0u), message_data, write_size);
        }

        hdr->m_put_pos = pos;

        const uint32_t old_queue_size = hdr->m_size;
        hdr->m_size = old_queue_size + block_count;
        if (old_queue_size == 0u)
            hdr->m_nonempty_queue.notify_one();
    }

    //! Retrieves the next message and invokes the handler to store the message contents
    void dequeue_message(receive_handler handler, void* state)
    {
        header* const hdr = get_header();

        const uint32_t capacity = hdr->m_capacity;
        const size_type block_size = hdr->m_block_size;
        uint32_t pos = hdr->m_get_pos;
        BOOST_ASSERT(pos < capacity);

        block_header* block = hdr->get_block(pos);
        size_type message_size = block->m_size;
        uint32_t block_count = estimate_block_count(message_size);

        BOOST_ASSERT(block_count <= hdr->m_size);

        size_type read_size = (std::min)(static_cast< size_type >((capacity - pos) * block_size - block_header::get_header_overhead()), message_size);
        handler(state, block->get_data(), read_size);

        pos += block_count;
        if (BOOST_UNLIKELY(pos >= capacity))
        {
            // Read the tail of the message
            pos -= capacity;
            read_size = message_size - read_size;
            if (read_size > 0u)
                handler(state, hdr->get_block(0u), read_size);
        }

        hdr->m_get_pos = pos;
        hdr->m_size -= block_count;

        hdr->m_nonfull_queue.notify_all();
    }

    static void short_yield() BOOST_NOEXCEPT
    {
#if defined(BOOST_HAS_SCHED_YIELD)
        sched_yield();
#elif defined(BOOST_HAS_PTHREAD_YIELD)
        pthread_yield();
#else
        long_yield();
#endif
    }

    static void long_yield() BOOST_NOEXCEPT
    {
#if defined(BOOST_HAS_NANOSLEEP)
        timespec ts = {};
        ts.tv_sec = 0;
        ts.tv_nsec = 1000;
        nanosleep(&ts, NULL);
#else
        usleep(1);
#endif
    }
};

BOOST_LOG_API void reliable_message_queue::create(object_name const& name, uint32_t capacity, size_type block_size, overflow_policy oflow_policy, permissions const& perms)
{
    BOOST_ASSERT(m_impl == NULL);
    if (!boost::log::aux::is_power_of_2(block_size))
        BOOST_THROW_EXCEPTION(std::invalid_argument("Interprocess message queue block size is not a power of 2"));
    try
    {
        m_impl = new implementation(open_mode::create_only, name, capacity, static_cast< size_type >(boost::alignment::align_up(block_size, BOOST_LOG_CPU_CACHE_LINE_SIZE)), oflow_policy, perms);
    }
    catch (boost::exception& e)
    {
        e << boost::log::ipc::object_name_info(name);
        throw;
    }
    catch (boost::interprocess::interprocess_exception& e)
    {
        BOOST_THROW_EXCEPTION(boost::enable_error_info(system_error(boost::system::error_code(e.get_native_error(), boost::system::system_category()), e.what())) << boost::log::ipc::object_name_info(name));
    }
}

BOOST_LOG_API void reliable_message_queue::open_or_create(object_name const& name, uint32_t capacity, size_type block_size, overflow_policy oflow_policy, permissions const& perms)
{
    BOOST_ASSERT(m_impl == NULL);
    if (!boost::log::aux::is_power_of_2(block_size))
        BOOST_THROW_EXCEPTION(std::invalid_argument("Interprocess message queue block size is not a power of 2"));
    try
    {
        m_impl = new implementation(open_mode::open_or_create, name, capacity, static_cast< size_type >(boost::alignment::align_up(block_size, BOOST_LOG_CPU_CACHE_LINE_SIZE)), oflow_policy, perms);
    }
    catch (boost::exception& e)
    {
        e << boost::log::ipc::object_name_info(name);
        throw;
    }
    catch (boost::interprocess::interprocess_exception& e)
    {
        BOOST_THROW_EXCEPTION(boost::enable_error_info(system_error(boost::system::error_code(e.get_native_error(), boost::system::system_category()), e.what())) << boost::log::ipc::object_name_info(name));
    }
}

BOOST_LOG_API void reliable_message_queue::open(object_name const& name, overflow_policy oflow_policy, permissions const&)
{
    BOOST_ASSERT(m_impl == NULL);
    try
    {
        m_impl = new implementation(open_mode::open_only, name, oflow_policy);
    }
    catch (boost::exception& e)
    {
        e << boost::log::ipc::object_name_info(name);
        throw;
    }
    catch (boost::interprocess::interprocess_exception& e)
    {
        BOOST_THROW_EXCEPTION(boost::enable_error_info(system_error(boost::system::error_code(e.get_native_error(), boost::system::system_category()), e.what())) << boost::log::ipc::object_name_info(name));
    }
}

BOOST_LOG_API void reliable_message_queue::clear()
{
    BOOST_ASSERT(m_impl != NULL);
    try
    {
        m_impl->clear();
    }
    catch (boost::exception& e)
    {
        e << boost::log::ipc::object_name_info(m_impl->name());
        throw;
    }
}

BOOST_LOG_API object_name const& reliable_message_queue::name() const
{
    BOOST_ASSERT(m_impl != NULL);
    return m_impl->name();
}

BOOST_LOG_API uint32_t reliable_message_queue::capacity() const
{
    BOOST_ASSERT(m_impl != NULL);
    return m_impl->capacity();
}

BOOST_LOG_API reliable_message_queue::size_type reliable_message_queue::block_size() const
{
    BOOST_ASSERT(m_impl != NULL);
    return m_impl->block_size();
}

BOOST_LOG_API void reliable_message_queue::stop_local()
{
    BOOST_ASSERT(m_impl != NULL);
    try
    {
        m_impl->stop_local();
    }
    catch (boost::exception& e)
    {
        e << boost::log::ipc::object_name_info(m_impl->name());
        throw;
    }
}

BOOST_LOG_API void reliable_message_queue::reset_local()
{
    BOOST_ASSERT(m_impl != NULL);
    try
    {
        m_impl->reset_local();
    }
    catch (boost::exception& e)
    {
        e << boost::log::ipc::object_name_info(m_impl->name());
        throw;
    }
}

BOOST_LOG_API void reliable_message_queue::do_close() BOOST_NOEXCEPT
{
    delete m_impl;
    m_impl = NULL;
}

BOOST_LOG_API reliable_message_queue::operation_result reliable_message_queue::send(void const* message_data, size_type message_size)
{
    BOOST_ASSERT(m_impl != NULL);
    try
    {
        return m_impl->send(message_data, message_size);
    }
    catch (boost::exception& e)
    {
        e << boost::log::ipc::object_name_info(m_impl->name());
        throw;
    }
}

BOOST_LOG_API bool reliable_message_queue::try_send(void const* message_data, size_type message_size)
{
    BOOST_ASSERT(m_impl != NULL);
    try
    {
        return m_impl->try_send(message_data, message_size);
    }
    catch (boost::exception& e)
    {
        e << boost::log::ipc::object_name_info(m_impl->name());
        throw;
    }
}

BOOST_LOG_API reliable_message_queue::operation_result reliable_message_queue::do_receive(receive_handler handler, void* state)
{
    BOOST_ASSERT(m_impl != NULL);
    try
    {
        return m_impl->receive(handler, state);
    }
    catch (boost::exception& e)
    {
        e << boost::log::ipc::object_name_info(m_impl->name());
        throw;
    }
}

BOOST_LOG_API bool reliable_message_queue::do_try_receive(receive_handler handler, void* state)
{
    BOOST_ASSERT(m_impl != NULL);
    try
    {
        return m_impl->try_receive(handler, state);
    }
    catch (boost::exception& e)
    {
        e << boost::log::ipc::object_name_info(m_impl->name());
        throw;
    }
}

//! Fixed buffer receive handler
BOOST_LOG_API void reliable_message_queue::fixed_buffer_receive_handler(void* state, const void* data, size_type size)
{
    fixed_buffer_state* p = static_cast< fixed_buffer_state* >(state);
    if (BOOST_UNLIKELY(size > p->size))
        BOOST_THROW_EXCEPTION(bad_alloc("Buffer too small to receive the message"));

    std::memcpy(p->data, data, size);
    p->data += size;
    p->size -= size;
}

BOOST_LOG_API void reliable_message_queue::remove(object_name const& name)
{
    boost::interprocess::shared_memory_object::remove(name.c_str());
}

} // namespace ipc

BOOST_LOG_CLOSE_NAMESPACE // namespace log

} // namespace boost

#include <boost/log/detail/footer.hpp>
