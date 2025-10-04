/**
 *    Copyright (C) 2018-present MongoDB, Inc.
 *
 *    This program is free software: you can redistribute it and/or modify
 *    it under the terms of the Server Side Public License, version 1,
 *    as published by MongoDB, Inc.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    Server Side Public License for more details.
 *
 *    You should have received a copy of the Server Side Public License
 *    along with this program. If not, see
 *    <http://www.mongodb.com/licensing/server-side-public-license>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the Server Side Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */
#pragma once

#include "mongo/stdx/type_traits.h"

#include <type_traits>

namespace mongo {
namespace concept {

    /**
     * The Constructable trait indicates whether `T` is constructible from `Constructible`.
     *
     * RETURNS: true if `T{ std::declval< Constructible >() }` is a valid expression and false
     * otherwise.
     */
    template <typename T, typename Constructible, typename = void>
    struct is_constructible : std::false_type {};

    template <typename T, typename Constructible>
    struct is_constructible<T,
                            Constructible,
                            stdx::void_t<decltype(T{std::declval<Constructible<T>>()})>>
        : std::true_type {};

    /**
     * The Constructable concept models a type which can be passed to a single-argument constructor
     * of `T`. This is not possible to describe in the type `Constructible`.
     *
     * The expression: `T{ std::declval< Constructible< T > >() }` should be valid.
     *
     * This concept is more broadly applicable than `ConvertibleTo`.  `ConvertibleTo` uses implicit
     * conversion, whereas `Constructible` uses direct construction.
     */
    template <typename T>
    struct Constructible {};
}  // namespace concept
}  // namespace mongo
