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

#include <string>

#include <boost/optional.hpp>

#include "mongo/db/index/multikey_paths.h"
#include "mongo/db/operation_context.h"

namespace mongo {

struct MultikeyPathInfo {
    NamespaceString nss;
    std::string indexName;
    MultikeyPaths multikeyPaths;
};

using WorkerMultikeyPathInfo = std::vector<MultikeyPathInfo>;

/**
 * An OperationContext decoration that tracks which indexes should be made multikey. This is used
 * by IndexCatalogEntryImpl::setMultikey() to track what indexes should be set as multikey during
 * secondary oplog application. This both marks if the multikey path information should be tracked
 * instead of set immediately and saves the multikey path information for later if needed.
 */
class MultikeyPathTracker {
public:
    static const OperationContext::Decoration<MultikeyPathTracker> get;

    /**
     * Returns a string representation of MultikeyPaths for logging.
     */
    static std::string dumpMultikeyPaths(const MultikeyPaths& multikeyPaths);

    static void mergeMultikeyPaths(MultikeyPaths* toMergeInto, const MultikeyPaths& newPaths);

    /**
     * Returns whether paths contains only empty sets, i.e., {{}, {}, {}}. This includes the case
     * where the MultikeyPaths vector itself has no elements, e.g., {}.
     */
    static bool isMultikeyPathsTrivial(const MultikeyPaths& paths);

    // Decoration requires a default constructor.
    MultikeyPathTracker() = default;

    /**
     * Appends the provided multikey path information to the list of indexes to set as multikey
     * after the current replication batch finishes.
     * Must call startTrackingMultikeyPathInfo() first.
     */
    void addMultikeyPathInfo(MultikeyPathInfo info);

    /**
     * Returns the multikey path information that has been saved.
     */
    const WorkerMultikeyPathInfo& getMultikeyPathInfo() const;

    /**
     * Returns the multikey path information for the given inputs, or boost::none if none exist.
     */
    const boost::optional<MultikeyPaths> getMultikeyPathInfo(const NamespaceString& nss,
                                                             const std::string& indexName);

    /**
     * Specifies that we should track multikey path information on this MultikeyPathTracker. This is
     * only expected to be called during oplog application on secondaries. We cannot simply check
     * 'canAcceptWritesFor' because background index builds use their own OperationContext and
     * cannot store their multikey path info here.
     */
    void startTrackingMultikeyPathInfo();

    /**
     * Specifies to stop tracking multikey path information.
     */
    void stopTrackingMultikeyPathInfo();

    /**
     * Returns if we've called startTrackingMultikeyPathInfo() and not yet called
     * stopTrackingMultikeyPathInfo().
     */
    bool isTrackingMultikeyPathInfo() const;


private:
    WorkerMultikeyPathInfo _multikeyPathInfo;
    bool _trackMultikeyPathInfo = false;
};

}  // namespace mongo
