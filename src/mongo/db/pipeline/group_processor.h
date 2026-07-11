// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/db/pipeline/group_processor_base.h"
#include "mongo/db/sorter/file.h"
#include "mongo/db/sorter/sorter.h"
#include "mongo/util/modules.h"

#include <memory>
#include <utility>

#include <boost/optional.hpp>

namespace [[MONGO_MOD_PUBLIC]] mongo {

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
                   MemoryUsageLimit maxMemoryUsageBytes);

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
     * Returns true if there are more aggregated result documents to return via getNext().
     * Must be called after readyGroups().
     */
    bool hasNext() const {
        if (_spilled) {
            return _sorterIterator != nullptr;
        }
        return _groupsIterator != _groups.end();
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
    std::shared_ptr<sorter::File> _file;
    std::vector<std::shared_ptr<Sorter<Value, Value>::Iterator>> _sortedFiles;
    bool _spilled{false};

    // Only used when '_spilled' is true.
    std::unique_ptr<Sorter<Value, Value>::Iterator> _sorterIterator;
    std::pair<Value, Value> _firstPartOfNextGroup;
    GroupProcessorBase::Accumulators _currentAccumulators;
};

}  // namespace mongo
