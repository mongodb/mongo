//
// Copyright (c) 2010 Artyom Beilis (Tonkikh)
//
// Distributed under the Boost Software License, Version 1.0.
// https://www.boost.org/LICENSE_1_0.txt

#ifndef BOOST_LOCALE_HOLD_PTR_H
#define BOOST_LOCALE_HOLD_PTR_H

#include <boost/locale/config.hpp>
#include <boost/core/exchange.hpp>

namespace boost { namespace locale {
    /// \brief a smart pointer similar to std::unique_ptr but the
    /// underlying object has the same constness as the pointer itself (unlike an ordinary pointer).
    template<typename T>
    class hold_ptr {
    public:
        /// Create new empty pointer
        hold_ptr() : ptr_(nullptr) {}

        /// Create a pointer that holds \a v, ownership is transferred to smart pointer
        explicit hold_ptr(T* v) : ptr_(v) {}

        /// Destroy smart pointer and the object it owns.
        ~hold_ptr() { delete ptr_; }

        // Non-copyable
        hold_ptr(const hold_ptr&) = delete;
        hold_ptr& operator=(const hold_ptr&) = delete;
        // Movable
        hold_ptr(hold_ptr&& other) noexcept : ptr_(exchange(other.ptr_, nullptr)) {}
        hold_ptr& operator=(hold_ptr&& other) noexcept
        {
            swap(other);
            return *this;
        }

        /// Get a const pointer to the object
        T const* get() const { return ptr_; }
        /// Get a mutable pointer to the object
        T* get() { return ptr_; }

        /// Explicitly convertible to bool. Returns: get() != nullptr
        explicit operator bool() const { return ptr_ != nullptr; }

        /// Get a const reference to the object
        T const& operator*() const { return *ptr_; }
        /// Get a mutable reference to the object
        T& operator*() { return *ptr_; }

        /// Get a const pointer to the object
        T const* operator->() const { return ptr_; }
        /// Get a mutable pointer to the object
        T* operator->() { return ptr_; }

        /// Transfer ownership of the pointer to user
        T* release() { return exchange(ptr_, nullptr); }

        /// Set new value to pointer, previous object is destroyed, ownership of new object is transferred
        void reset(T* p = nullptr)
        {
            if(ptr_)
                delete ptr_;
            ptr_ = p;
        }

        /// Swap two pointers
        void swap(hold_ptr& other) noexcept { ptr_ = exchange(other.ptr_, ptr_); }

    private:
        T* ptr_;
    };

}} // namespace boost::locale

#endif
