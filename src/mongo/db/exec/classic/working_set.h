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

#include "mongo/base/string_data.h"
#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/util/builder.h"
#include "mongo/db/exec/document_value/document.h"
#include "mongo/db/exec/document_value/document_metadata_fields.h"
#include "mongo/db/record_id.h"
#include "mongo/db/storage/snapshot.h"
#include "mongo/stdx/unordered_set.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/bufreader.h"

#include <cstddef>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include <boost/move/utility_core.hpp>
#include <boost/none.hpp>
#include <boost/optional.hpp>
#include <boost/optional/optional.hpp>

namespace mongo {

class WorkingSetMember;

typedef size_t WorkingSetID;

/**
 * A type used to identify indexes that have been registered with the WorkingSet. A WorkingSetMember
 * can be associated with a particular index via this id.
 */
using WorkingSetRegisteredIndexId = unsigned int;

/**
 * The key data extracted from an index.  Keeps track of both the key (currently a BSONObj) and
 * the index that provided the key.  The index key pattern is required to correctly interpret
 * the key.
 */
struct IndexKeyDatum {
    IndexKeyDatum(const BSONObj& keyPattern,
                  const BSONObj& key,
                  WorkingSetRegisteredIndexId indexId,
                  SnapshotId snapshotId)
        : indexKeyPattern(keyPattern), keyData(key), indexId(indexId), snapshotId(snapshotId) {}

    /**
     * getFieldDotted produces the field with the provided name based on index keyData. The return
     * object is populated if the element is in a provided index key.  Returns none otherwise.
     * Returning none indicates a query planning error.
     */
    static boost::optional<BSONElement> getFieldDotted(const std::vector<IndexKeyDatum>& keyData,
                                                       const std::string& field) {
        for (size_t i = 0; i < keyData.size(); ++i) {
            BSONObjIterator keyPatternIt(keyData[i].indexKeyPattern);
            BSONObjIterator keyDataIt(keyData[i].keyData);

            while (keyPatternIt.more()) {
                BSONElement keyPatternElt = keyPatternIt.next();
                MONGO_verify(keyDataIt.more());
                BSONElement keyDataElt = keyDataIt.next();

                if (field == keyPatternElt.fieldName())
                    return boost::make_optional(keyDataElt);
            }
        }
        return boost::none;
    }

    // This is not owned and points into the IndexDescriptor's data.
    BSONObj indexKeyPattern;

    // This is the BSONObj for the key that we put into the index.  Owned by us.
    BSONObj keyData;

    // Associates this index key with an index that has been registered with the WorkingSet. Can be
    // used to recover pointers to catalog objects for this index from the WorkingSet.
    WorkingSetRegisteredIndexId indexId;

    // Identifies the storage engine snapshot from which this index key was obtained.
    SnapshotId snapshotId;
};

/**
 * The type of the data passed between query stages.  In particular:
 *
 * Index scan stages return a WorkingSetMember in the RID_AND_IDX state.
 *
 * Collection scan stages return a WorkingSetMember in the RID_AND_OBJ state.
 *
 * A WorkingSetMember may have any of the data above.
 */
class WorkingSetMember {
public:
    enum MemberState {
        // Initial state.
        INVALID,

        // Data is from 1 or more indices.
        RID_AND_IDX,

        // Data is from a collection scan, or data is from an index scan and was fetched. The
        // BSONObj might be owned or unowned.
        RID_AND_OBJ,

        // The WSM doesn't correspond to an on-disk document anymore (e.g. is a computed
        // expression). Since it doesn't correspond to a stored document, a WSM in this state has an
        // owned BSONObj, but no record id.
        OWNED_OBJ,
    };

    static WorkingSetMember deserialize(BufReader& buf);

    /**
     * Reset to an "empty" state.
     */
    void clear();

    //
    // Member state and state transitions
    //

    MONGO_COMPILER_ALWAYS_INLINE MemberState getState() const {
        return _state;
    }

    void transitionToRecordIdAndObj();

    void transitionToOwnedObj();

    //
    // Core attributes
    //

    RecordId recordId;
    Snapshotted<Document> doc;
    std::vector<IndexKeyDatum> keyData;

    MONGO_COMPILER_ALWAYS_INLINE bool hasRecordId() const {
        return _state == RID_AND_IDX || _state == RID_AND_OBJ;
    }

    MONGO_COMPILER_ALWAYS_INLINE bool hasObj() const {
        return _state == OWNED_OBJ || _state == RID_AND_OBJ;
    }

    MONGO_COMPILER_ALWAYS_INLINE bool hasOwnedObj() const {
        return _state == OWNED_OBJ || _state == RID_AND_OBJ;
    }

    /**
     * Ensures that 'obj' of a WSM in the RID_AND_OBJ state is owned BSON. It is a no-op if the WSM
     * is in a different state or if 'obj' is already owned.
     *
     * It is illegal for unowned BSON to survive a yield, so this must be called on any working set
     * members which may stay alive across yield points.
     */
    void makeObjOwnedIfNeeded();

    /**
     * getFieldDotted uses its state (obj or index data) to produce the field with the provided
     * name.
     *
     * Returns true if there is the element is in an index key or in an (owned or unowned)
     * object.  *out is set to the element if so.
     *
     * Returns false otherwise.  Returning false indicates a query planning error.
     */
    bool getFieldDotted(const std::string& field, BSONElement* out) const;

    /**
     * Returns expected memory usage of working set member.
     */
    size_t getMemUsage() const;

    /**
     * Returns a const reference to an object housing the metadata fields associated with this
     * WorkingSetMember.
     */
    MONGO_COMPILER_ALWAYS_INLINE const DocumentMetadataFields& metadata() const {
        return _metadata;
    }

    /**
     * Returns a non-const reference to an object housing the metadata fields associated with this
     * WorkingSetMember.
     */
    DocumentMetadataFields& metadata() {
        return _metadata;
    }

    /**
     * Clears all metadata fields inside this WorkingSetMember, and returns a structure containing
     * that extracted metadata to the caller. The metadata can then be attached to a new
     * WorkingSetMember or to another data structure that houses metadata.
     */
    DocumentMetadataFields releaseMetadata() {
        return std::move(_metadata);
    }

    /**
     * Transfers metadata fields to this working set member. By pairs of calls to releaseMetadata()
     * and setMetadata(), callers can cheaply transfer metadata between WorkingSetMembers.
     */
    void setMetadata(DocumentMetadataFields&& metadata) {
        _metadata = std::move(metadata);
    }

    /**
     * Resets the underlying BSONObj in the doc field. This avoids unnecessary allocation/
     * deallocation of Document/DocumentStorage objects.
     */
    void resetDocument(SnapshotId snapshot, const BSONObj& obj);

    void serialize(BufBuilder& buf) const;

private:
    friend class WorkingSet;

    MemberState _state = WorkingSetMember::INVALID;

    DocumentMetadataFields _metadata;
};

/**
 * A variant of WorkingSetMember that is designed to be compatible with the SortExecutor. Objects of
 * this type are small, acting only as a handle to the underlying WorkingSetMember. This means that
 * they can be efficiently copied or moved during the sorting process while the actual
 * WorkingSetMember data remains in place.
 *
 * A SortableWorkingSetMember supports serialization and deserialization so that objects of this
 * type can be flushed to disk and later recovered.
 */
class SortableWorkingSetMember {
public:
    struct SorterDeserializeSettings {};

    static SortableWorkingSetMember deserializeForSorter(BufReader& buf,
                                                         const SorterDeserializeSettings&) {
        return WorkingSetMember::deserialize(buf);
    }

    /**
     * Constructs an empty SortableWorkingSetMember.
     */
    SortableWorkingSetMember() = default;

    /**
     * Constructs a SortableWorkingSetMember from a regular WorkingSetMember. Supports implicit
     * conversion from WorkingSetMember.
     */
    /* implicit */ SortableWorkingSetMember(WorkingSetMember&& wsm)
        : _holder(std::make_shared<WorkingSetMember>(std::move(wsm))) {}

    void serializeForSorter(BufBuilder& buf) const {
        _holder->serialize(buf);
    }

    int memUsageForSorter() const {
        return _holder->getMemUsage();
    }

    /**
     * Extracts and returns the underlying WorkingSetMember held by this SortableWorkingSetMember.
     */
    WorkingSetMember extract() {
        return std::move(*_holder);
    }

    /**
     * Returns a reference to the underlying WorkingSetMember.
     */
    WorkingSetMember* operator->() const {
        return _holder.get();
    }

    WorkingSetMember& operator*() const {
        return *_holder;
    }

    SortableWorkingSetMember getOwned() const;
    void makeOwned();

private:
    std::shared_ptr<WorkingSetMember> _holder;
};

/**
 * All data in use by a query.  Data is passed through the stage tree by referencing the ID of
 * an element of the working set.  Stages can add elements to the working set, delete elements
 * from the working set, or mutate elements in the working set.
 */
class WorkingSet {
    WorkingSet(const WorkingSet&) = delete;
    WorkingSet& operator=(const WorkingSet&) = delete;

public:
    static const WorkingSetID INVALID_ID = WorkingSetID(-1);

    WorkingSet();

    ~WorkingSet() = default;

    /**
     * Allocate a new query result and return the ID used to get and free it.
     */
    WorkingSetID allocate();

    /**
     * Get the i-th mutable query result. The pointer will be valid for this id until freed.
     * Do not delete the returned pointer as the WorkingSet retains ownership. Call free() to
     * release it.
     */
    MONGO_COMPILER_ALWAYS_INLINE WorkingSetMember* get(WorkingSetID i) {
        dassert(i < _data.size());              // ID has been allocated.
        dassert(_data[i].nextFreeOrSelf == i);  // ID currently in use.
        return &_data[i].member;
    }

    MONGO_COMPILER_ALWAYS_INLINE const WorkingSetMember* get(WorkingSetID i) const {
        dassert(i < _data.size());              // ID has been allocated.
        dassert(_data[i].nextFreeOrSelf == i);  // ID currently in use.
        return &_data[i].member;
    }

    /**
     * Returns true if WorkingSetMember with id 'i' is free.
     */
    bool isFree(WorkingSetID i) const {
        return _data[i].nextFreeOrSelf != i;
    }

    /**
     * Deallocate the i-th query result and release its resources.
     */
    inline void free(WorkingSetID i) {
        MemberHolder& holder = _data[i];
        MONGO_verify(i < _data.size());            // ID has been allocated.
        MONGO_verify(holder.nextFreeOrSelf == i);  // ID currently in use.

        // Free resources and push this WSM to the head of the freelist.
        holder.member.clear();
        holder.nextFreeOrSelf = _freeList;
        _freeList = i;
    }

    /**
     * Removes and deallocates all members of this working set.
     */
    void clear();

    //
    // WorkingSetMember state transitions
    //

    void transitionToRecordIdAndIdx(WorkingSetID id);
    void transitionToRecordIdAndObj(WorkingSetID id);
    void transitionToOwnedObj(WorkingSetID id);

    /**
     * Registers the index ident with the WorkingSet, and returns a handle that can be used to
     * recover the index ident.
     */
    WorkingSetRegisteredIndexId registerIndexIdent(const std::string& ident);

    /**
     * Returns the index ident for an index that has previously been registered with the WorkingSet
     * using 'registerIndexIdent()'.
     */
    StringData retrieveIndexIdent(WorkingSetRegisteredIndexId indexId) const {
        return _registeredIndexes[indexId];
    }

    /**
     * Returns the WorkingSetMember with the given id after removing it from this WorkingSet. The
     * WSM can be reinstated in the WorkingSet by calling 'emplace()'.
     *
     * WorkingSetMembers typically only temporarily live free of their WorkingSet, so calls to
     * 'extract()' and 'emplace()' should come in pairs.
     */
    WorkingSetMember extract(WorkingSetID);

    /**
     * Puts the given WorkingSetMember into this WorkingSet. Assigns the WorkingSetMember an id and
     * returns it. This id can be used later to obtain a pointer to the WSM using 'get()'.
     *
     * WorkingSetMembers typically only temporarily live free of their WorkingSet, so calls to
     * 'extract()' and 'emplace()' should come in pairs.
     */
    WorkingSetID emplace(WorkingSetMember&&);

private:
    struct MemberHolder {
        // Free list link if freed. Points to self if in use.
        WorkingSetID nextFreeOrSelf;

        WorkingSetMember member;
    };

    // All WorkingSetIDs are indexes into this, except for INVALID_ID.
    // Elements are added to _freeList rather than removed when freed.
    std::vector<MemberHolder> _data;

    // Index into _data, forming a linked-list using MemberHolder::nextFreeOrSelf as the next
    // link. INVALID_ID is the list terminator since 0 is a valid index.
    // If _freeList == INVALID_ID, the free list is empty and all elements in _data are in use.
    WorkingSetID _freeList;

    // Holds index idents that have been registered with 'registerIndexIdent()`. The
    // WorkingSetRegisteredIndexId is the offset into the vector.
    std::vector<std::string> _registeredIndexes;
};

}  // namespace mongo
