/*
 * Copyright (c) 2012 Glen Joseph Fernandes 
 * glenfe at live dot com
 *
 * Distributed under the Boost Software License, 
 * Version 1.0. (See accompanying file LICENSE_1_0.txt 
 * or copy at http://boost.org/LICENSE_1_0.txt)
 */
#ifndef BOOST_SMART_PTR_DETAIL_ALLOCATE_ARRAY_HELPER_HPP
#define BOOST_SMART_PTR_DETAIL_ALLOCATE_ARRAY_HELPER_HPP

#include <boost/type_traits/alignment_of.hpp>

namespace boost {
    namespace detail {
        template<typename A, typename T, typename Y = char>
        class allocate_array_helper;
        template<typename A, typename T, typename Y>
        class allocate_array_helper<A, T[], Y> {
            template<typename A9, typename T9, typename Y9>
            friend class allocate_array_helper;
            typedef typename A::template rebind<Y>   ::other A2;
            typedef typename A::template rebind<char>::other A3;
        public:
            typedef typename A2::value_type      value_type;
            typedef typename A2::pointer         pointer;
            typedef typename A2::const_pointer   const_pointer;
            typedef typename A2::reference       reference;
            typedef typename A2::const_reference const_reference;
            typedef typename A2::size_type       size_type;
            typedef typename A2::difference_type difference_type;
            template<typename U>
            struct rebind {
                typedef allocate_array_helper<A, T[], U> other;
            };
            allocate_array_helper(const A& allocator_, std::size_t size_, T** data_)
                : allocator(allocator_),
                  size(sizeof(T) * size_),
                  data(data_) {
            }
            template<class U>
            allocate_array_helper(const allocate_array_helper<A, T[], U>& other) 
                : allocator(other.allocator),
                  size(other.size),
                  data(other.data) {
            }
            pointer address(reference value) const {
                return allocator.address(value);
            }
            const_pointer address(const_reference value) const {
                return allocator.address(value);
            }
            size_type max_size() const {
                return allocator.max_size();
            }
            pointer allocate(size_type count, const void* value = 0) {
                std::size_t a1 = boost::alignment_of<T>::value;
                std::size_t n1 = count * sizeof(Y) + a1 - 1;
                char*  p1 = A3(allocator).allocate(n1 + size, value);
                char*  p2 = p1 + n1;
                while (std::size_t(p2) % a1 != 0) {
                    p2--;
                }
                *data = reinterpret_cast<T*>(p2);
                return  reinterpret_cast<Y*>(p1);
            }
            void deallocate(pointer memory, size_type count) {
                std::size_t a1 = boost::alignment_of<T>::value;
                std::size_t n1 = count * sizeof(Y) + a1 - 1;
                char*  p1 = reinterpret_cast<char*>(memory);
                A3(allocator).deallocate(p1, n1 + size);
            }
            void construct(pointer memory, const Y& value) {
                allocator.construct(memory, value);
            }
            void destroy(pointer memory) {
                allocator.destroy(memory);
            }
            template<typename U>
            bool operator==(const allocate_array_helper<A, T[], U>& other) const {
                return allocator == other.allocator;
            }
            template<typename U>
            bool operator!=(const allocate_array_helper<A, T[], U>& other) const {
                return !(*this == other); 
            }
        private:
            A2 allocator;
            std::size_t size;
            T** data;
        };
        template<typename A, typename T, std::size_t N, typename Y>
        class allocate_array_helper<A, T[N], Y> {
            template<typename A9, typename T9, typename Y9>
            friend class allocate_array_helper;
            typedef typename A::template rebind<Y>   ::other A2;
            typedef typename A::template rebind<char>::other A3;
        public:
            typedef typename A2::value_type      value_type;
            typedef typename A2::pointer         pointer;
            typedef typename A2::const_pointer   const_pointer;
            typedef typename A2::reference       reference;
            typedef typename A2::const_reference const_reference;
            typedef typename A2::size_type       size_type;
            typedef typename A2::difference_type difference_type;
            template<typename U>
            struct rebind {
                typedef allocate_array_helper<A, T[N], U> other;
            };
            allocate_array_helper(const A& allocator_, T** data_)
                : allocator(allocator_),
                  data(data_) {
            }
            template<class U>
            allocate_array_helper(const allocate_array_helper<A, T[N], U>& other) 
                : allocator(other.allocator),
                  data(other.data) {
            }
            pointer address(reference value) const {
                return allocator.address(value);
            }
            const_pointer address(const_reference value) const {
                return allocator.address(value);
            }
            size_type max_size() const {
                return allocator.max_size();
            }
            pointer allocate(size_type count, const void* value = 0) {
                std::size_t a1 = boost::alignment_of<T>::value;
                std::size_t n1 = count * sizeof(Y) + a1 - 1;
                char*  p1 = A3(allocator).allocate(n1 + N1, value);
                char*  p2 = p1 + n1;
                while (std::size_t(p2) % a1 != 0) {
                    p2--;
                }
                *data = reinterpret_cast<T*>(p2);
                return  reinterpret_cast<Y*>(p1);
            }
            void deallocate(pointer memory, size_type count) {
                std::size_t a1 = boost::alignment_of<T>::value;
                std::size_t n1 = count * sizeof(Y) + a1 - 1;
                char*  p1 = reinterpret_cast<char*>(memory);
                A3(allocator).deallocate(p1, n1 + N1);
            }
            void construct(pointer memory, const Y& value) {
                allocator.construct(memory, value);
            }
            void destroy(pointer memory) {
                allocator.destroy(memory);
            }
            template<typename U>
            bool operator==(const allocate_array_helper<A, T[N], U>& other) const {
                return allocator == other.allocator;
            }
            template<typename U>
            bool operator!=(const allocate_array_helper<A, T[N], U>& other) const {
                return !(*this == other); 
            }
        private:
            enum {
                N1 = N * sizeof(T)
            };
            A2 allocator;
            T** data;
        };
    }
}

#endif
