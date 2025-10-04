#pragma once
/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

namespace Aws
{
    namespace Crt
    {
        /**
         * Custom implementation of an in_place type tag for constructor parameter list
         */
        struct InPlaceT
        {
            explicit InPlaceT() = default;
        };
        static constexpr InPlaceT InPlace{};

        template <typename T> struct InPlaceTypeT
        {
            explicit InPlaceTypeT() = default;
        };
        /** Variable templates are only available since C++14
         *  Use a dummy object "Aws::Crt::InPlaceTypeT<T>() in-place instead in C++11"*/
#if defined(__cplusplus) && __cplusplus > 201103L //
        template <class T> static constexpr InPlaceTypeT<T> InPlaceType{};
#endif

    } // namespace Crt
} // namespace Aws
