/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#pragma once

#include <aws/core/Core_EXPORTS.h>

#include <aws/core/utils/memory/AWSMemory.h>
#include <aws/core/utils/memory/stl/AWSVector.h>
#include <aws/core/utils/memory/stl/AWSString.h>
#include <aws/crt/Types.h>
#include <memory>
#include <cassert>
#include <cstring>
#include <algorithm>

#ifdef _WIN32

#include <iterator>

#endif // _WIN32

namespace Aws
{
    namespace Utils
    {
        static const char* ARRAY_ALLOCATION_TAG = "Aws::Array";

        /**
         * Safe array class with move and copy semantics.
         */
        template<typename T>
        class Array
        {

        public:
            /**
             * Create new empty array of size arraySize. Default argument is 0. If it is empty then no allocation happens.
             */
            Array(size_t arraySize = 0) :
                m_capacity(arraySize),
                m_length(arraySize),
                m_data(arraySize > 0 ? Aws::MakeUniqueArray<T>(arraySize, ARRAY_ALLOCATION_TAG) : nullptr)
            {
            }

            /**
             * Create new array and initialize it to a raw array
             */
            Array(const T* arrayToCopy, size_t arraySize) :
                m_capacity(arraySize),
                m_length(arraySize),
                m_data(nullptr)
            {
                if (arrayToCopy != nullptr && m_capacity > 0)
                {
                    m_data.reset(Aws::NewArray<T>(m_capacity, ARRAY_ALLOCATION_TAG));
                    std::copy(arrayToCopy, arrayToCopy + arraySize, m_data.get());
                }
            }

            /**
             * Create new array with a pointer and its dimensions.
             */
            Array(size_t capacity,
                  size_t length,
                  UniqueArrayPtr<T> data)
                : m_capacity(capacity),
                  m_length(length),
                  m_data(std::move(data))
            {
            }

            /**
             * Merge multiple arrays into one
             */
            Array(Aws::Vector<Array*>&& toMerge)
            {
                size_t totalSize = 0;
                for(auto& array : toMerge)
                {
                    totalSize += array->m_length;
                }

                m_capacity = totalSize;
                m_data.reset(Aws::NewArray<T>(m_capacity, ARRAY_ALLOCATION_TAG));

                size_t location = 0;
                for(auto& arr : toMerge)
                {
                    if(arr->m_capacity > 0 && arr->m_data)
                    {
                        size_t arraySize = arr->m_length;
                        std::copy(arr->m_data.get(), arr->m_data.get() + arraySize, m_data.get() + location);
                        location += arraySize;
                    }
                }
                m_length = location;
            }

            Array(const Array& other)
            {
                m_capacity = other.m_capacity;
                m_length = other.m_length;
                m_data = nullptr;

                if (m_capacity > 0)
                {
                    m_data.reset(Aws::NewArray<T>(m_capacity, ARRAY_ALLOCATION_TAG));
                    std::copy(other.m_data.get(), other.m_data.get() + other.m_capacity, m_data.get());
                }
            }

            //move c_tor
            Array(Array&& other) noexcept:
                m_capacity(other.m_capacity),
                m_length(other.m_length),
                m_data(std::move(other.m_data))
            {
                other.m_capacity = 0;
                other.m_data = nullptr;
            }

            virtual ~Array() = default;

            Array& operator=(const Array& other)
            {
                if (this == &other)
                {
                    return *this;
                }

                m_capacity = other.m_capacity;
                m_length = other.m_length;
                m_data = nullptr;

                if (m_capacity > 0)
                {
                    m_data.reset(Aws::NewArray<T>(m_capacity, ARRAY_ALLOCATION_TAG));
                    std::copy(other.m_data.get(), other.m_data.get() + other.m_length, m_data.get());
                }

                return *this;
            }

            Array& operator=(Array&& other) noexcept
            {
                m_capacity = other.m_capacity;
                m_length = other.m_length;
                m_data = std::move(other.m_data);

                return *this;
            }

            Array(const Aws::String& string):
                m_capacity(string.capacity()),
                m_length(string.length()),
                m_data(nullptr)
            {
                m_data.reset(Aws::NewArray<unsigned char>(m_capacity, ARRAY_ALLOCATION_TAG));
                std::copy(string.c_str(), string.c_str() + string.length(), m_data.get());
            }

            bool operator==(const Array& other) const
            {
                if (this == &other)
                    return true;

                if (m_capacity == 0 && other.m_capacity == 0)
                {
                    return true;
                }

                if (m_length != other.m_length)
                {
                    return false;
                }

                if (m_length == other.m_length && m_capacity == other.m_capacity && m_data && other.m_data)
                {
                    for (unsigned i = 0; i < m_length; ++i)
                    {
                        if (m_data.get()[i] != other.m_data.get()[i])
                            return false;
                    }

                    return true;
                }

                return false;
            }

            bool operator!=(const Array& other) const
            {
                return !(*this == other);
            }           

            T const& GetItem(size_t index) const
            {
                assert(index < m_length);
                return m_data.get()[index];
            }

            T& GetItem(size_t index)
            {
                assert(index < m_length);
                return m_data.get()[index];
            }

            T& operator[](size_t index)
            {
                return GetItem(index);
            }

            T const& operator[](size_t index) const
            {
                return GetItem(index);
            }

            inline size_t GetLength() const
            {
                return m_length;
            }

            inline size_t GetSize() const
            {
                return m_capacity;
            }

            inline T* GetUnderlyingData() const
            {
                return m_data.get();
            }

            inline void SetLength(size_t len)
            {
                m_length = len;
            }

        protected:
            size_t m_capacity = 0;
            size_t m_length = 0;
            Aws::UniqueArrayPtr<T> m_data;
        };

        typedef Array<unsigned char> ByteBuffer;

        /**
         * Buffer for cryptographic operations. It zeroes itself back out upon deletion. Everything else is identical
         * to byte buffer.
         */
        class AWS_CORE_API CryptoBuffer : public ByteBuffer
        {
        public:
            CryptoBuffer(size_t arraySize = 0) : ByteBuffer(arraySize) {}
            CryptoBuffer(const unsigned char* arrayToCopy, size_t arraySize) : ByteBuffer(arrayToCopy, arraySize) {}
            CryptoBuffer(Aws::Vector<ByteBuffer*>&& toMerge) : ByteBuffer(std::move(toMerge)) {}
            CryptoBuffer(const ByteBuffer& other) : ByteBuffer(other) {}
            CryptoBuffer(const CryptoBuffer& other) : ByteBuffer(other) {}
            CryptoBuffer(CryptoBuffer&& other) : ByteBuffer(std::move(other)) {}
            CryptoBuffer& operator=(const CryptoBuffer& other) { Zero(); ByteBuffer::operator=(other); return *this; }
            CryptoBuffer& operator=(CryptoBuffer&& other) { Zero(); ByteBuffer::operator=(std::move(other)); return *this; }

            CryptoBuffer(Crt::ByteBuf&& other) noexcept : ByteBuffer(
                other.len,
                other.len,
                nullptr)
            {
                // Crt::ByteBuf must be allocated using SDK not CRT allocator
                assert(get_aws_allocator() == other.allocator);
                m_data.reset(other.buffer);
                other.capacity = 0;
                other.len = 0;
                other.allocator = nullptr;
                other.buffer = nullptr;
            }

            CryptoBuffer& operator=(Crt::ByteBuf&& other) noexcept
            {
                // Crt::ByteBuf must be allocated using SDK not CRT allocator
                assert(get_aws_allocator() == other.allocator);
                m_capacity = other.len;
                m_length = other.len;
                m_data.reset(other.buffer);
                other.capacity = 0;
                other.len = 0;
                other.allocator = nullptr;
                other.buffer = nullptr;
                return *this;
            }

            bool operator==(const CryptoBuffer& other) const { return ByteBuffer::operator==(other); }
            bool operator!=(const CryptoBuffer& other) const { return ByteBuffer::operator!=(other); }

            ~CryptoBuffer() override { Zero(); }

            Array<CryptoBuffer> Slice(size_t sizeOfSlice) const;
            CryptoBuffer& operator^(const CryptoBuffer& operand);
            void Zero();
        };

    } // namespace Utils
} // namespace Aws
