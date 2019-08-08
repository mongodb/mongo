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

#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kStorage

#include "mongo/platform/basic.h"

#include <cstring>
#include <memory>
#include <string>

#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/catalog/collection.h"
#include "mongo/db/catalog/index_catalog_entry.h"
#include "mongo/db/index/index_descriptor.h"
#include "mongo/db/storage/biggie/biggie_recovery_unit.h"
#include "mongo/db/storage/biggie/biggie_sorted_impl.h"
#include "mongo/db/storage/biggie/store.h"
#include "mongo/db/storage/index_entry_comparison.h"
#include "mongo/db/storage/key_string.h"
#include "mongo/util/bufreader.h"
#include "mongo/util/hex.h"
#include "mongo/util/shared_buffer.h"
#include "mongo/util/str.h"

namespace mongo {
namespace biggie {
namespace {

const Ordering allAscending = Ordering::make(BSONObj());

// This just checks to see if the field names are empty or not.
bool hasFieldNames(const BSONObj& obj) {
    BSONForEach(e, obj) {
        if (e.fieldName()[0])
            return true;
    }
    return false;
}

// This function converts a key and an ordering to a KeyString::Builder.
std::unique_ptr<KeyString::Builder> keyToKeyStringBuilder(const BSONObj& key, Ordering order) {
    KeyString::Version version = KeyString::Version::V1;
    std::unique_ptr<KeyString::Builder> retKs =
        std::make_unique<KeyString::Builder>(version, key, order);
    return retKs;
}

void prefixKeyString(KeyString::Builder* keyString,
                     const RecordId& loc,
                     std::string prefixToUse,
                     bool isUnique) {
    BSONObjBuilder b;
    b.append("", prefixToUse);                                                // prefix
    b.append("", std::string(keyString->getBuffer(), keyString->getSize()));  // key

    if (isUnique)
        keyString->resetToKey(b.obj(), allAscending);
    else
        keyString->resetToKey(b.obj(), allAscending, loc);
}

std::string createKeyString(const BSONObj& key,
                            const RecordId& loc,
                            std::string prefixToUse,
                            Ordering order,
                            bool isUnique) {
    KeyString::Version version = KeyString::Version::V1;
    KeyString::Builder ks(version, key, order);

    prefixKeyString(&ks, loc, prefixToUse, isUnique);
    return std::string(ks.getBuffer(), ks.getSize());
}

std::string createKeyString(const KeyString::Value& keyString,
                            const RecordId& loc,
                            std::string prefixToUse,
                            bool isUnique) {
    KeyString::Builder ks(KeyString::Version::V1);
    auto sizeWithoutRecordId =
        KeyString::sizeWithoutRecordIdAtEnd(keyString.getBuffer(), keyString.getSize());
    ks.resetFromBuffer(keyString.getBuffer(), sizeWithoutRecordId);

    prefixKeyString(&ks, loc, prefixToUse, isUnique);
    return std::string(ks.getBuffer(), ks.getSize());
}

bool keysAreIdentical(std::string ks1, std::string ks2, bool isUnique) {
    size_t size1 =
        isUnique ? ks1.length() : KeyString::sizeWithoutRecordIdAtEnd(ks1.c_str(), ks1.length());
    size_t size2 =
        isUnique ? ks2.length() : KeyString::sizeWithoutRecordIdAtEnd(ks2.c_str(), ks2.length());

    if (size1 != size2)
        return false;
    return !ks1.compare(0, size2, ks2);
}

/**
 * This function converts a KeyString::Builder into an IndexKeyEntry. We don't need to store the
 * typebits for the outer key string (the one consisting of the prefix, the key, and the recordId)
 * since those will not be used. However, we do need to store the typebits for the internal
 * keystring (made from the key itself), as those typebits are potentially important.
 *
 * The data which is serialized as a byte array, has the following structure:
 *     [RecordId][TypeBits of internal keystring]
 */
IndexKeyEntry keyStringToIndexKeyEntry(const std::string keyString,
                                       std::string data,
                                       const Ordering order) {
    int64_t ridRepr;
    std::memcpy(&ridRepr, data.data(), sizeof(int64_t));
    RecordId rid(ridRepr);

    std::string typeBitsString(data.length() - sizeof(int64_t), '\0');
    std::memcpy(&typeBitsString[0], data.data() + sizeof(int64_t), data.length() - sizeof(int64_t));

    KeyString::Version version = KeyString::Version::V1;
    KeyString::TypeBits tbInternal = KeyString::TypeBits(version);
    KeyString::TypeBits tbOuter = KeyString::TypeBits(version);

    BufReader brTbInternal(typeBitsString.c_str(), typeBitsString.length());
    tbInternal.resetFromBuffer(&brTbInternal);

    BSONObj bsonObj =
        KeyString::toBsonSafe(keyString.c_str(), keyString.length(), allAscending, tbOuter);

    SharedBuffer sb;
    auto it = BSONObjIterator(bsonObj);
    ++it;  // We want the second part
    KeyString::Builder ks(version);
    ks.resetFromBuffer((*it).valuestr(), (*it).valuestrsize());

    BSONObj originalKey = KeyString::toBsonSafe(ks.getBuffer(), ks.getSize(), order, tbInternal);

    sb = SharedBuffer::allocate(originalKey.objsize());
    std::memcpy(sb.get(), originalKey.objdata(), originalKey.objsize());

    BSONObj key(ConstSharedBuffer{sb});

    return IndexKeyEntry(key, rid);
}
}  // namespace

SortedDataBuilderInterface::SortedDataBuilderInterface(OperationContext* opCtx,
                                                       bool unique,
                                                       bool dupsAllowed,
                                                       Ordering order,
                                                       const std::string& prefix,
                                                       const std::string& identEnd,
                                                       const NamespaceString& collectionNamespace,
                                                       const std::string& indexName,
                                                       const BSONObj& keyPattern)
    : _opCtx(opCtx),
      _unique(unique),
      _dupsAllowed(dupsAllowed),
      _order(order),
      _prefix(prefix),
      _identEnd(identEnd),
      _collectionNamespace(collectionNamespace),
      _indexName(indexName),
      _keyPattern(keyPattern),
      _hasLast(false),
      _lastKeyToString(""),
      _lastRID(-1) {}

void SortedDataBuilderInterface::commit(bool mayInterrupt) {
    WriteUnitOfWork wunit(_opCtx);
    wunit.commit();
}

Status SortedDataBuilderInterface::addKey(const BSONObj& key, const RecordId& loc) {
    invariant(loc.isNormal() || loc.isReserved());
    invariant(!hasFieldNames(key));

    KeyString::HeapBuilder keyString(KeyString::Version::V1, key, _order, loc);

    return addKey(std::move(keyString.release()), loc);
}

Status SortedDataBuilderInterface::addKey(const KeyString::Value& keyString, const RecordId& loc) {
    dassert(loc == KeyString::decodeRecordIdAtEnd(keyString.getBuffer(), keyString.getSize()));

    StringStore* workingCopy(RecoveryUnit::get(_opCtx)->getHead());
    auto sizeWithoutRecordId =
        KeyString::sizeWithoutRecordIdAtEnd(keyString.getBuffer(), keyString.getSize());
    std::string newKSToString = std::string(keyString.getBuffer(), sizeWithoutRecordId);

    int twoKeyCmp = 1;
    int twoRIDCmp = 1;

    if (_hasLast) {
        twoKeyCmp = newKSToString.compare(_lastKeyToString);
        twoRIDCmp = loc.repr() - _lastRID;
    }

    if (twoKeyCmp < 0 || (_dupsAllowed && twoKeyCmp == 0 && twoRIDCmp < 0)) {
        return Status(ErrorCodes::InternalError,
                      "expected ascending (key, RecordId) order in bulk builder");
    }

    std::string workingCopyInsertKey =
        createKeyString(keyString, loc, _prefix, /* isUnique */ _unique);

    if (twoKeyCmp == 0 && twoRIDCmp != 0) {
        if (!_dupsAllowed) {
            auto key = KeyString::toBson(keyString, _order);
            return buildDupKeyErrorStatus(key, _collectionNamespace, _indexName, _keyPattern);
        }
        // Duplicate index entries are allowed on this unique index, so we put the RecordId in the
        // KeyString until the unique constraint is resolved.
        workingCopyInsertKey = createKeyString(keyString, loc, _prefix, /* isUnique */ false);
    }

    std::string internalTbString(keyString.getTypeBits().getBuffer(),
                                 keyString.getTypeBits().getSize());

    // Since this is an in-memory storage engine, we don't need to take endianness into account.
    int64_t recIdRepr = loc.repr();
    std::string data(sizeof(int64_t) + internalTbString.length(), '\0');
    std::memcpy(&data[0], &recIdRepr, sizeof(int64_t));
    std::memcpy(&data[0] + sizeof(int64_t), internalTbString.data(), internalTbString.length());

    workingCopy->insert(StringStore::value_type(workingCopyInsertKey, data));

    _hasLast = true;
    _lastKeyToString = newKSToString;
    _lastRID = loc.repr();

    RecoveryUnit::get(_opCtx)->makeDirty();
    return Status::OK();
}

SortedDataBuilderInterface* SortedDataInterface::getBulkBuilder(OperationContext* opCtx,
                                                                bool dupsAllowed) {
    return new SortedDataBuilderInterface(opCtx,
                                          _isUnique,
                                          dupsAllowed,
                                          _ordering,
                                          _prefix,
                                          _identEnd,
                                          _collectionNamespace,
                                          _indexName,
                                          _keyPattern);
}

// We append \1 to all idents we get, and therefore the KeyString with ident + \0 will only be
// before elements in this ident, and the KeyString with ident + \2 will only be after elements in
// this ident.
SortedDataInterface::SortedDataInterface(OperationContext* opCtx,
                                         StringData ident,
                                         const IndexDescriptor* desc)
    : ::mongo::SortedDataInterface(KeyString::Version::V1, Ordering::make(desc->keyPattern())),
      // All entries in this ident will have a prefix of ident + \1.
      _prefix(ident.toString().append(1, '\1')),
      // Therefore, the string ident + \2 will be greater than all elements in this ident.
      _identEnd(ident.toString().append(1, '\2')),
      _collectionNamespace(desc->getCollection()->ns()),
      _indexName(desc->indexName()),
      _keyPattern(desc->keyPattern()),
      _isUnique(desc->unique()),
      _isPartial(desc->isPartial()) {
    // This is the string representation of the KeyString before elements in this ident, which is
    // ident + \0. This is before all elements in this ident.
    _KSForIdentStart = createKeyString(
        BSONObj(), RecordId::min(), ident.toString().append(1, '\0'), _ordering, _isUnique);
    // Similarly, this is the string representation of the KeyString for something greater than
    // all other elements in this ident.
    _KSForIdentEnd = createKeyString(BSONObj(), RecordId::min(), _identEnd, _ordering, _isUnique);
}

SortedDataInterface::SortedDataInterface(const Ordering& ordering, bool isUnique, StringData ident)
    : ::mongo::SortedDataInterface(KeyString::Version::V1, ordering),
      _prefix(ident.toString().append(1, '\1')),
      _identEnd(ident.toString().append(1, '\2')),
      _isUnique(isUnique),
      _isPartial(false) {
    _KSForIdentStart = createKeyString(
        BSONObj(), RecordId::min(), ident.toString().append(1, '\0'), _ordering, _isUnique);
    _KSForIdentEnd = createKeyString(BSONObj(), RecordId::min(), _identEnd, _ordering, _isUnique);
}

Status SortedDataInterface::insert(OperationContext* opCtx,
                                   const BSONObj& key,
                                   const RecordId& loc,
                                   bool dupsAllowed) {
    // The KeyString representation of the key.
    KeyString::HeapBuilder keyString(_keyStringVersion, key, _ordering, loc);

    return insert(opCtx, std::move(keyString.release()), loc, dupsAllowed);
}

Status SortedDataInterface::insert(OperationContext* opCtx,
                                   const KeyString::Value& keyString,
                                   const RecordId& loc,
                                   bool dupsAllowed) {
    dassert(loc == KeyString::decodeRecordIdAtEnd(keyString.getBuffer(), keyString.getSize()));

    StringStore* workingCopy(RecoveryUnit::get(opCtx)->getHead());
    std::string insertKeyString = createKeyString(keyString, loc, _prefix, _isUnique);

    // For unique indexes, if duplicate keys are allowed then we do the following:
    //   - Create the KeyString without the RecordId in it and see if anything exists with that.
    //     - If the cursor didn't find anything, we index with this KeyString.
    //     - If the cursor found a value and it had differing RecordId's, then generate a KeyString
    //       with the RecordId in it.
    if (_isUnique) {
        // Ensure that another index entry without the RecordId in its KeyString doesn't exist with
        // another RecordId already.
        auto workingCopyIt = workingCopy->find(insertKeyString);
        if (workingCopyIt != workingCopy->end()) {
            IndexKeyEntry entry =
                keyStringToIndexKeyEntry(workingCopyIt->first, workingCopyIt->second, _ordering);

            if (entry.loc != loc) {
                if (dupsAllowed) {
                    // Duplicate index entries are allowed on this unique index, so we put the
                    // RecordId in the KeyString until the unique constraint is resolved.
                    insertKeyString =
                        createKeyString(keyString, loc, _prefix, /* isUnique */ false);
                } else {
                    // There was an attempt to create an index entry with a different RecordId while
                    // dups were not allowed.
                    auto key = KeyString::toBson(keyString, _ordering);
                    return buildDupKeyErrorStatus(
                        key, _collectionNamespace, _indexName, _keyPattern);
                }
            } else {
                return Status::OK();
            }
        }
    } else {
        invariant(dupsAllowed);
    }

    if (workingCopy->find(insertKeyString) != workingCopy->end())
        return Status::OK();

    // The value we insert is the RecordId followed by the typebits.
    std::string internalTbString =
        std::string(keyString.getTypeBits().getBuffer(), keyString.getTypeBits().getSize());

    // Since this is an in-memory storage engine, we don't need to take endianness into account.
    int64_t recIdRepr = loc.repr();
    std::string data(sizeof(int64_t) + internalTbString.length(), '\0');
    std::memcpy(&data[0], &recIdRepr, sizeof(int64_t));
    std::memcpy(&data[0] + sizeof(int64_t), internalTbString.data(), internalTbString.length());

    workingCopy->insert(StringStore::value_type(insertKeyString, data));
    RecoveryUnit::get(opCtx)->makeDirty();

    return Status::OK();
}

void SortedDataInterface::unindex(OperationContext* opCtx,
                                  const BSONObj& key,
                                  const RecordId& loc,
                                  bool dupsAllowed) {
    KeyString::HeapBuilder keyString(_keyStringVersion, key, _ordering, loc);

    unindex(opCtx, std::move(keyString.release()), loc, dupsAllowed);
}

void SortedDataInterface::unindex(OperationContext* opCtx,
                                  const KeyString::Value& keyString,
                                  const RecordId& loc,
                                  bool dupsAllowed) {
    dassert(loc == KeyString::decodeRecordIdAtEnd(keyString.getBuffer(), keyString.getSize()));

    StringStore* workingCopy(RecoveryUnit::get(opCtx)->getHead());
    std::string removeKeyString;
    bool erased;

    if (_isUnique) {
        // For unique indexes, to unindex them we do the following:
        //   - Create the KeyString with or without the RecordId in it depending on dupsAllowed
        //     and try to remove the index entry.
        //     - If the index entry was removed, we're done.
        //     - If the index entry was not removed, we generate a KeyString with or without the
        //       RecordId in it.
        // This is required because of the way we insert on unique indexes when dups are allowed.
        if (dupsAllowed)
            removeKeyString = createKeyString(keyString, loc, _prefix, /* isUnique */ false);
        else
            removeKeyString = createKeyString(keyString, loc, _prefix, /* isUnique */ true);

        // Check that the record id matches when using partial indexes. We may be called to unindex
        // records that are not present in the index due to the partial filter expression.
        if (!ifPartialCheckRecordIdEquals(opCtx, removeKeyString, loc))
            return;
        erased = workingCopy->erase(removeKeyString);

        if (!erased) {
            // If nothing above was erased, then we have to generate the KeyString with or without
            // the RecordId in it, and erase that. This could only happen on unique indexes where
            // duplicate index entries were/are allowed.
            if (dupsAllowed)
                removeKeyString = createKeyString(keyString, loc, _prefix, /* isUnique */ true);
            else
                removeKeyString = createKeyString(keyString, loc, _prefix, /* isUnique */ false);

            if (!ifPartialCheckRecordIdEquals(opCtx, removeKeyString, loc))
                return;
            erased = workingCopy->erase(removeKeyString);
        }
    } else {
        removeKeyString = createKeyString(keyString, loc, _prefix, /* isUnique */ false);
        erased = workingCopy->erase(removeKeyString);
    }

    if (erased)
        RecoveryUnit::get(opCtx)->makeDirty();
}

// This function is, as of now, not in the interface, but there exists a server ticket to add
// truncate to the list of commands able to be used.
Status SortedDataInterface::truncate(OperationContext* opCtx) {
    StringStore* workingCopy(RecoveryUnit::get(opCtx)->getHead());
    std::vector<std::string> toDelete;
    auto end = workingCopy->upper_bound(_KSForIdentEnd);
    for (auto it = workingCopy->lower_bound(_KSForIdentStart); it != end; ++it) {
        toDelete.push_back(it->first);
    }
    if (!toDelete.empty()) {
        for (const auto& key : toDelete)
            workingCopy->erase(key);
        RecoveryUnit::get(opCtx)->makeDirty();
    }

    return Status::OK();
}

Status SortedDataInterface::dupKeyCheck(OperationContext* opCtx, const BSONObj& key) {
    invariant(_isUnique);
    StringStore* workingCopy(RecoveryUnit::get(opCtx)->getHead());

    std::string minKey = createKeyString(key, RecordId::min(), _prefix, _ordering, _isUnique);
    std::string maxKey = createKeyString(key, RecordId::max(), _prefix, _ordering, _isUnique);

    // We effectively do the same check as in insert. However, we also check to make sure that
    // the iterator returned to us by lower_bound also happens to be inside out ident.
    auto lowerBoundIterator = workingCopy->lower_bound(minKey);
    if (lowerBoundIterator == workingCopy->end()) {
        return Status::OK();
    }
    if (lowerBoundIterator->first.compare(maxKey) > 0) {
        return Status::OK();
    }
    auto lower =
        keyStringToIndexKeyEntry(lowerBoundIterator->first, lowerBoundIterator->second, _ordering);

    ++lowerBoundIterator;
    if (lowerBoundIterator == workingCopy->end()) {
        return Status::OK();
    }

    auto next =
        keyStringToIndexKeyEntry(lowerBoundIterator->first, lowerBoundIterator->second, _ordering);
    if (key.woCompare(next.key, _ordering, false) == 0) {
        return buildDupKeyErrorStatus(key, _collectionNamespace, _indexName, _keyPattern);
    }

    return Status::OK();
}

void SortedDataInterface::fullValidate(OperationContext* opCtx,
                                       long long* numKeysOut,
                                       ValidateResults* fullResults) const {
    StringStore* workingCopy(RecoveryUnit::get(opCtx)->getHead());
    long long numKeys = 0;
    auto it = workingCopy->lower_bound(_KSForIdentStart);
    while (it != workingCopy->end() && it->first.compare(_KSForIdentEnd) < 0) {
        ++it;
        numKeys++;
    }
    *numKeysOut = numKeys;
}

bool SortedDataInterface::appendCustomStats(OperationContext* opCtx,
                                            BSONObjBuilder* output,
                                            double scale) const {
    return false;
}

long long SortedDataInterface::getSpaceUsedBytes(OperationContext* opCtx) const {
    StringStore* workingCopy(RecoveryUnit::get(opCtx)->getHead());
    size_t totalSize = 0;
    StringStore::const_iterator it = workingCopy->lower_bound(_KSForIdentStart);
    StringStore::const_iterator end = workingCopy->upper_bound(_KSForIdentEnd);
    int64_t numElements = workingCopy->distance(it, end);
    for (int i = 0; i < numElements; i++) {
        totalSize += it->first.length();
        ++it;
    }
    return (long long)totalSize;
}

bool SortedDataInterface::isEmpty(OperationContext* opCtx) {
    StringStore* workingCopy(RecoveryUnit::get(opCtx)->getHead());
    return workingCopy->distance(workingCopy->lower_bound(_KSForIdentStart),
                                 workingCopy->upper_bound(_KSForIdentEnd)) == 0;
}

std::unique_ptr<mongo::SortedDataInterface::Cursor> SortedDataInterface::newCursor(
    OperationContext* opCtx, bool isForward) const {
    StringStore* workingCopy(RecoveryUnit::get(opCtx)->getHead());

    return std::make_unique<SortedDataInterface::Cursor>(opCtx,
                                                         isForward,
                                                         _prefix,
                                                         _identEnd,
                                                         workingCopy,
                                                         _ordering,
                                                         _isUnique,
                                                         _KSForIdentStart,
                                                         _KSForIdentEnd);
}

Status SortedDataInterface::initAsEmpty(OperationContext* opCtx) {
    return Status::OK();
}

bool SortedDataInterface::ifPartialCheckRecordIdEquals(OperationContext* opCtx,
                                                       const std::string key,
                                                       const RecordId rid) const {
    if (!_isPartial)
        return true;

    StringStore* workingCopy(RecoveryUnit::get(opCtx)->getHead());
    auto workingCopyIt = workingCopy->find(key);
    if (workingCopyIt == workingCopy->end())
        return true;

    IndexKeyEntry entry =
        keyStringToIndexKeyEntry(workingCopyIt->first, workingCopyIt->second, _ordering);
    return entry.loc == rid;
}

// Cursor
SortedDataInterface::Cursor::Cursor(OperationContext* opCtx,
                                    bool isForward,
                                    std::string _prefix,
                                    std::string _identEnd,
                                    StringStore* workingCopy,
                                    Ordering order,
                                    bool isUnique,
                                    std::string _KSForIdentStart,
                                    std::string identEndBSON)
    : _opCtx(opCtx),
      _workingCopy(workingCopy),
      _endPos(boost::none),
      _endPosReverse(boost::none),
      _forward(isForward),
      _atEOF(false),
      _lastMoveWasRestore(false),
      _prefix(_prefix),
      _identEnd(_identEnd),
      _forwardIt(workingCopy->begin()),
      _reverseIt(workingCopy->rbegin()),
      _order(order),
      _endPosIncl(false),
      _isUnique(isUnique),
      _KSForIdentStart(_KSForIdentStart),
      _KSForIdentEnd(identEndBSON) {}

// This function checks whether or not the cursor end position was set by the user or not.
bool SortedDataInterface::Cursor::endPosSet() {
    return (_forward && _endPos != boost::none) || (!_forward && _endPosReverse != boost::none);
}

// This function checks whether or not a cursor is valid. In particular, it checks 1) whether the
// cursor is at end() or rend(), 2) whether the cursor is on the wrong side of the end position
// if it was set, and 3) whether the cursor is still in the ident.
bool SortedDataInterface::Cursor::checkCursorValid() {
    if (_forward) {
        if (_forwardIt == _workingCopy->end()) {
            return false;
        }
        if (endPosSet()) {
            // The endPos must be in the ident, at most one past the ident, or end. Therefore, the
            // endPos includes the check for being inside the ident
            if (_endPosIncl && _isUnique) {
                if (*_endPos == _workingCopy->end())
                    return true;

                // For unique indexes, we need to check if the cursor moved up a position when it
                // was restored. This isn't required for non-unique indexes because we store the
                // RecordId in the KeyString and use a "<" comparison instead of "<=" since we know
                // that no RecordId will ever reach RecordId::max() so we don't need to check the
                // equal side of things. This assumption doesn't hold for unique index KeyStrings.
                BSONObj strippedBSON = BSONObj::stripFieldNames(*_endPosKey);
                std::string endPosKeyString =
                    createKeyString(strippedBSON, RecordId::max(), _prefix, _order, _isUnique);

                if (_forwardIt->first.compare(endPosKeyString) <= 0)
                    return true;
                return false;
            }

            return *_endPos == _workingCopy->end() ||
                _forwardIt->first.compare((*_endPos)->first) < 0;
        }
        return _forwardIt->first.compare(_KSForIdentEnd) <= 0;
    } else {
        // This is a reverse cursor
        if (_reverseIt == _workingCopy->rend()) {
            return false;
        }
        if (endPosSet()) {
            if (_endPosIncl && _isUnique) {
                if (*_endPosReverse == _workingCopy->rend())
                    return true;

                BSONObj strippedBSON = BSONObj::stripFieldNames(*_endPosKey);
                std::string endPosKeyString =
                    createKeyString(strippedBSON, RecordId::min(), _prefix, _order, _isUnique);

                if (_reverseIt->first.compare(endPosKeyString) >= 0)
                    return true;
                return false;
            }

            return *_endPosReverse == _workingCopy->rend() ||
                _reverseIt->first.compare((*_endPosReverse)->first) > 0;
        }
        return _reverseIt->first.compare(_KSForIdentStart) >= 0;
    }
}

void SortedDataInterface::Cursor::setEndPosition(const BSONObj& key, bool inclusive) {
    auto finalKey = BSONObj::stripFieldNames(key);
    StringStore* workingCopy(RecoveryUnit::get(_opCtx)->getHead());
    if (finalKey.isEmpty()) {
        _endPos = boost::none;
        _endPosReverse = boost::none;
        return;
    }
    _endPosIncl = inclusive;
    _endPosKey = key;
    std::string _endPosBound;
    // If forward and inclusive or reverse and not inclusive, then we use the last element in this
    // ident. Otherwise, we use the first as our bound.
    if (_forward == inclusive) {
        _endPosBound = createKeyString(finalKey, RecordId::max(), _prefix, _order, _isUnique);
    } else {
        _endPosBound = createKeyString(finalKey, RecordId::min(), _prefix, _order, _isUnique);
    }
    if (_forward) {
        _endPos = workingCopy->lower_bound(_endPosBound);
    } else {
        // Reverse iterators work with upper bound since upper bound will return the first element
        // past the argument, so when it becomes a reverse iterator, it goes backwards one,
        // (according to the C++ standard) and we end up in the right place.
        _endPosReverse =
            StringStore::const_reverse_iterator(workingCopy->upper_bound(_endPosBound));
    }
}

boost::optional<IndexKeyEntry> SortedDataInterface::Cursor::next(RequestedInfo parts) {
    if (!_atEOF) {
        // If the last move was restore, then we don't need to advance the cursor, since the user
        // never got the value the cursor was pointing to in the first place. However,
        // _lastMoveWasRestore will go through extra logic on a unique index, since unique indexes
        // are not allowed to return the same key twice.
        if (_lastMoveWasRestore) {
            _lastMoveWasRestore = false;
        } else {
            // We basically just check to make sure the cursor is in the ident.
            if (_forward) {
                if (checkCursorValid()) {
                    ++_forwardIt;
                }
            } else {
                if (checkCursorValid()) {
                    ++_reverseIt;
                }
            }
            // We check here to make sure that we are on the correct side of the end position, and
            // that the cursor is still in the ident after advancing.
            if (!checkCursorValid()) {
                _atEOF = true;
                return boost::none;
            }
        }
    } else {
        _lastMoveWasRestore = false;
        return boost::none;
    }

    if (_forward) {
        return keyStringToIndexKeyEntry(_forwardIt->first, _forwardIt->second, _order);
    }
    return keyStringToIndexKeyEntry(_reverseIt->first, _reverseIt->second, _order);
}

boost::optional<IndexKeyEntry> SortedDataInterface::Cursor::seekAfterProcessing(BSONObj finalKey,
                                                                                bool inclusive) {
    std::string workingCopyBound;

    // Similar to above, if forward and inclusive or reverse and not inclusive, then use min() for
    // recordId. Else, we should use max().
    if (_forward == inclusive) {
        workingCopyBound = createKeyString(finalKey, RecordId::min(), _prefix, _order, _isUnique);
    } else {
        workingCopyBound = createKeyString(finalKey, RecordId::max(), _prefix, _order, _isUnique);
    }

    if (finalKey.isEmpty()) {
        // If the key is empty and it's not inclusive, then no elements satisfy this seek.
        if (!inclusive) {
            _atEOF = true;
            return boost::none;
        } else {
            // Otherwise, we just try to find the first element in this ident.
            if (_forward) {
                _forwardIt = _workingCopy->lower_bound(workingCopyBound);
            } else {

                // Reverse iterators work with upper bound since upper bound will return the first
                // element past the argument, so when it becomes a reverse iterator, it goes
                // backwards one, (according to the C++ standard) and we end up in the right place.
                _reverseIt = StringStore::const_reverse_iterator(
                    _workingCopy->upper_bound(workingCopyBound));
            }
            // Here, we check to make sure the iterator doesn't fall off the data structure and is
            // in the ident. We also check to make sure it is on the correct side of the end
            // position, if it was set.
            if (!checkCursorValid()) {
                _atEOF = true;
                return boost::none;
            }
        }
    } else {
        // Otherwise, we seek to the nearest element to our key, but only to the right.
        if (_forward) {
            if (inclusive)
                _forwardIt = _workingCopy->lower_bound(workingCopyBound);
            else
                _forwardIt = _workingCopy->upper_bound(workingCopyBound);
        } else {
            // Reverse iterators work with upper bound since upper bound will return the first
            // element past the argument, so when it becomes a reverse iterator, it goes
            // backwards one, (according to the C++ standard) and we end up in the right place.
            if (inclusive)
                _reverseIt = StringStore::const_reverse_iterator(
                    _workingCopy->upper_bound(workingCopyBound));
            else
                _reverseIt = StringStore::const_reverse_iterator(
                    _workingCopy->lower_bound(workingCopyBound));
        }
        // Once again, we check to make sure the iterator didn't fall off the data structure and
        // still is in the ident.
        if (!checkCursorValid()) {
            _atEOF = true;
            return boost::none;
        }
    }

    // Everything checks out, so we have successfullly seeked and now return.
    if (_forward) {
        return keyStringToIndexKeyEntry(_forwardIt->first, _forwardIt->second, _order);
    }
    return keyStringToIndexKeyEntry(_reverseIt->first, _reverseIt->second, _order);
}

boost::optional<IndexKeyEntry> SortedDataInterface::Cursor::seek(const BSONObj& key,
                                                                 bool inclusive,
                                                                 RequestedInfo parts) {
    BSONObj finalKey = BSONObj::stripFieldNames(key);
    _lastMoveWasRestore = false;
    _atEOF = false;

    return seekAfterProcessing(finalKey, inclusive);
}

boost::optional<IndexKeyEntry> SortedDataInterface::Cursor::seek(const IndexSeekPoint& seekPoint,
                                                                 RequestedInfo parts) {
    const BSONObj key = IndexEntryComparison::makeQueryObject(seekPoint, _forward);
    _atEOF = false;
    bool inclusive = true;
    BSONObj finalKey = key;
    _lastMoveWasRestore = false;

    return seekAfterProcessing(finalKey, inclusive);
}

void SortedDataInterface::Cursor::save() {
    _atEOF = false;
    if (_lastMoveWasRestore) {
        return;
    } else if (_forward && _forwardIt != _workingCopy->end()) {
        _saveKey = _forwardIt->first;
    } else if (!_forward && _reverseIt != _workingCopy->rend()) {  // reverse
        _saveKey = _reverseIt->first;
    } else {
        _saveKey = "";
    }
}

void SortedDataInterface::Cursor::restore() {
    StringStore* workingCopy(RecoveryUnit::get(_opCtx)->getHead());

    this->_workingCopy = workingCopy;

    // Here, we have to reset the end position if one was set earlier.
    if (endPosSet()) {
        setEndPosition(*_endPosKey, _endPosIncl);
    }

    // We reset the cursor, and make sure it's within the end position bounds. It doesn't matter if
    // the cursor is not in the ident right now, since that will be taken care of upon the call to
    // next().
    if (_forward) {
        if (_saveKey.length() == 0) {
            _forwardIt = workingCopy->end();
        } else {
            _forwardIt = workingCopy->lower_bound(_saveKey);
        }
        if (!checkCursorValid()) {
            _atEOF = true;
            _lastMoveWasRestore = true;
            return;
        }

        if (!_isUnique) {
            _lastMoveWasRestore = (_forwardIt->first.compare(_saveKey) != 0);
        } else {
            // Unique indexes cannot return the same key twice. Therefore, if we would normally not
            // advance on the next call to next() by setting _lastMoveWasRestore, we potentially
            // won't set it if that would cause us to return the same value twice.
            _lastMoveWasRestore = !keysAreIdentical(_forwardIt->first, _saveKey, _isUnique);
        }

    } else {
        // Now we are dealing with reverse cursors, and use similar logic.
        if (_saveKey.length() == 0) {
            _reverseIt = workingCopy->rend();
        } else {
            _reverseIt = StringStore::const_reverse_iterator(workingCopy->upper_bound(_saveKey));
        }
        if (!checkCursorValid()) {
            _atEOF = true;
            _lastMoveWasRestore = true;
            return;
        }

        if (!_isUnique) {
            _lastMoveWasRestore = (_reverseIt->first.compare(_saveKey) != 0);
        } else {
            // We use similar logic for reverse cursors on unique indexes.
            _lastMoveWasRestore = !keysAreIdentical(_reverseIt->first, _saveKey, _isUnique);
        }
    }
}

void SortedDataInterface::Cursor::detachFromOperationContext() {
    _opCtx = nullptr;
}

void SortedDataInterface::Cursor::reattachToOperationContext(OperationContext* opCtx) {
    this->_opCtx = opCtx;
}
}  // namespace biggie
}  // namespace mongo
