/*
 * Copyright (c) 2012 Glen Joseph Fernandes
 * glenfe at live dot com
 *
 * Distributed under the Boost Software License,
 * Version 1.0. (See accompanying file LICENSE_1_0.txt
 * or copy at http://boost.org/LICENSE_1_0.txt)
 */
#ifndef BOOST_SMART_PTR_DETAIL_ARRAY_DELETER_HPP
#define BOOST_SMART_PTR_DETAIL_ARRAY_DELETER_HPP

#include <boost/smart_ptr/detail/array_utility.hpp>
#include <boost/smart_ptr/detail/sp_forward.hpp>

namespace boost {
    namespace detail {
        template<typename T>
        class array_deleter;
        template<typename T>
        class array_deleter<T[]> {
        public:
            array_deleter(std::size_t size_)
                : size(size_),
                  object(0) {
            }
            ~array_deleter() {
                if (object) {
                    array_destroy(object, size);
                }
            }
            void init(T* memory) {
                array_init(memory, size);
                object = memory;
            }
#if !defined(BOOST_NO_CXX11_RVALUE_REFERENCES)
            void init(T* memory, T&& value) {
                array_init_value(memory, size, sp_forward<T>(value));
                object = memory;                
            }
#if !defined(BOOST_NO_CXX11_VARIADIC_TEMPLATES)
            template<typename... Args>
            void init(T* memory, Args&&... args) {
                array_init_args(memory, size, sp_forward<Args>(args)...);
                object = memory;
            }
#endif
#endif
            void init_list(T* memory, const T* list) {
                array_init_list(memory, size, list);
                object = memory;
            }
            template<std::size_t M>
            void init_list(T* memory, const T* list) {
                array_init_list<T, M>(memory, size, list);
                object = memory;
            }
            void noinit(T* memory) {
                array_noinit(memory, size);
                object = memory;
            }
            void operator()(const void*) {
                if (object) {
                    array_destroy(object, size);
                    object = 0;
                }
            }
        private:
            std::size_t size;
            T* object;
        };
        template<typename T, std::size_t N>
        class array_deleter<T[N]> {
        public:
            array_deleter()
                : object(0) {
            }
            ~array_deleter() {
                if (object) {
                    array_destroy(object, N);
                }
            }
            void init(T* memory) {
                array_init(memory, N);
                object = memory;
            }
#if !defined(BOOST_NO_CXX11_RVALUE_REFERENCES)
            void init(T* memory, T&& value) {
                array_init_value(memory, N, sp_forward<T>(value));
                object = memory;                
            }
#if !defined(BOOST_NO_CXX11_VARIADIC_TEMPLATES)
            template<typename... Args>
            void init(T* memory, Args&&... args) {
                array_init_args(memory, N, sp_forward<Args>(args)...);
                object = memory;
            }
#endif
#endif
            void init_list(T* memory, const T* list) {
                array_init_list(memory, N, list);
                object = memory;
            }
            template<std::size_t M>
            void init_list(T* memory, const T* list) {
                array_init_list<T, M>(memory, N, list);
                object = memory;
            }
            void noinit(T* memory) {
                array_noinit(memory, N);
                object = memory;
            }
            void operator()(const void*) {
                if (object) {
                    array_destroy(object, N);
                    object = 0;
                }
            }
        private:
            T* object;
        };
    }
}

#endif
