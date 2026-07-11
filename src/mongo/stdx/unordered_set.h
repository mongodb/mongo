// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/stdx/trusted_hasher.h"
#include "mongo/util/modules.h"

#include <absl/container/node_hash_set.h>

namespace [[MONGO_MOD_PUBLIC]] mongo {
namespace stdx {

template <class Key, class Hasher = DefaultHasher<Key>, typename... Args>
using unordered_set = absl::node_hash_set<Key, EnsureTrustedHasher<Hasher, Key>, Args...>;

}  // namespace stdx
}  // namespace mongo
