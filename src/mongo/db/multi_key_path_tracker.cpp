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

// IWYU pragma: no_include "boost/container/detail/flat_tree.hpp"
#include <boost/container/flat_set.hpp>
#include <boost/container/small_vector.hpp>
#include <boost/container/vector.hpp>
// IWYU pragma: no_include "boost/intrusive/detail/iterator.hpp"
// IWYU pragma: no_include "boost/move/algo/detail/set_difference.hpp"
// IWYU pragma: no_include "boost/move/detail/iterator_to_raw_pointer.hpp"
#include "mongo/db/multi_key_path_tracker.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/decorable.h"
#include "mongo/util/str.h"

#include <cstddef>
#include <iterator>
#include <sstream>
#include <utility>

#include <boost/move/utility_core.hpp>
#include <boost/none.hpp>
#include <boost/optional/optional.hpp>

namespace mongo {

const OperationContext::Decoration<MultikeyPathTracker> MultikeyPathTracker::get =
    OperationContext::declareDecoration<MultikeyPathTracker>();

// static
std::string MultikeyPathTracker::dumpMultikeyPaths(const MultikeyPaths& multikeyPaths) {
    std::stringstream ss;

    ss << "[ ";
    for (const auto& multikeyComponents : multikeyPaths) {
        ss << "[ ";
        for (const auto& multikeyComponent : multikeyComponents) {
            ss << multikeyComponent << " ";
        }
        ss << "] ";
    }
    ss << "]";

    return ss.str();
}

void MultikeyPathTracker::mergeMultikeyPaths(MultikeyPaths* toMergeInto,
                                             const MultikeyPaths& newPaths) {
    invariant(toMergeInto->size() == newPaths.size(),
              str::stream() << "toMergeInto: " << dumpMultikeyPaths(*toMergeInto)
                            << "; newPaths: " << dumpMultikeyPaths(newPaths));
    for (auto idx = std::size_t(0); idx < toMergeInto->size(); ++idx) {
        toMergeInto->at(idx).insert(newPaths[idx].begin(), newPaths[idx].end());
    }
}

bool MultikeyPathTracker::covers(const MultikeyPaths& parent, const MultikeyPaths& child) {
    for (size_t idx = 0; idx < parent.size(); ++idx) {
        auto& parentPath = parent[idx];
        auto& childPath = child[idx];
        for (auto&& item : childPath) {
            if (parentPath.find(item) == parentPath.end()) {
                return false;
            }
        }
    }
    return true;
}

void MultikeyPathTracker::addMultikeyPathInfo(MultikeyPathInfo info) {
    invariant(_trackMultikeyPathInfo);
    // Merge the `MultikeyPathInfo` input into the accumulated value being tracked for the
    // (collection, index) key.
    for (auto& existingChanges : _multikeyPathInfo) {
        if (existingChanges.nss != info.nss || existingChanges.indexName != info.indexName) {
            continue;
        }

        mergeMultikeyPaths(&existingChanges.multikeyPaths, info.multikeyPaths);
        existingChanges.multikeyMetadataKeys.insert(
            std::make_move_iterator(info.multikeyMetadataKeys.begin()),
            std::make_move_iterator(info.multikeyMetadataKeys.end()));
        return;
    }

    // If an existing entry wasn't found for the (collection, index) input, create a new entry.
    _multikeyPathInfo.emplace_back(info);
}

void MultikeyPathTracker::clear() {
    invariant(!_trackMultikeyPathInfo);
    _multikeyPathInfo.clear();
}

const WorkerMultikeyPathInfo& MultikeyPathTracker::getMultikeyPathInfo() const {
    return _multikeyPathInfo;
}

boost::optional<MultikeyPaths> MultikeyPathTracker::getMultikeyPathInfo(
    const NamespaceString& nss, const std::string& indexName) {
    for (const auto& multikeyPathInfo : _multikeyPathInfo) {
        if (multikeyPathInfo.nss == nss && multikeyPathInfo.indexName == indexName) {
            return multikeyPathInfo.multikeyPaths;
        }
    }

    return boost::none;
}

void MultikeyPathTracker::startTrackingMultikeyPathInfo() {
    _trackMultikeyPathInfo = true;
}

void MultikeyPathTracker::stopTrackingMultikeyPathInfo() {
    _trackMultikeyPathInfo = false;
}

bool MultikeyPathTracker::isTrackingMultikeyPathInfo() const {
    return _trackMultikeyPathInfo;
}

bool MultikeyPathTracker::isEmpty() const {
    return _multikeyPathInfo.empty();
}

}  // namespace mongo
