#ifndef BOOST_UNORDERED_DETAIL_FOA_RW_SPINLOCK_HPP_INCLUDED
#define BOOST_UNORDERED_DETAIL_FOA_RW_SPINLOCK_HPP_INCLUDED

// Copyright 2023 Peter Dimov
// Distributed under the Boost Software License, Version 1.0.
// https://www.boost.org/LICENSE_1_0.txt

#include <boost/core/yield_primitives.hpp>
#include <atomic>
#include <cstdint>

namespace boost{
namespace unordered{
namespace detail{
namespace foa{

class rw_spinlock
{
private:

    // bit 31: locked exclusive
    // bit 30: writer pending
    // bit 29..0: reader lock count

    static constexpr std::uint32_t locked_exclusive_mask = 1u << 31; // 0x8000'0000
    static constexpr std::uint32_t writer_pending_mask = 1u << 30; // 0x4000'0000
    static constexpr std::uint32_t reader_lock_count_mask = writer_pending_mask - 1; // 0x3FFF'FFFF

    std::atomic<std::uint32_t> state_ = {};

private:

    // Effects: Provides a hint to the implementation that the current thread
    //          has been unable to make progress for k+1 iterations.

    static void yield( unsigned k ) noexcept
    {
        unsigned const sleep_every = 1024; // see below

        k %= sleep_every;

        if( k < 5 )
        {
            // Intel recommendation from the Optimization Reference Manual
            // Exponentially increase number of PAUSE instructions each
            // iteration until reaching a maximum which is approximately
            // one timeslice long (2^4 == 16 in our case)

            unsigned const pause_count = 1u << k;

            for( unsigned i = 0; i < pause_count; ++i )
            {
                boost::core::sp_thread_pause();
            }
        }
        else if( k < sleep_every - 1 )
        {
            // Once the maximum number of PAUSE instructions is reached,
            // we switch to yielding the timeslice immediately

            boost::core::sp_thread_yield();
        }
        else
        {
            // After `sleep_every` iterations of no progress, we sleep,
            // to avoid a deadlock if a lower priority thread has the lock

            boost::core::sp_thread_sleep();
        }
    }

public:

    bool try_lock_shared() noexcept
    {
        std::uint32_t st = state_.load( std::memory_order_relaxed );

        if( st >= reader_lock_count_mask )
        {
            // either bit 31 set, bit 30 set, or reader count is max
            return false;
        }

        std::uint32_t newst = st + 1;
        return state_.compare_exchange_strong( st, newst, std::memory_order_acquire, std::memory_order_relaxed );
    }

    void lock_shared() noexcept
    {
        for( unsigned k = 0; ; ++k )
        {
            std::uint32_t st = state_.load( std::memory_order_relaxed );

            if( st < reader_lock_count_mask )
            {
                std::uint32_t newst = st + 1;
                if( state_.compare_exchange_weak( st, newst, std::memory_order_acquire, std::memory_order_relaxed ) ) return;
            }

            yield( k );
        }
    }

    void unlock_shared() noexcept
    {
        // pre: locked shared, not locked exclusive

        state_.fetch_sub( 1, std::memory_order_release );

        // if the writer pending bit is set, there's a writer waiting
        // let it acquire the lock; it will clear the bit on unlock
    }

    bool try_lock() noexcept
    {
        std::uint32_t st = state_.load( std::memory_order_relaxed );

        if( st & locked_exclusive_mask )
        {
            // locked exclusive
            return false;
        }

        if( st & reader_lock_count_mask )
        {
            // locked shared
            return false;
        }

        std::uint32_t newst = locked_exclusive_mask;
        return state_.compare_exchange_strong( st, newst, std::memory_order_acquire, std::memory_order_relaxed );
    }

    void lock() noexcept
    {
        for( unsigned k = 0; ; ++k )
        {
            std::uint32_t st = state_.load( std::memory_order_relaxed );

            if( st & locked_exclusive_mask )
            {
                // locked exclusive, spin
            }
            else if( ( st & reader_lock_count_mask ) == 0 )
            {
                // not locked exclusive, not locked shared, try to lock

                std::uint32_t newst = locked_exclusive_mask;
                if( state_.compare_exchange_weak( st, newst, std::memory_order_acquire, std::memory_order_relaxed ) ) return;
            }
            else if( st & writer_pending_mask )
            {
                // writer pending bit already set, nothing to do
            }
            else
            {
                // locked shared, set writer pending bit

                std::uint32_t newst = st | writer_pending_mask;
                state_.compare_exchange_weak( st, newst, std::memory_order_relaxed, std::memory_order_relaxed );
            }

            yield( k );
        }
    }

    void unlock() noexcept
    {
        // pre: locked exclusive, not locked shared
        state_.store( 0, std::memory_order_release );
    }
};

} /* namespace foa */
} /* namespace detail */
} /* namespace unordered */
} /* namespace boost */

#endif // BOOST_UNORDERED_DETAIL_FOA_RW_SPINLOCK_HPP_INCLUDED
