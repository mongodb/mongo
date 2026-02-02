//
// immer: immutable data structures for C++
// Copyright (C) 2016, 2017, 2018 Juan Pedro Bolivar Puente
//
// This software is distributed under the Boost Software License, Version 1.0.
// See accompanying file LICENSE or copy at http://boost.org/LICENSE_1_0.txt
//

#pragma once

#include <atomic>
#include <cassert>
#include <cstddef>

namespace immer {

/*!
 * Adaptor that does not release the memory to the parent heap but
 * instead it keeps the memory in a thread-safe global free list.
 *
 * @tparam Size Maximum size of the objects to be allocated.
 * @tparam Base Type of the parent heap.
 */
template <std::size_t Size, std::size_t Limit, typename Base>
struct free_list_heap : Base
{
    struct node_t
    {
        node_t* next;
    };

    static_assert(sizeof(node_t) <= Size,
                  "free_list_heap size must at least fit a pointer");

    using base_t = Base;

    template <typename... Tags>
    static void* allocate(std::size_t size, Tags...)
    {
        assert(size <= Size);

        node_t* n;
        do {
            n = head().data;
            if (!n) {
                auto p = base_t::allocate(Size);
                return p;
            }
        } while (!head().data.compare_exchange_weak(n, n->next));
        head().count.fetch_sub(1u, std::memory_order_relaxed);
        return n;
    }

    template <typename... Tags>
    static void deallocate(std::size_t size, void* data, Tags...)
    {
        assert(size <= Size);

        // we use relaxed, because we are fine with temporarily having
        // a few more/less buffers in free list
        if (head().count.load(std::memory_order_relaxed) >= Limit) {
            base_t::deallocate(Size, data);
        } else {
            auto n = static_cast<node_t*>(data);
            do {
                n->next = head().data;
            } while (!head().data.compare_exchange_weak(n->next, n));
            head().count.fetch_add(1u, std::memory_order_relaxed);
        }
    }

private:
    struct head_t
    {
        std::atomic<node_t*> data;
        std::atomic<std::size_t> count;
    };

    static head_t& head()
    {
        static head_t head_{{nullptr}, {0}};
        return head_;
    }
};

} // namespace immer
