#pragma once
/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */
#include <aws/common/common.h>
#include <aws/crt/Exports.h>
#include <aws/crt/Optional.h>
#include <aws/crt/StlAllocator.h>
#include <aws/crt/StringView.h>
#include <aws/io/socket.h>
#include <aws/mqtt/mqtt.h>
#include <functional>
#include <list>
#include <map>
#include <sstream>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

struct aws_byte_buf;
struct aws_byte_cursor;
struct aws_socket_options;

namespace Aws
{
    namespace Crt
    {
        using ByteBuf = aws_byte_buf;
        using ByteCursor = aws_byte_cursor;

        namespace Io
        {
            using IStream = std::basic_istream<char, std::char_traits<char>>;
        } // namespace Io

        namespace Mqtt
        {
            using QOS = aws_mqtt_qos;
            using ReturnCode = aws_mqtt_connect_return_code;
        } // namespace Mqtt

        template <typename T> class StlAllocator;
        using String = std::basic_string<char, std::char_traits<char>, StlAllocator<char>>;
        using StringStream = std::basic_stringstream<char, std::char_traits<char>, StlAllocator<char>>;
        template <typename K, typename V> using Map = std::map<K, V, std::less<K>, StlAllocator<std::pair<const K, V>>>;
        template <typename K, typename V>
        using UnorderedMap =
            std::unordered_map<K, V, std::hash<K>, std::equal_to<K>, StlAllocator<std::pair<const K, V>>>;
        template <typename K, typename V>
        using MultiMap = std::multimap<K, V, std::less<K>, StlAllocator<std::pair<const K, V>>>;
        template <typename T> using Vector = std::vector<T, StlAllocator<T>>;
        template <typename T> using List = std::list<T, StlAllocator<T>>;

        AWS_CRT_CPP_API ByteBuf ByteBufFromCString(const char *str) noexcept;
        AWS_CRT_CPP_API ByteBuf ByteBufFromEmptyArray(const uint8_t *array, size_t len) noexcept;
        AWS_CRT_CPP_API ByteBuf ByteBufFromArray(const uint8_t *array, size_t capacity) noexcept;
        AWS_CRT_CPP_API ByteBuf ByteBufNewCopy(Allocator *alloc, const uint8_t *array, size_t len);
        AWS_CRT_CPP_API ByteBuf ByteBufInit(Allocator *alloc, size_t len);
        AWS_CRT_CPP_API void ByteBufDelete(ByteBuf &);

        AWS_CRT_CPP_API ByteCursor ByteCursorFromCString(const char *str) noexcept;
        AWS_CRT_CPP_API ByteCursor ByteCursorFromString(const Crt::String &str) noexcept;
        AWS_CRT_CPP_API ByteCursor ByteCursorFromStringView(const Crt::StringView &str) noexcept;
        AWS_CRT_CPP_API ByteCursor ByteCursorFromByteBuf(const ByteBuf &) noexcept;
        AWS_CRT_CPP_API ByteCursor ByteCursorFromArray(const uint8_t *array, size_t len) noexcept;

        AWS_CRT_CPP_API Vector<uint8_t> Base64Decode(const String &decode) noexcept;
        AWS_CRT_CPP_API String Base64Encode(const Vector<uint8_t> &encode) noexcept;

        template <typename RawType, typename TargetType> using TypeConvertor = std::function<TargetType(RawType)>;

        /**
         * Template function to convert an aws_array_list of RawType to a C++ like Vector of TargetType.
         * A conversion function should be provided to do the type conversion
         */
        template <typename RawType, typename TargetType>
        Vector<TargetType> ArrayListToVector(const aws_array_list *array, TypeConvertor<RawType, TargetType> conv)
        {
            Vector<TargetType> v;
            size_t cnt = aws_array_list_length(array);
            for (size_t i = 0; i < cnt; i++)
            {
                RawType t;
                aws_array_list_get_at(array, &t, i);
                v.emplace_back(conv(t));
            }
            return v;
        }

        /**
         * Template function to convert an aws_array_list of RawType to a C++ like Vector of TargetType.
         * This template assumes a direct constructor: TargetType(RawType) is available
         */
        template <typename RawType, typename TargetType>
        Vector<TargetType> ArrayListToVector(const aws_array_list *array)
        {
            Vector<TargetType> v;
            size_t cnt = aws_array_list_length(array);
            for (size_t i = 0; i < cnt; i++)
            {
                RawType t;
                aws_array_list_get_at(array, &t, i);
                v.emplace_back(TargetType(t));
            }
            return v;
        }

        /**
         * Template function to convert an aws_array_list of Type to a C++ like Vector of Type.
         */
        template <typename Type> Vector<Type> ArrayListToVector(const aws_array_list *array)
        {
            Vector<Type> v;
            size_t cnt = aws_array_list_length(array);
            for (size_t i = 0; i < cnt; i++)
            {
                Type t;
                aws_array_list_get_at(array, &t, i);
                v.emplace_back(t);
            }
            return v;
        }

        AWS_CRT_CPP_API inline StringView ByteCursorToStringView(const ByteCursor &bc)
        {
            return StringView(reinterpret_cast<char *>(bc.ptr), bc.len);
        }

        AWS_CRT_CPP_API inline ByteCursor StringViewToByteCursor(const StringView &sv)
        {
            ByteCursor bc;
            bc.ptr = (uint8_t *)(sv.data());
            bc.len = sv.size();
            return bc;
        }

        template <typename T> void Delete(T *t, Allocator *allocator)
        {
            t->~T();
            aws_mem_release(allocator, t);
        }

        template <typename T, typename... Args> T *New(Allocator *allocator, Args &&...args)
        {
            T *t = reinterpret_cast<T *>(aws_mem_acquire(allocator, sizeof(T)));
            if (!t)
                return nullptr;
            return new (t) T(std::forward<Args>(args)...);
        }

        template <typename T, typename... Args> std::shared_ptr<T> MakeShared(Allocator *allocator, Args &&...args)
        {
            T *t = reinterpret_cast<T *>(aws_mem_acquire(allocator, sizeof(T)));
            if (!t)
                return nullptr;
            new (t) T(std::forward<Args>(args)...);

            return std::shared_ptr<T>(t, [allocator](T *obj) { Delete(obj, allocator); });
        }

        template <typename T> using ScopedResource = std::unique_ptr<T, std::function<void(T *)>>;

    } // namespace Crt
} // namespace Aws
