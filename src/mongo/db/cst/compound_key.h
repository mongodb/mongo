/**
 *    Copyright (C) 2020-present MongoDB, Inc.
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

#include "mongo/platform/basic.h"

#include <memory>

namespace mongo {

struct CNode;

// This indicates compound inclusion projection. Leaves for these subtrees are NonZeroKeys and
// ZeroKeys.
struct CompoundInclusionKey final {
    CompoundInclusionKey() = default;
    explicit CompoundInclusionKey(CNode cNode);
    CompoundInclusionKey(std::unique_ptr<CNode> obj) : obj{std::move(obj)} {}
    CompoundInclusionKey(CompoundInclusionKey&&) = default;
    CompoundInclusionKey(const CompoundInclusionKey& other)
        : obj(std::make_unique<CNode>(*other.obj)) {}
    CompoundInclusionKey& operator=(CompoundInclusionKey&&) = default;
    CompoundInclusionKey& operator=(const CompoundInclusionKey& other) {
        obj = std::make_unique<CNode>(*other.obj);
        return *this;
    }
    std::unique_ptr<CNode> obj;
};
// This indicates compound exclusion projection. Leaves for these subtrees are NonZeroKeys and
// ZeroKeys.
struct CompoundExclusionKey final {
    CompoundExclusionKey() = default;
    explicit CompoundExclusionKey(CNode cNode);
    CompoundExclusionKey(std::unique_ptr<CNode> obj) : obj{std::move(obj)} {}
    CompoundExclusionKey(CompoundExclusionKey&&) = default;
    CompoundExclusionKey(const CompoundExclusionKey& other)
        : obj(std::make_unique<CNode>(*other.obj)) {}
    CompoundExclusionKey& operator=(CompoundExclusionKey&&) = default;
    CompoundExclusionKey& operator=(const CompoundExclusionKey& other) {
        obj = std::make_unique<CNode>(*other.obj);
        return *this;
    }
    std::unique_ptr<CNode> obj;
};
// This indicates inconsitent compound exclusion projection. This type of projection is disallowed
// and will produce an error in $project.
struct CompoundInconsistentKey final {
    CompoundInconsistentKey() = default;
    explicit CompoundInconsistentKey(CNode cNode);
    CompoundInconsistentKey(std::unique_ptr<CNode> obj) : obj{std::move(obj)} {}
    CompoundInconsistentKey(CompoundInconsistentKey&&) = default;
    CompoundInconsistentKey(const CompoundInconsistentKey& other)
        : obj(std::make_unique<CNode>(*other.obj)) {}
    CompoundInconsistentKey& operator=(CompoundInconsistentKey&&) = default;
    CompoundInconsistentKey& operator=(const CompoundInconsistentKey& other) {
        obj = std::make_unique<CNode>(*other.obj);
        return *this;
    }
    std::unique_ptr<CNode> obj;
};
}  // namespace mongo
