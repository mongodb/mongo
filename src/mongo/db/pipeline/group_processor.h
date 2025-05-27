/**
 *    Copyright (C) 2023-present MongoDB, Inc.
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

#include "mongo/db/pipeline/group_processor_base.h"
#include "mongo/db/sorter/sorter.h"

#include <memory>
#include <utility>

#include <boost/optional.hpp>

namespace mongo {

/**
 * This class is used by the aggregation framework and streams enterprise module to perform the
 * document processing needed for $group.
 *
 * A caller should call the public methods of this class in the following manner:
 * - For each document, call computeGroupKey() to find its group and then add the document to the
 *   processor using the add().
 * - Once all documents are added to the processor, call readyGroups() to indicate that there are no
 *   more documents to add.
 * - Repeatedly call getNext() to get all aggregated result documents.
 * - Eventually call reset() to reset the processor to its original state.
 */
class GroupProcessor : public GroupProcessorBase {
public:
    GroupProcessor(const boost::intrusive_ptr<ExpressionContext>& expCtx,
                   int64_t maxMemoryUsageBytes);

    /**
     * Adds the given document to the group corresponding to the specified group key.
     */
    void add(const Value& groupKey, const Document& root);

    /**
     * Prepares internal state to start returning fully aggregated groups back to the caller via
     * getNext() calls. Note that add() must not be called after this method is called.
     */
    void readyGroups();

    /**
     * Returns the next aggregated result document. Returns boost::none if there are no more
     * documents to return.
     *
     * Note that this must be called after readyGroups() has already been called once.
     */
    boost::optional<Document> getNext();

    /**
     * Resets the internal state to match the initial state.
     */
    void reset();

    /**
     * Returns true if this GroupProcessor stage used disk during execution and false otherwise.
     */
    bool usedDisk() const {
        return _stats.spillingStats.getSpills() > 0;
    }

    /**
     * Spills the GroupsMap to a new file and empties the map so that subsequent groups can be added
     * to it. Later when the groups need to be returned back to the caller, all groups in all the
     * spilled files are read, merged and returned to the caller.
     */
    void spill();

private:
    boost::optional<Document> getNextSpilled();
    boost::optional<Document> getNextStandard();

    /**
     * Cleans up any pending memory usage. Throws error, if memory usage is above
     * 'maxMemoryUsageBytes' and cannot spill to disk.
     *
     * Returns true, if the caller should spill to disk, false otherwise.
     */
    bool shouldSpillWithAttemptToSaveMemory();

    /**
     * Returns true if the caller should spill to disk every time we have a duplicate id. Returns
     * false otherwise.
     */
    bool shouldSpillOnEveryDuplicateId(bool isNewGroup);

    // Are groups ready to be returned?
    bool _groupsReady{false};

    // Only used when '_spilled' is false.
    GroupProcessorBase::GroupsMap::iterator _groupsIterator{_groups.end()};

    // Tracks the size of the spill file.
    std::unique_ptr<SorterFileStats> _spillStats;
    std::shared_ptr<Sorter<Value, Value>::File> _file;
    std::vector<std::shared_ptr<Sorter<Value, Value>::Iterator>> _sortedFiles;
    bool _spilled{false};

    // Only used when '_spilled' is true.
    std::unique_ptr<Sorter<Value, Value>::Iterator> _sorterIterator;
    std::pair<Value, Value> _firstPartOfNextGroup;
    GroupProcessorBase::Accumulators _currentAccumulators;
};

}  // namespace mongo
