// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/stdx/trusted_hasher.h"
#include "mongo/stdx/unordered_map.h"
#include "mongo/stdx/unordered_set.h"
#include "mongo/util/modules.h"

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
