/**
 *    Copyright (C) 2022-present MongoDB, Inc.
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

#include "mongo/stdx/trusted_hasher.h"
#include "mongo/stdx/unordered_map.h"
#include "mongo/stdx/unordered_set.h"

#include <unordered_map>
#include <unordered_set>

namespace mongo::abt::opt {

enum class ContainerImpl { STD, STDX };

// For debugging, switch between STD and STDX containers.
inline constexpr ContainerImpl kContainerImpl = ContainerImpl::STDX;

template <ContainerImpl>
struct OptContainers {};

template <>
struct OptContainers<ContainerImpl::STDX> {
    template <class K>
    using Hasher = mongo::DefaultHasher<K>;

    template <class T, class H, typename... Args>
    using unordered_set = stdx::unordered_set<T, H, Args...>;
    template <class K, class V, class H, typename... Args>
    using unordered_map = stdx::unordered_map<K, V, H, Args...>;
};

template <>
struct OptContainers<ContainerImpl::STD> {
    template <class K>
    using Hasher = std::hash<K>;

    template <class T, class H, typename... Args>
    using unordered_set = std::unordered_set<T, H, Args...>;  // NOLINT
    template <class K, class V, class H, typename... Args>
    using unordered_map = std::unordered_map<K, V, H, Args...>;  // NOLINT
};

using ActiveContainers = OptContainers<kContainerImpl>;

template <class T, class H = ActiveContainers::Hasher<T>, typename... Args>
using unordered_set = ActiveContainers::unordered_set<T, H, Args...>;

template <class K, class V, class H = ActiveContainers::Hasher<K>, typename... Args>
using unordered_map = ActiveContainers::unordered_map<K, V, H, Args...>;

}  // namespace mongo::abt::opt
