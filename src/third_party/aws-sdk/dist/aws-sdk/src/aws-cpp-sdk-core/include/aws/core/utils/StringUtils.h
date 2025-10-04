/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#pragma once

#include <aws/core/Core_EXPORTS.h>

#include <aws/core/utils/memory/stl/AWSString.h>
#include <aws/core/utils/memory/stl/AWSVector.h>
#include <aws/core/utils/memory/stl/AWSStringStream.h>
#include <aws/common/byte_buf.h>


namespace Aws
{
    namespace Utils
    {
        /**
        * All the things the c++ stdlib is missing for string operations that I needed.
        */
        class AWS_CORE_API StringUtils
        {
        public:
            static void Replace(Aws::String& s, const char* search, const char* replace);


            /**
            * Converts a string to lower case.
            */
            static Aws::String ToLower(const char* source);


            /**
            * Converts a string to upper case.
            */
            static Aws::String ToUpper(const char* source);


            /**
            * Does a caseless comparison of two strings.
            */
            static bool CaselessCompare(const char* value1, const char* value2);


            /**
            * URL encodes a string (uses %20 not + for spaces).
            */
            static Aws::String URLEncode(const char* unsafe);

            static inline Aws::String URLEncode(const Aws::String& unsafe)
            {
              return URLEncode(unsafe.c_str());
            }

            /**
            * Http Clients tend to escape some characters but not all. Escaping all of them causes problems, because the client
            * will also try to escape them.
            * So this only escapes non-ascii characters and the + character
            */
            static Aws::String UTF8Escape(const char* unicodeString, const char* delimiter);

            /**
            * URL encodes a double (if it ends up going to scientific notation) otherwise it just returns it as a string.
            */
            static Aws::String URLEncode(double unsafe);


            /**
            * Decodes a URL encoded string (will handle both encoding schemes for spaces).
            */
            static Aws::String URLDecode(const char* safe);

            enum class SplitOptions
            {
                /**
                 * By default, removes all the empty entries in the vector returned by Split()
                 */
                NOT_SET,
                /**
                 * Deprecated use INCLUDE_EMPTY_SEGMENTS instead.
                 */
                INCLUDE_EMPTY_ENTRIES,
                /**
                 * Include delimiters as empty segments in the split string
                 */
                INCLUDE_EMPTY_SEGMENTS,
            };

            /**
             * @brief Splits a string on a delimiter (empty items are excluded).
             * @param toSplit, the original string to split
             * @param splitOn, the delimiter you want to use.
             */
            static Aws::Vector<Aws::String> Split(const Aws::String& toSplit, char splitOn);

            /**
             * @brief Splits a string on a delimiter.
             * @param toSplit, the original string to split
             * @param splitOn, the delimiter you want to use.
             * @param option, if INCLUDE_EMPTY_ENTRIES, includes empty entries in the result, otherwise removes empty entries.
             */
            static Aws::Vector<Aws::String> Split(const Aws::String& toSplit, char splitOn, SplitOptions option);

            /**
             * @brief Splits a string on a delimiter (empty items are excluded).
             * @param toSplit, the original string to split
             * @param splitOn, the delimiter you want to use.
             * @param numOfTargetParts, how many target parts you want to get, if it is 0, as many as possible.
             */
            static Aws::Vector<Aws::String> Split(const Aws::String& toSplit, char splitOn, size_t numOfTargetParts);

            /**
             * @brief Splits a string on a delimiter.
             * @param toSplit, the original string to split
             * @param splitOn, the delimiter you want to use.
             * @param numOfTargetParts, how many target parts you want to get, if it is 0, as many as possible.
             * @param option, if INCLUDE_EMPTY_ENTRIES, includes empty entries in the result, otherwise removes empty entries.
             */
            static Aws::Vector<Aws::String> Split(const Aws::String& toSplit, char splitOn, size_t numOfTargetParts, SplitOptions option);

            /**
             * Splits a string on delimeter, keeping the delimiter in the string as a empty space.
             * @param toSplit, the original string to split
             * @param splitOn, the delimiter you want to use.
             */
            static Aws::Vector<Aws::String> SplitWithSpaces(const Aws::String& toSplit, char splitOn);

            /**
            * Splits a string on new line characters.
            */
            static Aws::Vector<Aws::String> SplitOnLine(const Aws::String& toSplit);


            /** static Aws::Vector<Aws::String> SplitOnRegex(Aws::String regex);
             *  trim from start
             */
            static Aws::String LTrim(const char* source);


            /**
             * trim from end
             */
            static Aws::String RTrim(const char* source);

            /**
             * trim from both ends
             */
            static Aws::String Trim(const char* source);


            /**
             * convert to int 64
             */
            static long long ConvertToInt64(const char* source);


            /**
             * convert to int 32
             */
            static long ConvertToInt32(const char* source);


            /**
             * convert to bool
             */
            static bool ConvertToBool(const char* source);


            /**
             * convert to double
             */
            static double ConvertToDouble(const char* source);


#ifdef _WIN32
            /**
            * Converts a string to wstring.
            */
            static Aws::WString ToWString(const char* source);

            /**
            * Converts a wstring to string.
            */
            static Aws::String FromWString(const wchar_t* source);
#endif

            /**
             * not all platforms (Android) have std::to_string
             */
            template< typename T >
            static Aws::String to_string(T value)
            {
                Aws::OStringStream os;
                os << value;
                return os.str();
            }

            /**
             * locale agnostic implementation of std::isalnum
             */
            static bool IsAlnum(char c)
            {
                return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9');
            }

            /**
             * Convert an unsigned integer to its hex string in upper case.
             */
            template<typename T, class = typename std::enable_if<std::is_unsigned<T>::value>::type>
            static Aws::String ToHexString(T value)
            {
                if (value == 0)
                {
                    return "0";
                }

                Aws::String s;
                s.reserve(sizeof(value) * 2);
                T r = value;
                while (r > 0)
                {
                    s += "0123456789ABCDEF"[r & 0xf];
                    r >>= 4;
                }

                std::reverse(s.begin(), s.end());
                return s;
            }

            static Aws::String FromByteCursor(aws_byte_cursor cursor)
            {
                return Aws::String(reinterpret_cast<char *>(cursor.ptr), cursor.len);
            }
        };


    } // namespace Utils
} // namespace Aws
