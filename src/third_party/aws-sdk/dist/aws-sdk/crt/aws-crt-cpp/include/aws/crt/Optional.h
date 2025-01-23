#pragma once
/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */
#include <aws/crt/Utility.h>
#include <utility>

namespace Aws
{
    namespace Crt
    {
        /**
         * Custom implementation of an Option type.  std::optional requires C++17
         * @tparam T type of the optional value
         */
        template <typename T> class Optional
        {
          public:
            Optional() : m_value(nullptr) {}
            Optional(const T &val)
            {
                new (m_storage) T(val);
                m_value = reinterpret_cast<T *>(m_storage);
            }

            Optional(T &&val)
            {
                new (m_storage) T(std::forward<T>(val));
                m_value = reinterpret_cast<T *>(m_storage);
            }

            ~Optional()
            {
                if (m_value)
                {
                    m_value->~T();
                }
            }

            template <typename U = T> Optional &operator=(U &&u)
            {
                if (m_value)
                {
                    *m_value = std::forward<U>(u);
                    return *this;
                }

                new (m_storage) T(std::forward<U>(u));
                m_value = reinterpret_cast<T *>(m_storage);

                return *this;
            }

            Optional(const Optional<T> &other)
            {
                if (other.m_value)
                {
                    new (m_storage) T(*other.m_value);
                    m_value = reinterpret_cast<T *>(m_storage);
                }
                else
                {
                    m_value = nullptr;
                }
            }

            Optional(Optional<T> &&other)
            {
                if (other.m_value)
                {
                    new (m_storage) T(std::forward<T>(*other.m_value));
                    m_value = reinterpret_cast<T *>(m_storage);
                }
                else
                {
                    m_value = nullptr;
                }
            }

            template <typename... Args> explicit Optional(Aws::Crt::InPlaceT, Args &&...args)
            {
                new (m_storage) T(std::forward<Args>(args)...);
                m_value = reinterpret_cast<T *>(m_storage);
            }

            Optional &operator=(const Optional &other)
            {
                if (this == &other)
                {
                    return *this;
                }

                if (m_value)
                {
                    if (other.m_value)
                    {
                        *m_value = *other.m_value;
                    }
                    else
                    {
                        m_value->~T();
                        m_value = nullptr;
                    }

                    return *this;
                }

                if (other.m_value)
                {
                    new (m_storage) T(*other.m_value);
                    m_value = reinterpret_cast<T *>(m_storage);
                }

                return *this;
            }

            template <typename U = T> Optional<T> &operator=(const Optional<U> &other)
            {
                if (this == &other)
                {
                    return *this;
                }

                if (m_value)
                {
                    if (other.m_value)
                    {
                        *m_value = *other.m_value;
                    }
                    else
                    {
                        m_value->~T();
                        m_value = nullptr;
                    }

                    return *this;
                }

                if (other.m_value)
                {
                    new (m_storage) T(*other.m_value);
                    m_value = reinterpret_cast<T *>(m_storage);
                }

                return *this;
            }

            template <typename U = T> Optional<T> &operator=(Optional<U> &&other)
            {
                if (this == &other)
                {
                    return *this;
                }

                if (m_value)
                {
                    if (other.m_value)
                    {
                        *m_value = std::forward<U>(*other.m_value);
                    }
                    else
                    {
                        m_value->~T();
                        m_value = nullptr;
                    }

                    return *this;
                }

                if (other.m_value)
                {
                    new (m_storage) T(std::forward<U>(*other.m_value));
                    m_value = reinterpret_cast<T *>(m_storage);
                }

                return *this;
            }

            template <typename... Args> T &emplace(Args &&...args)
            {
                reset();

                new (m_storage) T(std::forward<Args>(args)...);
                m_value = reinterpret_cast<T *>(m_storage);

                return *m_value;
            }

            const T *operator->() const { return m_value; }
            T *operator->() { return m_value; }
            const T &operator*() const & { return *m_value; }
            T &operator*() & { return *m_value; }
            const T &&operator*() const && { return std::move(*m_value); }
            T &&operator*() && { return std::move(*m_value); }

            explicit operator bool() const noexcept { return m_value != nullptr; }
            bool has_value() const noexcept { return m_value != nullptr; }

            T &value() & { return *m_value; }
            const T &value() const & { return *m_value; }

            T &&value() && { return std::move(*m_value); }
            const T &&value() const && { return std::move(*m_value); }

            void reset()
            {
                if (m_value)
                {
                    m_value->~T();
                    m_value = nullptr;
                }
            }

          private:
            alignas(T) char m_storage[sizeof(T)];
            T *m_value;
        };
    } // namespace Crt
} // namespace Aws
