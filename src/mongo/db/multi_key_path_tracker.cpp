// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

// IWYU pragma: no_include "boost/container/detail/flat_tree.hpp"
#include <boost/container/flat_set.hpp>
#include <boost/container/vector.hpp>
// IWYU pragma: no_include "boost/intrusive/detail/iterator.hpp"
// IWYU pragma: no_include "boost/move/algo/detail/set_difference.hpp"
// IWYU pragma: no_include "boost/move/detail/iterator_to_raw_pointer.hpp"
#include "mongo/db/multi_key_path_tracker.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/decorable.h"
#include "mongo/util/str.h"

#include <algorithm>
#include <cstddef>
#include <iterator>
#include <sstream>

#include <boost/none.hpp>
#include <boost/optional/optional.hpp>

namespace mongo {

const OperationContext::Decoration<MultikeyPathTracker> MultikeyPathTracker::get =
    OperationContext::declareDecoration<MultikeyPathTracker>();

bool MultikeyPathInfo::sameIndexAndCollectionAtTime(const MultikeyPathInfo& info) const {
    return collectionUUID == info.collectionUUID && indexName == info.indexName &&
        earliestTimestamp == info.earliestTimestamp;
}

void MultikeyPathInfo::mergePathsAndKeys(MultikeyPathInfo&& info) {
    invariant(sameIndexAndCollectionAtTime(info));

    if (multikeyPaths.empty()) {
        multikeyPaths = std::move(info.multikeyPaths);
    } else if (!info.multikeyPaths.empty()) {
        MultikeyPathTracker::mergeMultikeyPaths(&multikeyPaths, info.multikeyPaths);
    }
    multikeyMetadataKeys.insert(std::make_move_iterator(info.multikeyMetadataKeys.begin()),
                                std::make_move_iterator(info.multikeyMetadataKeys.end()));
}

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
WorkerMultikeyPathInfo MultikeyPathTracker::sortByTimestamp() const {
    WorkerMultikeyPathInfo sortedPathInfo;
    sortedPathInfo.reserve(_multikeyPathInfo.size());
    for (const MultikeyPathInfo& info : _multikeyPathInfo) {
        sortedPathInfo.push_back(info);
    }
    std::stable_sort(sortedPathInfo.begin(),
                     sortedPathInfo.end(),
                     [](const MultikeyPathInfo& lhs, const MultikeyPathInfo& rhs) {
                         return lhs.earliestTimestamp < rhs.earliestTimestamp;
                     });
    return sortedPathInfo;
}

void MultikeyPathTracker::addMultikeyPathInfo(MultikeyPathInfo&& info) {
    invariant(_trackMultikeyPathInfo);
    // Add entries to _multikeyPathInfo keyed by (collection UUID, index, timestamp). Entries that
    // share the same (uuid, index, timestamp) are merged immediately because they represent the
    // same catalog write made by the primary.
    for (auto& existingChanges : _multikeyPathInfo) {
        if (!existingChanges.sameIndexAndCollectionAtTime(info)) {
            continue;
        }
        existingChanges.mergePathsAndKeys(std::move(info));
        return;
    }

    // No entry for this write yet; create a new one.
    _multikeyPathInfo.push_back(std::move(info));
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
    invariant(!_trackMultikeyPathInfo);
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
