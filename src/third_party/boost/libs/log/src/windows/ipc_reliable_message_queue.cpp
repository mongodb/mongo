/*
 *                Copyright Lingxi Li 2015.
 *             Copyright Andrey Semashev 2016.
 * Distributed under the Boost Software License, Version 1.0.
 *    (See accompanying file LICENSE_1_0.txt or copy at
 *          http://www.boost.org/LICENSE_1_0.txt)
 */
/*!
 * \file   ipc_reliable_message_queue_win.hpp
 * \author Lingxi Li
 * \author Andrey Semashev
 * \date   28.10.2015
 *
 * \brief  This header is the Boost.Log library implementation, see the library documentation
 *         at http://www.boost.org/doc/libs/release/libs/log/doc/html/index.html.
 *
 * This file provides an interprocess message queue implementation on POSIX platforms.
 */

#include <boost/log/detail/config.hpp>
#include <cstddef>
#include <cstring>
#include <new>
#include <limits>
#include <string>
#include <algorithm>
#include <stdexcept>
#include <boost/assert.hpp>
#include <boost/static_assert.hpp>
#include <boost/cstdint.hpp>
#include <boost/memory_order.hpp>
#include <boost/atomic/ipc_atomic.hpp>
#include <boost/atomic/capabilities.hpp>
#include <boost/log/exceptions.hpp>
#include <boost/log/utility/ipc/reliable_message_queue.hpp>
#include <boost/log/support/exception.hpp>
#include <boost/log/detail/pause.hpp>
#include <boost/exception/info.hpp>
#include <boost/exception/enable_error_info.hpp>
#include <boost/align/align_up.hpp>
#include <boost/winapi/thread.hpp> // SwitchToThread
#include "windows/ipc_sync_wrappers.hpp"
#include "windows/mapped_shared_memory.hpp"
#include "windows/utf_code_conversion.hpp"
#include "murmur3.hpp"
#include "bit_tools.hpp"
#include <windows.h>
#include <boost/log/detail/header.hpp>

//! A suffix used in names of interprocess objects created by the queue.
//! Used as a protection against clashing with user-supplied names of interprocess queues and also to resolve conflicts between queues of different types.
#define BOOST_LOG_IPC_NAMES_AUX_SUFFIX L".3010b9950926463398eee00b35b44651"

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
        //! A flag indicating that the queue is constructed (i.e. the queue is constructed when the value is not 0).
        boost::ipc_atomic< uint32_t > m_initialized;
        //! Number of allocation blocks in the queue.
        const uint32_t m_capacity;
        //! Size of an allocation block, in bytes.
        const size_type m_block_size;
        //! Shared state of the mutex for protecting queue data structures.
        boost::log::ipc::aux::interprocess_mutex::shared_state m_mutex_state;
        //! Shared state of the condition variable used to block writers when the queue is full.
        boost::log::ipc::aux::interprocess_condition_variable::shared_state m_nonfull_queue_state;
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
            // Must be initialized last. m_initialized is zero-initialized initially.
            m_initialized.opaque_add(1u, boost::memory_order_release);
        }

        //! Returns the header structure ABI tag
        static uint32_t get_abi_tag() BOOST_NOEXCEPT
        {
            // This FOURCC identifies the queue type
            boost::log::aux::murmur3_32 hash(boost::log::aux::make_fourcc('r', 'e', 'l', 'q'));

            // This FOURCC identifies the queue implementation
            hash.mix(boost::log::aux::make_fourcc('w', 'n', 't', '5'));
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
            BOOST_LOG_MIX_HEADER_MEMBER(m_initialized);
            BOOST_LOG_MIX_HEADER_MEMBER(m_capacity);
            BOOST_LOG_MIX_HEADER_MEMBER(m_block_size);
            BOOST_LOG_MIX_HEADER_MEMBER(m_mutex_state);
            BOOST_LOG_MIX_HEADER_MEMBER(m_nonfull_queue_state);
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
    //! Shared memory object and mapping
    boost::log::ipc::aux::mapped_shared_memory m_shared_memory;
    //! Queue overflow handling policy
    const overflow_policy m_overflow_policy;
    //! The mask for selecting bits that constitute size values from 0 to (block_size - 1)
    size_type m_block_size_mask;
    //! The number of the bit set in block_size (i.e. log base 2 of block_size)
    uint32_t m_block_size_log2;

    //! Mutex for protecting queue data structures.
    boost::log::ipc::aux::interprocess_mutex m_mutex;
    //! Event used to block readers when the queue is empty.
    boost::log::ipc::aux::interprocess_event m_nonempty_queue;
    //! Condition variable used to block writers when the queue is full.
    boost::log::ipc::aux::interprocess_condition_variable m_nonfull_queue;
    //! The event indicates that stop has been requested
    boost::log::ipc::aux::auto_handle m_stop;

    //! The queue name, as specified by the user
    const object_name m_name;

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
        m_overflow_policy(oflow_policy),
        m_block_size_mask(0u),
        m_block_size_log2(0u),
        m_name(name)
    {
        BOOST_ASSERT(block_size >= block_header::get_header_overhead());
        const std::wstring wname = boost::log::aux::utf8_to_utf16(name.c_str());
        const std::size_t shmem_size = estimate_region_size(capacity, block_size);
        m_shared_memory.create(wname.c_str(), shmem_size, perms);
        m_shared_memory.map();

        create_queue(wname, capacity, block_size, perms);
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
        m_overflow_policy(oflow_policy),
        m_block_size_mask(0u),
        m_block_size_log2(0u),
        m_name(name)
    {
        BOOST_ASSERT(block_size >= block_header::get_header_overhead());
        const std::wstring wname = boost::log::aux::utf8_to_utf16(name.c_str());
        const std::size_t shmem_size = estimate_region_size(capacity, block_size);
        const bool created = m_shared_memory.create_or_open(wname.c_str(), shmem_size, perms);
        m_shared_memory.map();

        if (created)
            create_queue(wname, capacity, block_size, perms);
        else
            adopt_queue(wname, m_shared_memory.size(), perms);
    }

    //! The constructor opens the existing shared memory segment
    implementation
    (
        open_mode::open_only_tag,
        object_name const& name,
        overflow_policy oflow_policy,
        permissions const& perms
    ) :
        m_overflow_policy(oflow_policy),
        m_block_size_mask(0u),
        m_block_size_log2(0u),
        m_name(name)
    {
        const std::wstring wname = boost::log::aux::utf8_to_utf16(name.c_str());
        m_shared_memory.open(wname.c_str());
        m_shared_memory.map();

        adopt_queue(wname, m_shared_memory.size(), perms);
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

        if (!lock_queue())
            return aborted;

        boost::log::ipc::aux::interprocess_mutex::optional_unlock unlock(m_mutex);

        while (true)
        {
            if ((hdr->m_capacity - hdr->m_size) >= block_count)
                break;

            const overflow_policy oflow_policy = m_overflow_policy;
            if (oflow_policy == fail_on_overflow)
                return no_space;
            else if (BOOST_UNLIKELY(oflow_policy == throw_on_overflow))
                BOOST_LOG_THROW_DESCR(capacity_limit_reached, "Interprocess queue is full");

            if (!m_nonfull_queue.wait(unlock, m_stop.get()))
                return aborted;
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

        if (!lock_queue())
            return false;

        boost::log::ipc::aux::interprocess_mutex::auto_unlock unlock(m_mutex);

        if ((hdr->m_capacity - hdr->m_size) < block_count)
            return false;

        enqueue_message(message_data, message_size, block_count);

        return true;
    }

    operation_result receive(receive_handler handler, void* state)
    {
        if (!lock_queue())
            return aborted;

        boost::log::ipc::aux::interprocess_mutex::optional_unlock unlock(m_mutex);

        header* const hdr = get_header();

        while (true)
        {
            if (hdr->m_size > 0u)
                break;

            m_mutex.unlock();
            unlock.disengage();

            if (!m_nonempty_queue.wait(m_stop.get()) || !lock_queue())
                return aborted;

            unlock.engage(m_mutex);
        }

        dequeue_message(handler, state);

        return succeeded;
    }

    bool try_receive(receive_handler handler, void* state)
    {
        if (!lock_queue())
            return false;

        boost::log::ipc::aux::interprocess_mutex::auto_unlock unlock(m_mutex);

        header* const hdr = get_header();
        if (hdr->m_size == 0u)
            return false;

        dequeue_message(handler, state);

        return true;
    }

    void stop_local()
    {
        BOOST_VERIFY(boost::winapi::SetEvent(m_stop.get()) != 0);
    }

    void reset_local()
    {
        BOOST_VERIFY(boost::winapi::ResetEvent(m_stop.get()) != 0);
    }

    void clear()
    {
        m_mutex.lock();
        boost::log::ipc::aux::interprocess_mutex::auto_unlock unlock(m_mutex);
        clear_queue();
    }

private:
    header* get_header() const BOOST_NOEXCEPT
    {
        return static_cast< header* >(m_shared_memory.address());
    }

    static std::size_t estimate_region_size(uint32_t capacity, size_type block_size) BOOST_NOEXCEPT
    {
        return boost::alignment::align_up(sizeof(header), BOOST_LOG_CPU_CACHE_LINE_SIZE) + static_cast< std::size_t >(capacity) * static_cast< std::size_t >(block_size);
    }

    void create_stop_event()
    {
#if BOOST_USE_WINAPI_VERSION >= BOOST_WINAPI_VERSION_WIN6
        boost::winapi::HANDLE_ h = boost::winapi::CreateEventExW
        (
            NULL, // permissions
            NULL, // name
            boost::winapi::CREATE_EVENT_MANUAL_RESET_,
            boost::winapi::SYNCHRONIZE_ | boost::winapi::EVENT_MODIFY_STATE_
        );
#else
        boost::winapi::HANDLE_ h = boost::winapi::CreateEventW
        (
            NULL, // permissions
            true, // manual reset
            false, // initial state
            NULL // name
        );
#endif
        if (BOOST_UNLIKELY(h == NULL))
        {
            boost::winapi::DWORD_ err = boost::winapi::GetLastError();
            BOOST_LOG_THROW_DESCR_PARAMS(boost::log::system_error, "Failed to create an stop event object", (err));
        }

        m_stop.init(h);
    }

    void create_queue(std::wstring const& name, uint32_t capacity, size_type block_size, permissions const& perms)
    {
        // Initialize synchronization primitives before initializing the header as the openers will wait for it to be initialized
        header* const hdr = get_header();
        m_mutex.create((name + BOOST_LOG_IPC_NAMES_AUX_SUFFIX L".mutex").c_str(), &hdr->m_mutex_state, perms);
        m_nonempty_queue.create((name + BOOST_LOG_IPC_NAMES_AUX_SUFFIX L".nonempty_queue_event").c_str(), false, perms);
        m_nonfull_queue.init((name + BOOST_LOG_IPC_NAMES_AUX_SUFFIX L".nonfull_queue_cond_var").c_str(), &hdr->m_nonfull_queue_state, perms);
        create_stop_event();

        new (hdr) header(capacity, block_size);

        init_block_size(block_size);
    }

    void adopt_queue(std::wstring const& name, std::size_t shmem_size, permissions const& perms)
    {
        if (shmem_size < sizeof(header))
            BOOST_LOG_THROW_DESCR(setup_error, "Boost.Log interprocess message queue cannot be opened: shared memory segment size too small");

        // Wait until the mapped region becomes initialized
        header* const hdr = get_header();
        BOOST_CONSTEXPR_OR_CONST unsigned int wait_loops = 1000u, spin_loops = 16u, spins = 16u;
        for (unsigned int i = 0; i < wait_loops; ++i)
        {
            uint32_t initialized = hdr->m_initialized.load(boost::memory_order_acquire);
            if (initialized)
            {
                goto done;
            }

            if (i < spin_loops)
            {
                for (unsigned int j = 0; j < spins; ++j)
                {
                    boost::log::aux::pause();
                }
            }
            else
            {
                boost::winapi::SwitchToThread();
            }
        }

        BOOST_LOG_THROW_DESCR(setup_error, "Boost.Log interprocess message queue cannot be opened: shared memory segment is not initialized by creator for too long");

    done:
        // Check that the queue layout matches the current process ABI
        if (hdr->m_abi_tag != header::get_abi_tag())
            BOOST_LOG_THROW_DESCR(setup_error, "Boost.Log interprocess message queue cannot be opened: the queue ABI is incompatible");

        if (!boost::log::aux::is_power_of_2(hdr->m_block_size))
            BOOST_LOG_THROW_DESCR(setup_error, "Boost.Log interprocess message queue cannot be opened: the queue block size is not a power of 2");

        m_mutex.open((name + BOOST_LOG_IPC_NAMES_AUX_SUFFIX L".mutex").c_str(), &hdr->m_mutex_state);
        m_nonempty_queue.open((name + BOOST_LOG_IPC_NAMES_AUX_SUFFIX L".nonempty_queue_event").c_str());
        m_nonfull_queue.init((name + BOOST_LOG_IPC_NAMES_AUX_SUFFIX L".nonfull_queue_cond_var").c_str(), &hdr->m_nonfull_queue_state, perms);
        create_stop_event();

        init_block_size(hdr->m_block_size);
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

    bool lock_queue()
    {
        return m_mutex.lock(m_stop.get());
    }

    void clear_queue()
    {
        header* const hdr = get_header();
        hdr->m_size = 0u;
        hdr->m_put_pos = 0u;
        hdr->m_get_pos = 0u;
        m_nonfull_queue.notify_all();
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
            m_nonempty_queue.set();
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

        m_nonfull_queue.notify_all();
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
}

BOOST_LOG_API void reliable_message_queue::open(object_name const& name, overflow_policy oflow_policy, permissions const& perms)
{
    BOOST_ASSERT(m_impl == NULL);
    try
    {
        m_impl = new implementation(open_mode::open_only, name, oflow_policy, perms);
    }
    catch (boost::exception& e)
    {
        e << boost::log::ipc::object_name_info(name);
        throw;
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

BOOST_LOG_API void reliable_message_queue::remove(object_name const&)
{
    // System objects are reference counted on Windows, nothing to do here
}

} // namespace ipc

BOOST_LOG_CLOSE_NAMESPACE // namespace log

} // namespace boost

#include <boost/log/detail/footer.hpp>
