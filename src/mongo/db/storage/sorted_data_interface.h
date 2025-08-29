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

#include "mongo/db/operation_context.h"
#include "mongo/db/record_id.h"
#include "mongo/db/storage/container.h"
#include "mongo/db/storage/ident.h"
#include "mongo/db/storage/index_entry_comparison.h"
#include "mongo/db/storage/key_format.h"
#include "mongo/db/storage/key_string/key_string.h"

#include <memory>
#include <span>

#include <boost/optional/optional.hpp>

#pragma once

namespace mongo {

class BSONObjBuilder;
class BucketDeletionNotification;
class SortedDataBuilderInterface;
class IndexValidateResults;
class SortedDataKeyValueView;

namespace CollectionValidation {
class ValidationOptions;
}

struct IndexConfig {
    enum class IndexVersion { kV1 = 1, kV2 = 2 };
    static constexpr IndexVersion kLatestIndexVersion = IndexVersion::kV2;

    bool isIdIndex;
    bool unique;
    IndexVersion version;
    const BSONObj& infoObj;
    const std::string& indexName;
    const Ordering& ordering;

    IndexConfig(bool isIdIndex,
                bool unique,
                IndexVersion version,
                const BSONObj& infoObj,
                const std::string& indexName,
                const Ordering& ordering)
        : isIdIndex(isIdIndex),
          unique(unique),
          version(version),
          infoObj(infoObj),
          indexName(indexName),
          ordering(ordering) {}

    // Discourage caching by deleting copy/move constructors and assignment operators.
    IndexConfig(const IndexConfig&) = delete;
    IndexConfig& operator=(const IndexConfig&) = delete;
    IndexConfig(IndexConfig&&) = delete;
    IndexConfig& operator=(IndexConfig&&) = delete;

    std::string toString() const {
        return infoObj.toString();
    }
};

/**
 * This is the uniform interface for storing indexes and supporting point queries as well as range
 * queries. The actual implementation is up to the storage engine. All the storage engines must
 * support an index key size up to the maximum document size.
 */
class SortedDataInterface {
public:
    struct DuplicateKey {
        BSONObj key;
        boost::optional<RecordId> id;
        DuplicateKeyErrorInfo::FoundValue foundValue;
    };

    /**
     * Constructs a SortedDataInterface. The rsKeyFormat is the RecordId key format of the related
     * RecordStore.
     */
    SortedDataInterface(key_string::Version keyStringVersion,
                        Ordering ordering,
                        KeyFormat rsKeyFormat)
        : _keyStringVersion(keyStringVersion), _ordering(ordering), _rsKeyFormat(rsKeyFormat) {}

    virtual ~SortedDataInterface() {}

    //
    // Data changes
    //

    /**
     * Return a bulk builder for 'this' index.
     *
     * Implementations can assume that 'this' index outlives its bulk
     * builder.
     *
     * @param opCtx the transaction under which keys are added to 'this' index
     * @param ru the RecoveryUnit for the current operation.
     */
    virtual std::unique_ptr<SortedDataBuilderInterface> makeBulkBuilder(OperationContext* opCtx,
                                                                        RecoveryUnit& ru) = 0;

    /**
     * Inserts the given key into the index, which must have a RecordId appended to the end. Returns
     * DuplicateKey if `dupsAllowed` is false and the key already exists in the index with a
     * different RecordId. Returns OK if the key exists with the same RecordId.
     *
     * If `includeDuplicateRecordId` is kOn and DuplicateKey is returned, embeds the record id of
     * the duplicate in the returned status.
     */
    virtual std::variant<Status, DuplicateKey> insert(
        OperationContext* opCtx,
        RecoveryUnit& ru,
        const key_string::View& keyString,
        bool dupsAllowed,
        IncludeDuplicateRecordId includeDuplicateRecordId = IncludeDuplicateRecordId::kOff) = 0;

    /**
     * Remove the entry from the index with the specified KeyString, which must have a RecordId
     * appended to the end.
     *
     * @param opCtx the transaction under which the remove takes place
     * @param dupsAllowed true to enforce strict checks to ensure we only delete a key with an exact
     *        match, false otherwise
     */
    virtual void unindex(OperationContext* opCtx,
                         RecoveryUnit& ru,
                         const key_string::View& keyString,
                         bool dupsAllowed) = 0;

    /**
     * Retuns the RecordId of the first key whose prefix matches this KeyString.
     *
     * This will not accept a KeyString with a Discriminator other than kInclusive.
     */
    virtual boost::optional<RecordId> findLoc(OperationContext* opCtx,
                                              RecoveryUnit& ru,
                                              std::span<const char> keyString) const = 0;

    /**
     * Return duplicate key information if there is more than one occurrence of 'KeyString' in this
     * index, or boost::none otherwise. This call is only allowed on a unique index, and will fail
     * an invariant otherwise.
     *
     * @param opCtx the transaction under which this operation takes place
     */
    virtual boost::optional<DuplicateKey> dupKeyCheck(OperationContext* opCtx,
                                                      RecoveryUnit& ru,
                                                      const key_string::View& keyString) = 0;

    /**
     * Attempt to reduce the storage space used by this index via compaction. Only called if the
     * indexed record store supports compaction-in-place.
     * Returns an estimated number of bytes when doing a dry run.
     */
    virtual StatusWith<int64_t> compact(OperationContext* opCtx,
                                        RecoveryUnit& ru,
                                        const CompactOptions& options) {
        return Status::OK();
    }

    /**
     * Removes all keys from the index.
     */
    virtual Status truncate(OperationContext* opCtx, RecoveryUnit& ru) = 0;

    //
    // Information about the tree
    //

    /**
     * Validates the sorted data. If 'full' is false, only performs checks which do not traverse the
     * data. If 'full' is true, additionally traverses the data and validates its internal
     * structure.
     */
    virtual IndexValidateResults validate(
        OperationContext* opCtx,
        RecoveryUnit& ru,
        const CollectionValidation::ValidationOptions& options) const = 0;

    virtual bool appendCustomStats(OperationContext* opCtx,
                                   RecoveryUnit& ru,
                                   BSONObjBuilder* output,
                                   double scale) const = 0;


    /**
     * Return the number of bytes consumed by 'this' index.
     *
     * @param opCtx the transaction under which this operation takes place
     *
     * @see IndexAccessMethod::getSpaceUsedBytes
     */
    virtual long long getSpaceUsedBytes(OperationContext* opCtx, RecoveryUnit& ru) const = 0;

    /**
     * The number of unused free bytes consumed by this index on disk.
     */
    virtual long long getFreeStorageBytes(OperationContext* opCtx, RecoveryUnit& ru) const = 0;

    /**
     * Return true if 'this' index is empty, and false otherwise.
     */
    virtual bool isEmpty(OperationContext* opCtx, RecoveryUnit& ru) = 0;

    /**
     * Prints any storage engine provided metadata for the index entry with key 'keyString'.
     */
    virtual void printIndexEntryMetadata(OperationContext* opCtx,
                                         RecoveryUnit& ru,
                                         const key_string::View& keyString) const = 0;

    /**
     * Return the number of entries in 'this' index.
     */
    virtual int64_t numEntries(OperationContext* opCtx, RecoveryUnit& ru) const = 0;

    /**
     * Returns the underlying container.
     */
    virtual StringKeyedContainer& getContainer() = 0;

    /**
     * Returns the underlying container.
     */
    virtual const StringKeyedContainer& getContainer() const = 0;

    /*
     * Return the KeyString version for 'this' index.
     */
    key_string::Version getKeyStringVersion() const {
        return _keyStringVersion;
    }

    /*
     * Return the ordering for 'this' index.
     */
    Ordering getOrdering() const {
        return _ordering;
    }

    /**
     * Returns the format of the associated RecordStore's RecordId keys.
     */
    KeyFormat rsKeyFormat() const {
        return _rsKeyFormat;
    }

    std::shared_ptr<Ident> getSharedIdent() const {
        return getContainer().ident();
    }

    void setIdent(std::shared_ptr<Ident> newIdent) {
        getContainer().setIdent(std::move(newIdent));
    }

    /**
     * Navigates over the sorted data.
     *
     * A cursor is constructed with a direction flag with the following effects:
     *      - The direction that next() moves.
     *      - If a seek method hits an exact match on key, forward cursors will be positioned on
     *        the first value for that key, reverse cursors on the last.
     *      - If a seek method or restore does not hit an exact match, cursors will be
     *        positioned on the closest position *after* the query in the direction of the
     *        search.
     *      - The end position is on the "far" side of the query. In a forward cursor that means
     *        that it is the lowest value for the key if the end is exclusive or the first entry
     *        past the key if the end is inclusive or there are no exact matches.
     *
     * A cursor is tied to a transaction, such as the OperationContext or a WriteUnitOfWork
     * inside that context. Any cursor acquired inside a transaction is invalid outside
     * of that transaction, instead use the save and restore methods to reestablish the cursor.
     *
     * Any method other than the save methods may throw WriteConflict exception. If that
     * happens, the cursor may not be used again until it has been saved and successfully
     * restored. If next() or restore() throw a WCE the cursor's position will be the same as
     * before the call (strong exception guarantee). All other methods leave the cursor in a
     * valid state but with an unspecified position (basic exception guarantee). All methods
     * only provide the basic guarantee for exceptions other than WCE.
     *
     * Any returned unowned BSON is only valid until the next call to any method on this
     * interface. The implementations must assume that passed-in unowned BSON is only valid for
     * the duration of the call.
     *
     * Implementations may override any default implementation if they can provide a more
     * efficient implementation.
     */
    class Cursor {
    public:
        /**
         * Tells methods that return an IndexKeyEntry whether the caller is interested
         * in including the key field.
         */
        enum class KeyInclusion {
            kExclude,
            kInclude,
        };

        virtual ~Cursor() = default;

        /**
         * Sets the position to stop scanning. An empty key unsets the end position.
         *
         * If next() hits this position, or a seek method attempts to seek past it they
         * unposition the cursor and return boost::none.
         *
         * Setting the end position should be done before seeking since the current position, if
         * any, isn't checked.
         */
        virtual void setEndPosition(const BSONObj& key, bool inclusive) = 0;
        virtual void setEndPosition(const key_string::Value& keyString) = 0;

        /**
         * Moves forward and returns the new data or boost::none if there is no more data.
         * If not positioned, returns the first entry or boost::none.
         *
         * Note that nextKeyValueView() returns unowned data, which is invalidated upon
         * calling a next() or seek() variant, a save(), or when the cursor is destructed.
         */
        virtual boost::optional<IndexKeyEntry> next(
            RecoveryUnit& ru, KeyInclusion keyInclusion = KeyInclusion::kInclude) = 0;
        virtual boost::optional<KeyStringEntry> nextKeyString(RecoveryUnit& ru) = 0;
        virtual SortedDataKeyValueView nextKeyValueView(RecoveryUnit& ru) = 0;

        //
        // Seeking
        //

        /**
         * Seeks to the provided keyString and returns the KeyStringEntry.
         * The provided keyString has discriminator information encoded.
         * The keyString should not have RecordId or TypeBits encoded, which is guaranteed if
         * obtained from BuilderBase::finishAndGetBuffer().
         */
        virtual boost::optional<KeyStringEntry> seekForKeyString(
            RecoveryUnit& ru, std::span<const char> keyString) = 0;

        /**
         * Seeks to the provided keyString and returns the SortedDataKeyValueView.
         * The provided keyString has discriminator information encoded.
         * The keyString should not have RecordId or TypeBits encoded, which is guaranteed if
         * obtained from BuilderBase::finishAndGetBuffer().
         *
         * Returns unowned data, which is invalidated upon calling a next() or seek()
         * variant, a save(), or when the cursor is destructed.
         */
        virtual SortedDataKeyValueView seekForKeyValueView(RecoveryUnit& ru,
                                                           std::span<const char> keyString) = 0;

        /**
         * Seeks to the provided keyString and returns the IndexKeyEntry.
         * The provided keyString has discriminator information encoded.
         * The keyString should not have RecordId or TypeBits encoded, which is guaranteed if
         * obtained from BuilderBase::finishAndGetBuffer().
         */
        virtual boost::optional<IndexKeyEntry> seek(
            RecoveryUnit& ru,
            std::span<const char> keyString,
            KeyInclusion keyInclusion = KeyInclusion::kInclude) = 0;

        /**
         * Seeks to the provided keyString and returns the RecordId of the matching key, or
         * boost::none if one does not exist.
         * The provided keyString must always have a kInclusive discriminator.
         * The keyString should not have RecordId or TypeBits encoded, which is guaranteed if
         * obtained from BuilderBase::finishAndGetBuffer().
         */
        virtual boost::optional<RecordId> seekExact(RecoveryUnit& ru,
                                                    std::span<const char> keyString) = 0;

        //
        // Saving and restoring state
        //

        /**
         * Prepares for state changes in underlying data in a way that allows the cursor's
         * current position to be restored.
         *
         * It is safe to call save multiple times in a row.
         * No other method (excluding destructor) may be called until successfully restored.
         */
        virtual void save() = 0;

        /**
         * Prepares for state changes in underlying data without necessarily saving the current
         * state.
         *
         * The cursor's position when restored is unspecified. Caller is expected to seek
         * following the restore.
         *
         * It is safe to call saveUnpositioned multiple times in a row.
         * No other method (excluding destructor) may be called until successfully restored.
         */
        virtual void saveUnpositioned() {
            save();
        }

        /**
         * Recovers from potential state changes in underlying data.
         *
         * If the former position no longer exists, a following call to next() will return the
         * next closest position in the direction of the scan, if any.
         *
         * This handles restoring after either save() or saveUnpositioned().
         */
        virtual void restore(RecoveryUnit& ru) = 0;

        /**
         * Detaches from the OperationContext. Releases storage-engine resources, unless
         * setSaveStorageCursorOnDetachFromOperationContext() has been set to true.
         */
        virtual void detachFromOperationContext() = 0;

        /**
         * Reattaches to the OperationContext and reacquires any storage-engine state if necessary.
         *
         * It is only legal to call this in the "detached" state. On return, the cursor may still
         * be a "saved" state if there was a prior call to save(). In this case, callers must still
         * call restore() to use this object.
         */
        virtual void reattachToOperationContext(OperationContext* opCtx) = 0;

        /**
         * Toggles behavior on whether to give up the underlying storage cursor (and any record
         * pointed to by it) on detachFromOperationContext(). This supports the query layer
         * retaining valid and positioned cursors across commands.
         */
        virtual void setSaveStorageCursorOnDetachFromOperationContext(bool) = 0;

        /**
         * Returns true if the record id can be extracted from the unique index key string.
         *
         * Unique indexes created prior to 4.2 may contain key strings that do not have
         * an embedded record id. We will have to look up the record for this key in the index
         * to obtain the record id.
         *
         * This is used primarily during validation to identify unique indexes with keys in the
         * old format due to rolling upgrades.
         */
        virtual bool isRecordIdAtEndOfKeyString() const = 0;
    };

    /**
     * Returns an unpositioned cursor over 'this' index.
     *
     * Implementations can assume that 'this' index outlives all cursors it produces.
     */
    virtual std::unique_ptr<Cursor> newCursor(OperationContext* opCtx,
                                              RecoveryUnit& ru,
                                              bool isForward = true) const = 0;

    //
    // Index creation
    //

    virtual Status initAsEmpty() = 0;

protected:
    const key_string::Version _keyStringVersion;
    const Ordering _ordering;
    const KeyFormat _rsKeyFormat;
};

/**
 * A version-hiding wrapper around the bulk builder for the Btree.
 */
class SortedDataBuilderInterface {
public:
    virtual ~SortedDataBuilderInterface() {}

    /**
     * Adds 'keyString' to intermediate storage.
     *
     * 'keyString' must be > or >= the last key passed to this function, depending on whether the
     * thing being built supports duplicates. The behavior if this in violated is unspecified and
     * must not be relied on by the caller.
     *
     * Some storage engines require callers to manage a WriteUnitOfWork to perform these inserts
     * transactionally. Other storage engines do not perform inserts transactionally and will ignore
     * any parent WriteUnitOfWork.
     */
    virtual void addKey(RecoveryUnit& ru, const key_string::View& keyString) = 0;
};

/**
 * A SortedDataKeyValueView is an unowned view into a KeyString-formatted binary blob.
 *
 * A SortedDataKeyValueView is composed of three parts:
 * - KeyString data without RecordId
 * - Encoded RecordId
 * - TypeBits (optional)
 *
 * This differs from key_string::View in that the three components may not be contiguous in memory,
 * as keystrings are split into a key and value when stored in an index.
 */
class SortedDataKeyValueView {
public:
    /**
     * Construct a SortedDataKeyValueView with pointers and sizes to each underlying component.
     */
    SortedDataKeyValueView(std::span<const char> key,
                           std::span<const char> rid,
                           std::span<const char> typeBits,
                           key_string::Version version,
                           bool isRecordIdAtEndOfKeyString,
                           const RecordId* id = nullptr)
        : _ksData(key.data()),
          _ridData(rid.data()),
          _tbData(typeBits.data()),
          _ksSize(static_cast<int32_t>(key.size())),
          _ridSize(static_cast<int32_t>(rid.size())),
          _tbSize(static_cast<int32_t>(typeBits.size())),
          _version(version),
          _id(id) {
        invariant(key.size() > 0 && key.size() < std::numeric_limits<int32_t>::max());
        invariant(rid.size() < std::numeric_limits<int32_t>::max());
        invariant(typeBits.size() < std::numeric_limits<int32_t>::max());
        _ksOriginalSize = isRecordIdAtEndOfKeyString ? (key.size() + rid.size()) : key.size();
    }

    SortedDataKeyValueView() = default;
    SortedDataKeyValueView(const SortedDataKeyValueView& other) = default;
    SortedDataKeyValueView(SortedDataKeyValueView&& other) = default;
    SortedDataKeyValueView& operator=(const SortedDataKeyValueView& other) = default;
    SortedDataKeyValueView& operator=(SortedDataKeyValueView&& other) = default;

    /**
     * Return the original index key buffer as is, which may or may not end with a RecordId,
     * depending on isRecordIdAtEndOfKeyString().
     */
    std::span<const char> getKeyStringOriginalView() const {
        return {_ksData, static_cast<std::span<const char>::size_type>(_ksOriginalSize)};
    }

    std::span<const char> getKeyStringWithoutRecordIdView() const {
        return {_ksData, static_cast<std::span<const char>::size_type>(_ksSize)};
    }

    /**
     * Return the raw TypeBits buffer including the size prefix.
     */
    std::span<const char> getTypeBitsView() const {
        return {_tbData, static_cast<std::span<const char>::size_type>(_tbSize)};
    }

    std::span<const char> getRecordIdView() const {
        return {_ridData, static_cast<std::span<const char>::size_type>(_ridSize)};
    }

    key_string::Version getVersion() const {
        return _version;
    }

    bool isRecordIdAtEndOfKeyString() const {
        return _ksOriginalSize > _ksSize;
    }

    /**
     * Return the cached RecordId pointer that was passed to this view's constructor.
     *
     * A nullptr only means the pointer was not cached, but the RecordId may still be present,
     * as long as the original key_string::Value has a RecordId at the end. In this case, the
     * RecordId buffer can be obtained through getRecordIdView(), and then be decoded as long
     * or binary string depending on its key format.
     */
    const RecordId* getRecordId() const {
        return _id;
    }

    /**
     * Create a Value copy from this view including all components.
     */
    key_string::Value getValueCopy() const {
        return key_string::Value::makeValue(
            _version, getKeyStringWithoutRecordIdView(), getRecordIdView(), getTypeBitsView());
    }

    /**
     * Returns an unowned view of the provided Value. Remains valid as long as this Value is
     * valid.
     */
    static SortedDataKeyValueView fromValue(const key_string::Value& value) {
        return {value.getViewWithoutRecordId(),
                value.getRecordIdView(),
                value.getTypeBitsView(),
                value.getVersion(),
                value.getRecordIdSize() > 0 /* isRecordIdAtEndOfKeyString */};
    }

    bool isEmpty() const {
        return _ksSize == 0;
    }

    void reset() {
        _ksSize = 0;
    }

    std::string toString() const {
        if (isEmpty())
            return "";
        std::stringstream ss;
        ss << hexblob::encode(_ksData, _ksSize) << "_" << hexblob::encode(_ridData, _ridSize) << "_"
           << (_tbSize > 0 ? hexblob::encode(_tbData, _tbSize) : "");
        return ss.str();
    }

private:
    const char* _ksData = nullptr;
    const char* _ridData = nullptr;
    const char* _tbData = nullptr;

    int32_t _ksSize = 0;
    int32_t _ksOriginalSize = 0;
    int32_t _ridSize = 0;
    int32_t _tbSize = 0;
    key_string::Version _version = key_string::Version::kLatestVersion;
    const RecordId* _id = nullptr;
};

}  // namespace mongo
