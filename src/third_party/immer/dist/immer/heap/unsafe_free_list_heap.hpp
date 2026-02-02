//
// immer: immutable data structures for C++
// Copyright (C) 2016, 2017, 2018 Juan Pedro Bolivar Puente
//
// This software is distributed under the Boost Software License, Version 1.0.
// See accompanying file LICENSE or copy at http://boost.org/LICENSE_1_0.txt
//

#pragma once

#include <immer/config.hpp>

#include <cassert>
#include <cstddef>

namespace immer {
namespace detail {

template <typename Heap>
struct unsafe_free_list_storage
{
    struct node_t
    {
        node_t* next;
    };

    struct head_t
    {
        node_t* data;
        std::size_t count;
    };

    static head_t& head()
    {
        static head_t head_{nullptr, 0};
        return head_;
    }
};

template <template <class> class Storage,
          std::size_t Size,
          std::size_t Limit,
          typename Base>
class unsafe_free_list_heap_impl : Base
{
    using storage = Storage<unsafe_free_list_heap_impl>;
    using node_t  = typename storage::node_t;

public:
    using base_t = Base;

    static_assert(sizeof(node_t) <= Size,
                  "unsafe_free_list_heap size must at least fit a pointer");

    template <typename... Tags>
    static void* allocate(std::size_t size, Tags...)
    {
        assert(size <= Size);

        auto n = storage::head().data;
        if (!n) {
            auto p = base_t::allocate(Size);
            return p;
        }
        --storage::head().count;
        storage::head().data = n->next;
        return n;
    }

    template <typename... Tags>
    static void deallocate(std::size_t size, void* data, Tags...)
    {
        assert(size <= Size);

        if (storage::head().count >= Limit)
            base_t::deallocate(Size, data);
        else {
            auto n               = static_cast<node_t*>(data);
            n->next              = storage::head().data;
            storage::head().data = n;
            ++storage::head().count;
        }
    }

    static void clear()
    {
        while (storage::head().data) {
            auto n = storage::head().data->next;
            base_t::deallocate(Size, storage::head().data);
            storage::head().data = n;
            --storage::head().count;
        }
    }
};

} // namespace detail

/*!
 * Adaptor that does not release the memory to the parent heap but
 * instead it keeps the memory in a global free list that **is not
 * thread-safe**.
 *
 * @tparam Size  Maximum size of the objects to be allocated.
 * @tparam Limit Maximum number of elements to keep in the free list.
 * @tparam Base  Type of the parent heap.
 */
template <std::size_t Size, std::size_t Limit, typename Base>
struct unsafe_free_list_heap
    : detail::unsafe_free_list_heap_impl<detail::unsafe_free_list_storage,
                                         Size,
                                         Limit,
                                         Base>
{};

} // namespace immer
