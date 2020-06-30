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

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kStorage

#include "mongo/platform/basic.h"

#include <boost/iterator/iterator_facade.hpp>
#include <cstring>
#include <memory>
#include <string>

#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/catalog/collection.h"
#include "mongo/db/catalog/index_catalog_entry.h"
#include "mongo/db/index/index_descriptor.h"
#include "mongo/db/storage/ephemeral_for_test/ephemeral_for_test_radix_store.h"
#include "mongo/db/storage/ephemeral_for_test/ephemeral_for_test_recovery_unit.h"
#include "mongo/db/storage/ephemeral_for_test/ephemeral_for_test_sorted_impl.h"
#include "mongo/db/storage/index_entry_comparison.h"
#include "mongo/db/storage/key_string.h"
#include "mongo/util/bufreader.h"
#include "mongo/util/hex.h"
#include "mongo/util/shared_buffer.h"
#include "mongo/util/str.h"

namespace mongo {
namespace ephemeral_for_test {
namespace {

// Helper to interpret index data buffer
class IndexDataEntry {
public:
    IndexDataEntry() : _buffer(nullptr) {}
    IndexDataEntry(const uint8_t* buffer);
    IndexDataEntry(const std::string& indexDataEntry);

    static std::string create(RecordId loc, const KeyString::TypeBits& typeBits);

    const uint8_t* buffer() const;
    size_t size() const;  // returns buffer size
    RecordId loc() const;
    KeyString::TypeBits typeBits() const;

private:
    const uint8_t* _buffer;
};

// Forward iterator for IndexDataEntry with contigous memory layout
class IndexDataEntryIterator : public boost::iterator_facade<IndexDataEntryIterator,
                                                             IndexDataEntry const,
                                                             boost::forward_traversal_tag> {
public:
    IndexDataEntryIterator() = default;
    IndexDataEntryIterator(const uint8_t* entryData);

private:
    friend class boost::iterator_core_access;

    void increment();
    bool equal(IndexDataEntryIterator const& other) const;
    const IndexDataEntry& dereference() const;

    IndexDataEntry _entry;
};

// Helper to interpret index data buffer for unique index
class UniqueIndexData {
public:
    UniqueIndexData() : _size(0), _begin(nullptr), _end(nullptr) {}
    UniqueIndexData(const std::string& indexData) {
        std::memcpy(&_size, indexData.data(), sizeof(uint64_t));
        _begin = reinterpret_cast<const uint8_t*>(indexData.data() + sizeof(uint64_t));
        _end = reinterpret_cast<const uint8_t*>(indexData.data() + indexData.size());
    }

    using const_iterator = IndexDataEntryIterator;

    size_t size() const {
        return _size;
    }
    bool empty() const {
        return _size == 0;
    }
    const_iterator begin() const {
        return IndexDataEntryIterator(_begin);
    }
    const_iterator end() const {
        return IndexDataEntryIterator(_end);
    }
    const_iterator lower_bound(RecordId loc) const;
    const_iterator upper_bound(RecordId loc) const;

    // Creates a new UniqueIndexData buffer containing an additional item. Returns boost::none if
    // entry already exists.
    boost::optional<std::string> add(RecordId loc, const KeyString::TypeBits& typeBits);

    // Creates a new UniqueIndexData buffer with item with RecordId removed. Returns boost::none if
    // entry did not exist.
    boost::optional<std::string> remove(RecordId loc);

private:
    size_t _memoryUsage() const;

    size_t _size;
    const uint8_t* _begin;
    const uint8_t* _end;
};

IndexDataEntry::IndexDataEntry(const uint8_t* buffer) : _buffer(buffer) {}
IndexDataEntry::IndexDataEntry(const std::string& indexDataEntry)
    : _buffer(reinterpret_cast<const uint8_t*>(indexDataEntry.data())) {}

std::string IndexDataEntry::create(RecordId loc, const KeyString::TypeBits& typeBits) {
    uint64_t repr = loc.repr();
    uint64_t typebitsSize = typeBits.getSize();
    std::string output(sizeof(loc) + sizeof(typebitsSize) + typebitsSize, '\0');

    // RecordId
    std::memcpy(output.data(), &repr, sizeof(repr));

    // TypeBits size
    std::memcpy(output.data() + sizeof(repr), &typebitsSize, sizeof(typebitsSize));

    // TypeBits data
    std::memcpy(
        output.data() + sizeof(repr) + sizeof(typebitsSize), typeBits.getBuffer(), typebitsSize);

    return output;
}

const uint8_t* IndexDataEntry::buffer() const {
    return _buffer;
}

size_t IndexDataEntry::size() const {
    uint64_t typeBitsSize;
    std::memcpy(&typeBitsSize, _buffer + sizeof(uint64_t), sizeof(uint64_t));

    // RecordId + TypeBits size + TypeBits buffer
    return sizeof(uint64_t) * 2 + typeBitsSize;
}

RecordId IndexDataEntry::loc() const {
    uint64_t repr;
    std::memcpy(&repr, _buffer, sizeof(uint64_t));
    return RecordId(repr);
}

KeyString::TypeBits IndexDataEntry::typeBits() const {
    uint64_t size;
    std::memcpy(&size, _buffer + sizeof(uint64_t), sizeof(uint64_t));

    BufReader reader(_buffer + sizeof(uint64_t) * 2, size);
    return KeyString::TypeBits::fromBuffer(KeyString::Version::kLatestVersion, &reader);
}

IndexDataEntryIterator::IndexDataEntryIterator(const uint8_t* entryData)
    : _entry(IndexDataEntry(entryData)) {}
void IndexDataEntryIterator::increment() {
    _entry = IndexDataEntry(_entry.buffer() + _entry.size());
}
bool IndexDataEntryIterator::equal(IndexDataEntryIterator const& other) const {
    return _entry.buffer() == other._entry.buffer();
}

const IndexDataEntry& IndexDataEntryIterator::dereference() const {
    return _entry;
}

UniqueIndexData::const_iterator UniqueIndexData::lower_bound(RecordId loc) const {
    // Linear search to the first item not less than loc
    return std::find_if_not(
        begin(), end(), [loc](const IndexDataEntry& entry) { return entry.loc() < loc; });
}
UniqueIndexData::const_iterator UniqueIndexData::upper_bound(RecordId loc) const {
    // Linear search to the first item larger than loc
    auto lb = lower_bound(loc);
    return std::find_if(
        lb, end(), [loc](const IndexDataEntry& entry) { return loc < entry.loc(); });
}

size_t UniqueIndexData::_memoryUsage() const {
    return sizeof(_size) + _end - _begin;
}

boost::optional<std::string> UniqueIndexData::add(RecordId loc,
                                                  const KeyString::TypeBits& typeBits) {
    // If entry already exists then nothing to do
    auto it = lower_bound(loc);
    if (it != end() && it->loc() == loc)
        return boost::none;

    auto itBuffer = it->buffer();
    std::string entry = IndexDataEntry::create(loc, typeBits);

    // Allocate string that fit the new entry
    std::string output(_memoryUsage() + entry.size(), '\0');
    auto pos = output.data();

    // Write number of entries
    uint64_t num = size() + 1;
    std::memcpy(pos, &num, sizeof(num));
    pos += sizeof(num);

    // Write old entries smaller than the new one
    if (auto bytes = itBuffer - _begin) {
        std::memcpy(pos, _begin, bytes);
        pos += bytes;
    }

    // Write new entry
    std::memcpy(pos, entry.data(), entry.size());
    pos += entry.size();

    // Write old entries larger than the new one
    if (auto bytes = _end - itBuffer) {
        std::memcpy(pos, itBuffer, bytes);
    }

    return output;
}
boost::optional<std::string> UniqueIndexData::remove(RecordId loc) {
    // If entry doesn't exist then nothing to do
    auto it = lower_bound(loc);
    if (it == end() || it->loc() != loc)
        return boost::none;

    // Allocate string with approrpriate amount of space
    std::string output(_memoryUsage() - it->size(), '\0');
    auto pos = output.data();

    // Write number of entries
    uint64_t num = size() - 1;
    std::memcpy(pos, &num, sizeof(num));
    pos += sizeof(num);

    // Write entries before entry to remove
    std::memcpy(pos, _begin, it->buffer() - _begin);
    pos += it->buffer() - _begin;

    // Skip entry to remove and write remaining entries
    ++it;
    std::memcpy(pos, it->buffer(), _end - it->buffer());
    return output;
}

const Ordering allAscending = Ordering::make(BSONObj());

void prefixKeyStringWithoutLoc(KeyString::Builder* keyString, const std::string& prefixToUse) {
    BSONObjBuilder b;
    b.append("", prefixToUse);                                               // prefix
    b.append("", StringData(keyString->getBuffer(), keyString->getSize()));  // key

    keyString->resetToKey(b.obj(), allAscending);
}

void prefixKeyStringWithLoc(KeyString::Builder* keyString,
                            RecordId loc,
                            const std::string& prefixToUse) {
    BSONObjBuilder b;
    b.append("", prefixToUse);                                               // prefix
    b.append("", StringData(keyString->getBuffer(), keyString->getSize()));  // key

    keyString->resetToKey(b.obj(), allAscending, loc);
}

std::string createRadixKeyWithoutLocFromObj(const BSONObj& key,
                                            const std::string& prefixToUse,
                                            Ordering order) {
    KeyString::Version version = KeyString::Version::kLatestVersion;
    KeyString::Builder ks(version, BSONObj::stripFieldNames(key), order);

    prefixKeyStringWithoutLoc(&ks, prefixToUse);
    return std::string(ks.getBuffer(), ks.getSize());
}

std::string createRadixKeyWithoutLocFromKS(const KeyString::Value& keyString,
                                           const std::string& prefixToUse) {
    KeyString::Builder ks(KeyString::Version::kLatestVersion);
    auto ksBuffer = keyString.getBuffer();
    if (ksBuffer)
        ks.resetFromBuffer(ksBuffer,
                           KeyString::sizeWithoutRecordIdAtEnd(ksBuffer, keyString.getSize()));
    prefixKeyStringWithoutLoc(&ks, prefixToUse);
    return std::string(ks.getBuffer(), ks.getSize());
}

std::string createRadixKeyWithoutLocFromKSWithoutRecordId(const KeyString::Value& keyString,
                                                          const std::string& prefixToUse) {
    KeyString::Builder ks(KeyString::Version::kLatestVersion);
    auto ksBuffer = keyString.getBuffer();
    if (ksBuffer)
        ks.resetFromBuffer(ksBuffer, keyString.getSize());
    prefixKeyStringWithoutLoc(&ks, prefixToUse);
    return std::string(ks.getBuffer(), ks.getSize());
}

std::string createRadixKeyWithLocFromObj(const BSONObj& key,
                                         RecordId loc,
                                         const std::string& prefixToUse,
                                         Ordering order) {
    KeyString::Version version = KeyString::Version::kLatestVersion;
    KeyString::Builder ks(version, BSONObj::stripFieldNames(key), order);

    prefixKeyStringWithLoc(&ks, loc, prefixToUse);
    return std::string(ks.getBuffer(), ks.getSize());
}

std::string createRadixKeyWithLocFromKS(const KeyString::Value& keyString,
                                        RecordId loc,
                                        const std::string& prefixToUse) {
    KeyString::Builder ks(KeyString::Version::kLatestVersion);
    auto ksBuffer = keyString.getBuffer();
    if (ksBuffer)
        ks.resetFromBuffer(ksBuffer,
                           KeyString::sizeWithoutRecordIdAtEnd(ksBuffer, keyString.getSize()));
    prefixKeyStringWithLoc(&ks, loc, prefixToUse);
    return std::string(ks.getBuffer(), ks.getSize());
}

std::string createRadixKeyWithLocFromKSWithoutRecordId(const KeyString::Value& keyString,
                                                       RecordId loc,
                                                       const std::string& prefixToUse) {
    KeyString::Builder ks(KeyString::Version::kLatestVersion);
    auto ksBuffer = keyString.getBuffer();
    if (ksBuffer)
        ks.resetFromBuffer(ksBuffer, keyString.getSize());
    prefixKeyStringWithLoc(&ks, loc, prefixToUse);
    return std::string(ks.getBuffer(), ks.getSize());
}

BSONObj createObjFromRadixKey(const std::string& radixKey,
                              const KeyString::TypeBits& typeBits,
                              const Ordering& order) {
    KeyString::Version version = KeyString::Version::kLatestVersion;
    KeyString::TypeBits tbOuter = KeyString::TypeBits(version);
    BSONObj bsonObj =
        KeyString::toBsonSafe(radixKey.data(), radixKey.size(), allAscending, tbOuter);

    SharedBuffer sb;
    auto it = BSONObjIterator(bsonObj);
    ++it;  // We want the second part
    KeyString::Builder ks(version);
    ks.resetFromBuffer((*it).valuestr(), (*it).valuestrsize());

    return KeyString::toBsonSafe(ks.getBuffer(), ks.getSize(), order, typeBits);
}

IndexKeyEntry createIndexKeyEntryFromRadixKey(const std::string& radixKey,
                                              RecordId loc,
                                              const KeyString::TypeBits& typeBits,
                                              const Ordering order) {
    return IndexKeyEntry(createObjFromRadixKey(radixKey, typeBits, order), loc);
}

IndexKeyEntry createIndexKeyEntryFromRadixKey(const std::string& radixKey,
                                              const std::string& indexDataEntry,
                                              const Ordering order) {
    IndexDataEntry data(indexDataEntry);
    return IndexKeyEntry(createObjFromRadixKey(radixKey, data.typeBits(), order), data.loc());
}

boost::optional<KeyStringEntry> createKeyStringEntryFromRadixKey(
    const std::string& radixKey,
    RecordId loc,
    const KeyString::TypeBits& typeBits,
    const Ordering& order) {
    auto key = createObjFromRadixKey(radixKey, typeBits, order);
    KeyString::Builder ksFinal(KeyString::Version::kLatestVersion, key, order);
    ksFinal.appendRecordId(loc);
    return KeyStringEntry(ksFinal.getValueCopy(), loc);
}

boost::optional<KeyStringEntry> createKeyStringEntryFromRadixKey(const std::string& radixKey,
                                                                 const std::string& indexDataEntry,
                                                                 const Ordering& order) {
    IndexDataEntry data(indexDataEntry);
    RecordId loc = data.loc();
    auto key = createObjFromRadixKey(radixKey, data.typeBits(), order);
    KeyString::Builder ksFinal(KeyString::Version::kLatestVersion, key, order);
    ksFinal.appendRecordId(loc);
    return KeyStringEntry(ksFinal.getValueCopy(), loc);
}

/*
 * This is the base cursor class required by the sorted data interface.
 * Using CRTP (static inheritance) to reuse shared implementation for cursors over unique and
 * standard indexes
 */
template <class CursorImpl>
class CursorBase : public ::mongo::SortedDataInterface::Cursor {
public:
    // All the following public functions just implement the interface.
    CursorBase(OperationContext* opCtx,
               bool isForward,
               // This is the ident.
               std::string _prefix,
               // This is a string immediately after the ident and before other idents.
               std::string _identEnd,
               StringStore* workingCopy,
               Ordering order,
               std::string prefixBSON,
               std::string KSForIdentEnd);
    virtual void setEndPosition(const BSONObj& key, bool inclusive) override;
    virtual boost::optional<IndexKeyEntry> seek(const KeyString::Value& keyString,
                                                RequestedInfo parts = kKeyAndLoc) override;
    virtual boost::optional<KeyStringEntry> seekForKeyString(
        const KeyString::Value& keyStringValue) override;
    virtual boost::optional<KeyStringEntry> seekExactForKeyString(
        const KeyString::Value& keyStringValue) override;
    virtual boost::optional<IndexKeyEntry> seekExact(const KeyString::Value& keyStringValue,
                                                     RequestedInfo) override;
    virtual void save() override;
    virtual void restore() override;
    virtual void detachFromOperationContext() override;
    virtual void reattachToOperationContext(OperationContext* opCtx) override;

private:
    // CRTP Interface
    std::string createRadixKeyFromObj(const BSONObj& key,
                                      RecordId loc,
                                      const std::string& prefixToUse,
                                      Ordering order) {
        return static_cast<CursorImpl*>(this)->createRadixKeyFromObj(key, loc, prefixToUse, order);
    }
    std::string createRadixKeyFromKSWithoutRecordId(const KeyString::Value& keyString,
                                                    RecordId loc,
                                                    const std::string& prefixToUse) {
        return static_cast<CursorImpl*>(this)->createRadixKeyFromKSWithoutRecordId(
            keyString, loc, prefixToUse);
    }
    boost::optional<KeyStringEntry> finishSeekAfterProcessing() {
        return static_cast<CursorImpl*>(this)->finishSeekAfterProcessing();
    }
    bool advanceNextInternal() {
        return static_cast<CursorImpl*>(this)->advanceNextInternal();
    }
    void finishAdvanceNext() {
        static_cast<CursorImpl*>(this)->finishAdvanceNext();
    }
    bool checkCursorValid() {
        return static_cast<CursorImpl*>(this)->checkCursorValid();
    }
    void saveForward() {
        return static_cast<CursorImpl*>(this)->saveForward();
    }
    void saveReverse() {
        return static_cast<CursorImpl*>(this)->saveReverse();
    }
    void restoreForward() {
        return static_cast<CursorImpl*>(this)->restoreForward();
    }
    void restoreReverse() {
        return static_cast<CursorImpl*>(this)->restoreReverse();
    }

protected:
    bool advanceNext();
    // This is a helper function to check if the cursor was explicitly set by the user or not.
    bool endPosSet();
    // This is a helper function for seek.
    boost::optional<IndexKeyEntry> seekAfterProcessing(BSONObj finalKey);
    boost::optional<KeyStringEntry> seekAfterProcessing(const KeyString::Value& keyString);
    OperationContext* _opCtx;
    // This is the "working copy" of the master "branch" in the git analogy.
    StringStore* _workingCopy;
    // These store the end positions.
    boost::optional<StringStore::const_iterator> _endPos;
    boost::optional<StringStore::const_reverse_iterator> _endPosReverse;
    // This means if the cursor is a forward or reverse cursor.
    bool _forward;
    // This means whether the cursor has reached the last EOF (with regard to this index).
    bool _atEOF;
    // This means whether or not the last move was restore.
    bool _lastMoveWasRestore;
    // This is the keystring for the saved location.
    std::string _saveKey;
    RecordId _saveLoc;
    // These are the same as before.
    std::string _prefix;
    std::string _identEnd;
    // These two store the const_iterator, which is the data structure for cursors. The one we
    // use depends on _forward.
    StringStore::const_iterator _forwardIt;
    StringStore::const_reverse_iterator _reverseIt;
    // This is the ordering for the key's values for multi-field keys.
    Ordering _order;
    // This stores whether or not the end position is inclusive for restore.
    bool _endPosIncl;
    // This stores the key for the end position.
    boost::optional<BSONObj> _endPosKey;
    // The next two are the same as above.
    std::string _KSForIdentStart;
    std::string _KSForIdentEnd;
};

// Cursor
template <class CursorImpl>
CursorBase<CursorImpl>::CursorBase(OperationContext* opCtx,
                                   bool isForward,
                                   std::string _prefix,
                                   std::string _identEnd,
                                   StringStore* workingCopy,
                                   Ordering order,
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
      _KSForIdentStart(_KSForIdentStart),
      _KSForIdentEnd(identEndBSON) {}

template <class CursorImpl>
bool CursorBase<CursorImpl>::advanceNext() {
    if (!_atEOF) {
        // If the last move was restore, then we don't need to advance the cursor, since the user
        // never got the value the cursor was pointing to in the first place. However,
        // _lastMoveWasRestore will go through extra logic on a unique index, since unique indexes
        // are not allowed to return the same key twice.
        if (_lastMoveWasRestore) {
            _lastMoveWasRestore = false;
        } else {
            if (advanceNextInternal())
                return true;

            // We basically just check to make sure the cursor is in the ident.
            if (_forward && checkCursorValid()) {
                ++_forwardIt;
            } else if (!_forward && checkCursorValid()) {
                ++_reverseIt;
            }
            // We check here to make sure that we are on the correct side of the end position, and
            // that the cursor is still in the ident after advancing.
            if (!checkCursorValid()) {
                _atEOF = true;
                return false;
            }
        }
    } else {
        _lastMoveWasRestore = false;
        return false;
    }

    finishAdvanceNext();

    return true;
}

// This function checks whether or not the cursor end position was set by the user or not.
template <class CursorImpl>
bool CursorBase<CursorImpl>::endPosSet() {
    return (_forward && _endPos != boost::none) || (!_forward && _endPosReverse != boost::none);
}

template <class CursorImpl>
void CursorBase<CursorImpl>::setEndPosition(const BSONObj& key, bool inclusive) {
    StringStore* workingCopy(RecoveryUnit::get(_opCtx)->getHead());
    if (key.isEmpty()) {
        _endPos = boost::none;
        _endPosReverse = boost::none;
        return;
    }
    _endPosIncl = inclusive;
    _endPosKey = key;
    StringStore::const_iterator it;
    // If forward and inclusive or reverse and not inclusive, then we use the last element in this
    // ident. Otherwise, we use the first as our bound.
    if (_forward == inclusive)
        it = workingCopy->upper_bound(createRadixKeyFromObj(key, RecordId::max(), _prefix, _order));
    else
        it = workingCopy->lower_bound(createRadixKeyFromObj(key, RecordId::min(), _prefix, _order));
    if (_forward)
        _endPos = it;
    else
        _endPosReverse = StringStore::const_reverse_iterator(it);
}

template <class CursorImpl>
boost::optional<IndexKeyEntry> CursorBase<CursorImpl>::seekAfterProcessing(BSONObj finalKey) {
    std::string workingCopyBound;

    KeyString::Builder ks(KeyString::Version::kLatestVersion, finalKey, _order);
    auto ksEntry = seekAfterProcessing(ks.getValueCopy());

    const BSONObj bson = KeyString::toBson(ksEntry->keyString.getBuffer(),
                                           ksEntry->keyString.getSize(),
                                           _order,
                                           ksEntry->keyString.getTypeBits());
    return IndexKeyEntry(bson, ksEntry->loc);
}

template <class CursorImpl>
boost::optional<KeyStringEntry> CursorBase<CursorImpl>::seekAfterProcessing(
    const KeyString::Value& keyStringVal) {

    KeyString::Discriminator discriminator = KeyString::decodeDiscriminator(
        keyStringVal.getBuffer(), keyStringVal.getSize(), _order, keyStringVal.getTypeBits());

    bool inclusive;
    switch (discriminator) {
        case KeyString::Discriminator::kInclusive:
            inclusive = true;
            break;
        case KeyString::Discriminator::kExclusiveBefore:
            inclusive = _forward;
            break;
        case KeyString::Discriminator::kExclusiveAfter:
            inclusive = !_forward;
            break;
    }

    // If the key is empty and it's not inclusive, then no elements satisfy this seek.
    if (keyStringVal.isEmpty() && !inclusive) {
        _atEOF = true;
        return boost::none;
    }

    StringStore::const_iterator it;
    // Forward inclusive seek uses lower_bound and exclusive upper_bound. For reverse iterators this
    // is also reversed.
    if (_forward == inclusive)
        it = _workingCopy->lower_bound(
            createRadixKeyFromKSWithoutRecordId(keyStringVal, RecordId::min(), _prefix));
    else
        it = _workingCopy->upper_bound(
            createRadixKeyFromKSWithoutRecordId(keyStringVal, RecordId::max(), _prefix));
    if (_forward)
        _forwardIt = it;
    else
        _reverseIt = StringStore::const_reverse_iterator(it);

    // Here, we check to make sure the iterator doesn't fall off the data structure and is
    // in the ident. We also check to make sure it is on the correct side of the end
    // position, if it was set.
    if (!checkCursorValid()) {
        _atEOF = true;
        return boost::none;
    }

    return finishSeekAfterProcessing();
}

template <class CursorImpl>
boost::optional<IndexKeyEntry> CursorBase<CursorImpl>::seek(const KeyString::Value& keyString,
                                                            RequestedInfo parts) {
    boost::optional<KeyStringEntry> ksValue = seekForKeyString(keyString);
    if (ksValue) {
        BSONObj bson = KeyString::toBson(ksValue->keyString.getBuffer(),
                                         ksValue->keyString.getSize(),
                                         _order,
                                         ksValue->keyString.getTypeBits());
        return IndexKeyEntry(bson, ksValue->loc);
    }
    return boost::none;
}

template <class CursorImpl>
boost::optional<KeyStringEntry> CursorBase<CursorImpl>::seekForKeyString(
    const KeyString::Value& keyStringValue) {
    _lastMoveWasRestore = false;
    _atEOF = false;
    return seekAfterProcessing(keyStringValue);
}

template <class CursorImpl>
boost::optional<KeyStringEntry> CursorBase<CursorImpl>::seekExactForKeyString(
    const KeyString::Value& keyStringValue) {
    dassert(KeyString::decodeDiscriminator(keyStringValue.getBuffer(),
                                           keyStringValue.getSize(),
                                           _order,
                                           keyStringValue.getTypeBits()) ==
            KeyString::Discriminator::kInclusive);
    auto ksEntry = seekForKeyString(keyStringValue);
    if (!ksEntry) {
        return {};
    }
    if (KeyString::compare(ksEntry->keyString.getBuffer(),
                           keyStringValue.getBuffer(),
                           KeyString::sizeWithoutRecordIdAtEnd(ksEntry->keyString.getBuffer(),
                                                               ksEntry->keyString.getSize()),
                           keyStringValue.getSize()) == 0) {
        return KeyStringEntry(ksEntry->keyString, ksEntry->loc);
    }
    return {};
}

template <class CursorImpl>
boost::optional<IndexKeyEntry> CursorBase<CursorImpl>::seekExact(
    const KeyString::Value& keyStringValue, RequestedInfo parts) {
    auto ksEntry = seekExactForKeyString(keyStringValue);
    if (!ksEntry) {
        return {};
    }

    BSONObj bson;
    if (parts & SortedDataInterface::Cursor::kWantKey) {
        bson = KeyString::toBson(ksEntry->keyString.getBuffer(),
                                 ksEntry->keyString.getSize(),
                                 _order,
                                 ksEntry->keyString.getTypeBits());
    }
    return IndexKeyEntry(std::move(bson), ksEntry->loc);
}

template <class CursorImpl>
void CursorBase<CursorImpl>::save() {
    _atEOF = false;
    if (_lastMoveWasRestore) {
        return;
    } else if (_forward && checkCursorValid()) {
        _saveKey = _forwardIt->first;
        saveForward();
    } else if (!_forward && checkCursorValid()) {  // reverse
        _saveKey = _reverseIt->first;
        saveReverse();
    } else {
        _saveKey = "";
        _saveLoc = RecordId();
    }
}

template <class CursorImpl>
void CursorBase<CursorImpl>::restore() {
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
        restoreForward();
    } else {
        // Now we are dealing with reverse cursors, and use similar logic.
        if (_saveKey.length() == 0) {
            _reverseIt = workingCopy->rend();
        } else {
            _reverseIt = StringStore::const_reverse_iterator(workingCopy->upper_bound(_saveKey));
        }
        restoreReverse();
    }
}

template <class CursorImpl>
void CursorBase<CursorImpl>::detachFromOperationContext() {
    _opCtx = nullptr;
}

template <class CursorImpl>
void CursorBase<CursorImpl>::reattachToOperationContext(OperationContext* opCtx) {
    this->_opCtx = opCtx;
}

/*
 * This is the cursor class required by the sorted data interface for unique indexes.
 */
class CursorUnique final : public CursorBase<CursorUnique> {
public:
    using CursorBase::CursorBase;

    virtual boost::optional<IndexKeyEntry> next(RequestedInfo parts = kKeyAndLoc) override;
    virtual boost::optional<KeyStringEntry> nextKeyString() override;

private:
    // Implementations of CursorBase interface
    friend class CursorBase;

    bool advanceNextInternal();
    void finishAdvanceNext();
    std::string createRadixKeyFromObj(const BSONObj& key,
                                      RecordId loc,
                                      const std::string& prefixToUse,
                                      Ordering order) {
        return createRadixKeyWithoutLocFromObj(key, prefixToUse, order);
    }
    std::string createRadixKeyFromKSWithoutRecordId(const KeyString::Value& keyString,
                                                    RecordId loc,
                                                    const std::string& prefixToUse) {
        return createRadixKeyWithoutLocFromKSWithoutRecordId(keyString, prefixToUse);
    }
    boost::optional<KeyStringEntry> finishSeekAfterProcessing();

    void saveForward();
    void saveReverse();
    void restoreForward();
    void restoreReverse();

    // This is a helper function to check if the cursor is valid or not.
    bool checkCursorValid();
    // Helper function to set index data iterators to reverse position, we cannot use reverse
    // iterators because we only have forward iterator support over this data
    void initReverseDataIterators();
    // Unpacked data from current position in the radix tree. Needed to iterate over indexes
    // containing duplicates
    UniqueIndexData _indexData;
    UniqueIndexData::const_iterator _indexDataIt;
    UniqueIndexData::const_iterator _indexDataEnd;
    size_t _reversePos;
};

bool CursorUnique::advanceNextInternal() {
    // Iterate over duplicates before moving to the next item in the radix tree
    if (!_indexData.empty()) {
        if (_forward) {
            if (++_indexDataIt != _indexDataEnd)
                return true;
        } else {
            if (++_reversePos < _indexData.size()) {
                initReverseDataIterators();
                return true;
            }
        }
    }
    return false;
}

void CursorUnique::finishAdvanceNext() {
    // We have moved to a new position in the tree, initialize index data for iterating over
    // duplicates
    if (_forward) {
        _indexData = UniqueIndexData(_forwardIt->second);
        _indexDataIt = _indexData.begin();
        _indexDataEnd = _indexData.end();
    } else {
        _indexData = UniqueIndexData(_reverseIt->second);
        _reversePos = 0;
        initReverseDataIterators();
    }
}

// This function checks whether or not a cursor is valid. In particular, it checks 1) whether the
// cursor is at end() or rend(), 2) whether the cursor is on the wrong side of the end position
// if it was set, and 3) whether the cursor is still in the ident.
bool CursorUnique::checkCursorValid() {
    if (_forward) {
        if (_forwardIt == _workingCopy->end()) {
            return false;
        }
        if (endPosSet()) {
            // The endPos must be in the ident, at most one past the ident, or end. Therefore, the
            // endPos includes the check for being inside the ident
            if (_endPosIncl) {
                if (*_endPos == _workingCopy->end())
                    return true;

                // For unique indexes, we need to check if the cursor moved up a position when it
                // was restored. This isn't required for non-unique indexes because we store the
                // RecordId in the KeyString and use a "<" comparison instead of "<=" since we know
                // that no RecordId will ever reach RecordId::max() so we don't need to check the
                // equal side of things. This assumption doesn't hold for unique index KeyStrings.
                std::string endPosKeyString =
                    createRadixKeyFromObj(*_endPosKey, RecordId::min(), _prefix, _order);

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
            if (_endPosIncl) {
                if (*_endPosReverse == _workingCopy->rend())
                    return true;

                std::string endPosKeyString =
                    createRadixKeyFromObj(*_endPosKey, RecordId::min(), _prefix, _order);

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

void CursorUnique::initReverseDataIterators() {
    _indexDataIt = _indexData.begin();
    _indexDataEnd = _indexData.end();
    for (size_t i = 1; i < (_indexData.size() - _reversePos); ++i)
        ++_indexDataIt;
}

boost::optional<IndexKeyEntry> CursorUnique::next(RequestedInfo parts) {
    if (!advanceNext()) {
        return {};
    }

    if (_forward) {
        return createIndexKeyEntryFromRadixKey(
            _forwardIt->first, _indexDataIt->loc(), _indexDataIt->typeBits(), _order);
    }
    return createIndexKeyEntryFromRadixKey(
        _reverseIt->first, _indexDataIt->loc(), _indexDataIt->typeBits(), _order);
}

boost::optional<KeyStringEntry> CursorUnique::nextKeyString() {
    if (!advanceNext()) {
        return {};
    }

    if (_forward) {
        return createKeyStringEntryFromRadixKey(
            _forwardIt->first, _indexDataIt->loc(), _indexDataIt->typeBits(), _order);
    }
    return createKeyStringEntryFromRadixKey(
        _reverseIt->first, _indexDataIt->loc(), _indexDataIt->typeBits(), _order);
}

boost::optional<KeyStringEntry> CursorUnique::finishSeekAfterProcessing() {
    // We have seeked to an entry in the tree. Now unpack the data and initialize iterators to point
    // to the first entry if this index contains duplicates
    if (_forward) {
        _indexData = UniqueIndexData(_forwardIt->second);
        _indexDataIt = _indexData.begin();
        _indexDataEnd = _indexData.end();
        return createKeyStringEntryFromRadixKey(
            _forwardIt->first, _indexDataIt->loc(), _indexDataIt->typeBits(), _order);
    } else {
        _indexData = UniqueIndexData(_reverseIt->second);
        _reversePos = 0;
        initReverseDataIterators();
        return createKeyStringEntryFromRadixKey(
            _reverseIt->first, _indexDataIt->loc(), _indexDataIt->typeBits(), _order);
    }
}

void CursorUnique::saveForward() {
    if (!_indexData.empty()) {
        _saveLoc = _indexDataIt->loc();
    }
}

void CursorUnique::saveReverse() {
    if (!_indexData.empty()) {
        _saveLoc = _indexDataIt->loc();
    }
}

void CursorUnique::restoreForward() {
    _lastMoveWasRestore = true;
    if (_saveLoc != RecordId() && _forwardIt != _workingCopy->end() &&
        _forwardIt->first == _saveKey) {
        _indexData = UniqueIndexData(_forwardIt->second);
        _indexDataIt = _indexData.lower_bound(_saveLoc);
        _indexDataEnd = _indexData.end();
        if (_indexDataIt == _indexDataEnd) {
            // We reached the end of the index data, so we need to go to the next item in the
            // radix tree to be positioned on a valid item
            ++_forwardIt;
            if (checkCursorValid()) {
                _indexData = UniqueIndexData(_forwardIt->second);
                _indexDataIt = _indexData.begin();
                _indexDataEnd = _indexData.end();
            }
        } else {
            // Unique indexes disregard difference in location and forces the cursor to advance
            // to guarantee that we never return the same key twice
            _lastMoveWasRestore = false;
        }
    }
    if (!checkCursorValid()) {
        _atEOF = true;
    }
}

void CursorUnique::restoreReverse() {
    _lastMoveWasRestore = true;
    if (_saveLoc != RecordId() && _reverseIt != _workingCopy->rend() &&
        _reverseIt->first == _saveKey) {
        _indexData = UniqueIndexData(_reverseIt->second);
        _indexDataIt = _indexData.upper_bound(_saveLoc);
        _indexDataEnd = _indexData.end();
        if (_indexDataIt == _indexDataEnd) {
            ++_reverseIt;
            if (checkCursorValid()) {
                _indexData = UniqueIndexData(_reverseIt->second);
                _reversePos = 0;
                initReverseDataIterators();
            }
        } else {
            _reversePos = _indexData.size() - std::distance(_indexData.begin(), _indexDataIt) - 1;
            _lastMoveWasRestore = false;
        }
    }
    if (!checkCursorValid()) {
        _atEOF = true;
    }
}

/*
 * This is the cursor class required by the sorted data interface for standard (non-unique) indexes.
 */
class CursorStandard final : public CursorBase<CursorStandard> {
public:
    using CursorBase::CursorBase;

    virtual boost::optional<IndexKeyEntry> next(RequestedInfo parts = kKeyAndLoc) override;
    virtual boost::optional<KeyStringEntry> nextKeyString() override;

protected:
    // Implementations of CursorBase interface
    friend class CursorBase;

    bool advanceNextInternal() {
        return false;
    }
    void finishAdvanceNext() {}
    std::string createRadixKeyFromObj(const BSONObj& key,
                                      RecordId loc,
                                      const std::string& prefixToUse,
                                      Ordering order) {
        return createRadixKeyWithLocFromObj(key, loc, prefixToUse, order);
    }
    std::string createRadixKeyFromKSWithoutRecordId(const KeyString::Value& keyString,
                                                    RecordId loc,
                                                    const std::string& prefixToUse) {
        return createRadixKeyWithLocFromKSWithoutRecordId(keyString, loc, prefixToUse);
    }
    boost::optional<KeyStringEntry> finishSeekAfterProcessing();
    void saveForward() {}
    void saveReverse() {}
    void restoreForward();
    void restoreReverse();

private:
    // This is a helper function to check if the cursor is valid or not.
    bool checkCursorValid();
};

// This function checks whether or not a cursor is valid. In particular, it checks 1) whether the
// cursor is at end() or rend(), 2) whether the cursor is on the wrong side of the end position
// if it was set, and 3) whether the cursor is still in the ident.
bool CursorStandard::checkCursorValid() {
    if (_forward) {
        if (_forwardIt == _workingCopy->end()) {
            return false;
        }
        if (endPosSet()) {
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
            return *_endPosReverse == _workingCopy->rend() ||
                _reverseIt->first.compare((*_endPosReverse)->first) > 0;
        }
        return _reverseIt->first.compare(_KSForIdentStart) >= 0;
    }
}


boost::optional<IndexKeyEntry> CursorStandard::next(RequestedInfo parts) {
    if (!advanceNext()) {
        return {};
    }

    if (_forward) {
        return createIndexKeyEntryFromRadixKey(_forwardIt->first, _forwardIt->second, _order);
    }
    return createIndexKeyEntryFromRadixKey(_reverseIt->first, _reverseIt->second, _order);
}

boost::optional<KeyStringEntry> CursorStandard::nextKeyString() {
    if (!advanceNext()) {
        return {};
    }

    if (_forward) {
        return createKeyStringEntryFromRadixKey(_forwardIt->first, _forwardIt->second, _order);
    }
    return createKeyStringEntryFromRadixKey(_reverseIt->first, _reverseIt->second, _order);
}

boost::optional<KeyStringEntry> CursorStandard::finishSeekAfterProcessing() {
    // We have seeked to an entry in the tree.
    if (_forward) {
        return createKeyStringEntryFromRadixKey(_forwardIt->first, _forwardIt->second, _order);
    } else {
        return createKeyStringEntryFromRadixKey(_reverseIt->first, _reverseIt->second, _order);
    }
}

void CursorStandard::restoreForward() {
    if (!checkCursorValid()) {
        _atEOF = true;
        _lastMoveWasRestore = true;
        return;
    }
    _lastMoveWasRestore = (_forwardIt->first.compare(_saveKey) != 0);
}
void CursorStandard::restoreReverse() {
    if (!checkCursorValid()) {
        _atEOF = true;
        _lastMoveWasRestore = true;
        return;
    }
    _lastMoveWasRestore = (_reverseIt->first.compare(_saveKey) != 0);
}

}  // namespace

SortedDataBuilderBase::SortedDataBuilderBase(OperationContext* opCtx,
                                             bool dupsAllowed,
                                             Ordering order,
                                             const std::string& prefix,
                                             const std::string& identEnd,
                                             const IndexDescriptor* desc,
                                             const std::string& indexName,
                                             const BSONObj& keyPattern,
                                             const BSONObj& collation)
    : _opCtx(opCtx),
      _dupsAllowed(dupsAllowed),
      _order(order),
      _prefix(prefix),
      _identEnd(identEnd),
      _desc(desc),
      _indexName(indexName),
      _keyPattern(keyPattern),
      _collation(collation) {}

void SortedDataBuilderBase::commit(bool mayInterrupt) {
    WriteUnitOfWork wunit(_opCtx);
    wunit.commit();
}

Status SortedDataBuilderUnique::addKey(const KeyString::Value& keyString) {
    dassert(KeyString::decodeRecordIdAtEnd(keyString.getBuffer(), keyString.getSize()).isValid());
    StringStore* workingCopy(RecoveryUnit::get(_opCtx)->getHead());
    RecordId loc = KeyString::decodeRecordIdAtEnd(keyString.getBuffer(), keyString.getSize());

    std::string key = createRadixKeyWithoutLocFromKS(keyString, _prefix);
    auto it = workingCopy->find(key);
    if (it != workingCopy->end()) {
        if (!_dupsAllowed) {
            // There was an attempt to create an index entry with a different RecordId while dups
            // were not allowed.
            auto obj = KeyString::toBson(keyString, _order);
            return buildDupKeyErrorStatus(_opCtx, keyString, _order, _desc);
        }

        UniqueIndexData data(it->second);
        // Bulk builder add keys in ascending order so we should insert at the end
        auto added = data.add(loc, keyString.getTypeBits());
        if (!added) {
            // Already indexed
            return Status::OK();
        }

        workingCopy->update({std::move(key), *added});
    } else {
        UniqueIndexData data;
        workingCopy->insert({std::move(key), *data.add(loc, keyString.getTypeBits())});
    }

    RecoveryUnit::get(_opCtx)->makeDirty();
    return Status::OK();
}

// We append \1 to all idents we get, and therefore the KeyString with ident + \0 will only be
// before elements in this ident, and the KeyString with ident + \2 will only be after elements in
// this ident.
SortedDataInterfaceBase::SortedDataInterfaceBase(OperationContext* opCtx,
                                                 StringData ident,
                                                 const IndexDescriptor* desc)
    : ::mongo::SortedDataInterface(
          ident, KeyString::Version::kLatestVersion, Ordering::make(desc->keyPattern())),
      // All entries in this ident will have a prefix of ident + \1.
      _prefix(ident.toString().append(1, '\1')),
      // Therefore, the string ident + \2 will be greater than all elements in this ident.
      _identEnd(ident.toString().append(1, '\2')),
      _desc(desc),
      _indexName(desc->indexName()),
      _keyPattern(desc->keyPattern()),
      _collation(desc->collation()),
      _isPartial(desc->isPartial()) {}

SortedDataInterfaceBase::SortedDataInterfaceBase(const Ordering& ordering, StringData ident)
    : ::mongo::SortedDataInterface(ident, KeyString::Version::kLatestVersion, ordering),
      _prefix(ident.toString().append(1, '\1')),
      _identEnd(ident.toString().append(1, '\2')),
      _desc(nullptr),
      _isPartial(false) {}

SortedDataBuilderInterface* SortedDataInterfaceUnique::getBulkBuilder(OperationContext* opCtx,
                                                                      bool dupsAllowed) {
    return new SortedDataBuilderUnique(opCtx,
                                       dupsAllowed,
                                       _ordering,
                                       _prefix,
                                       _identEnd,
                                       _desc,
                                       _indexName,
                                       _keyPattern,
                                       _collation);
}

// We append \1 to all idents we get, and therefore the KeyString with ident + \0 will only be
// before elements in this ident, and the KeyString with ident + \2 will only be after elements in
// this ident.
SortedDataInterfaceUnique::SortedDataInterfaceUnique(OperationContext* opCtx,
                                                     StringData ident,
                                                     const IndexDescriptor* desc)
    : SortedDataInterfaceBase(opCtx, ident, desc) {
    // This is the string representation of the KeyString before elements in this ident, which is
    // ident + \0. This is before all elements in this ident.
    _KSForIdentStart =
        createRadixKeyWithoutLocFromObj(BSONObj(), ident.toString().append(1, '\0'), _ordering);
    // Similarly, this is the string representation of the KeyString for something greater than
    // all other elements in this ident.
    _KSForIdentEnd = createRadixKeyWithoutLocFromObj(BSONObj(), _identEnd, _ordering);
}

SortedDataInterfaceUnique::SortedDataInterfaceUnique(const Ordering& ordering, StringData ident)
    : SortedDataInterfaceBase(ordering, ident) {
    _KSForIdentStart =
        createRadixKeyWithoutLocFromObj(BSONObj(), ident.toString().append(1, '\0'), _ordering);
    _KSForIdentEnd = createRadixKeyWithoutLocFromObj(BSONObj(), _identEnd, _ordering);
}

Status SortedDataInterfaceUnique::insert(OperationContext* opCtx,
                                         const KeyString::Value& keyString,
                                         bool dupsAllowed) {
    StringStore* workingCopy(RecoveryUnit::get(opCtx)->getHead());
    RecordId loc = KeyString::decodeRecordIdAtEnd(keyString.getBuffer(), keyString.getSize());

    std::string key = createRadixKeyWithoutLocFromKS(keyString, _prefix);
    auto it = workingCopy->find(key);
    if (it != workingCopy->end()) {
        if (!dupsAllowed) {
            // There was an attempt to create an index entry with a different RecordId while
            // dups were not allowed.
            return buildDupKeyErrorStatus(opCtx, keyString, _ordering, _desc);
        }

        UniqueIndexData data(it->second);
        auto added = data.add(loc, keyString.getTypeBits());
        if (!added) {
            // Already indexed
            return Status::OK();
        }

        workingCopy->update({std::move(key), *added});
    } else {
        UniqueIndexData data;
        workingCopy->insert({std::move(key), *data.add(loc, keyString.getTypeBits())});
    }
    RecoveryUnit::get(opCtx)->makeDirty();
    return Status::OK();
}

void SortedDataInterfaceUnique::unindex(OperationContext* opCtx,
                                        const KeyString::Value& keyString,
                                        bool dupsAllowed) {
    StringStore* workingCopy(RecoveryUnit::get(opCtx)->getHead());
    RecordId loc = KeyString::decodeRecordIdAtEnd(keyString.getBuffer(), keyString.getSize());

    auto key = createRadixKeyWithoutLocFromKS(keyString, _prefix);
    auto it = workingCopy->find(key);
    if (it != workingCopy->end()) {
        UniqueIndexData data(it->second);
        auto removed = data.remove(loc);
        if (!removed)
            return;  // loc not found, nothing to unindex

        if (UniqueIndexData(*removed).empty()) {
            workingCopy->erase(key);
        } else {
            workingCopy->update({std::move(key), *removed});
        }
        RecoveryUnit::get(opCtx)->makeDirty();
    }
}

// This function is, as of now, not in the interface, but there exists a server ticket to add
// truncate to the list of commands able to be used.
Status SortedDataInterfaceBase::truncate(mongo::RecoveryUnit* ru) {
    auto bRu = checked_cast<ephemeral_for_test::RecoveryUnit*>(ru);
    StringStore* workingCopy(bRu->getHead());
    std::vector<std::string> toDelete;
    auto end = workingCopy->upper_bound(_KSForIdentEnd);
    for (auto it = workingCopy->lower_bound(_KSForIdentStart); it != end; ++it) {
        toDelete.push_back(it->first);
    }
    if (!toDelete.empty()) {
        for (const auto& key : toDelete)
            workingCopy->erase(key);
        bRu->makeDirty();
    }

    return Status::OK();
}

Status SortedDataInterfaceUnique::dupKeyCheck(OperationContext* opCtx,
                                              const KeyString::Value& key) {
    StringStore* workingCopy(RecoveryUnit::get(opCtx)->getHead());

    std::string radixKey = createRadixKeyWithoutLocFromKSWithoutRecordId(key, _prefix);
    auto it = workingCopy->find(radixKey);
    if (it == workingCopy->end())
        return Status::OK();

    UniqueIndexData data(it->second);
    if (data.size() > 1) {
        return buildDupKeyErrorStatus(opCtx, key, _ordering, _desc);
    }

    return Status::OK();
}

void SortedDataInterfaceUnique::fullValidate(OperationContext* opCtx,
                                             long long* numKeysOut,
                                             ValidateResults* fullResults) const {
    StringStore* workingCopy(RecoveryUnit::get(opCtx)->getHead());
    long long numKeys = 0;
    auto it = workingCopy->lower_bound(_KSForIdentStart);
    while (it != workingCopy->end() && it->first.compare(_KSForIdentEnd) < 0) {
        numKeys += UniqueIndexData(it->second).size();
        ++it;
    }
    *numKeysOut = numKeys;
}

bool SortedDataInterfaceBase::appendCustomStats(OperationContext* opCtx,
                                                BSONObjBuilder* output,
                                                double scale) const {
    return false;
}

long long SortedDataInterfaceBase::getSpaceUsedBytes(OperationContext* opCtx) const {
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

bool SortedDataInterfaceBase::isEmpty(OperationContext* opCtx) {
    StringStore* workingCopy(RecoveryUnit::get(opCtx)->getHead());
    return workingCopy->distance(workingCopy->lower_bound(_KSForIdentStart),
                                 workingCopy->upper_bound(_KSForIdentEnd)) == 0;
}

std::unique_ptr<mongo::SortedDataInterface::Cursor> SortedDataInterfaceUnique::newCursor(
    OperationContext* opCtx, bool isForward) const {
    StringStore* workingCopy(RecoveryUnit::get(opCtx)->getHead());

    return std::make_unique<CursorUnique>(opCtx,
                                          isForward,
                                          _prefix,
                                          _identEnd,
                                          workingCopy,
                                          _ordering,
                                          _KSForIdentStart,
                                          _KSForIdentEnd);
}

Status SortedDataInterfaceBase::initAsEmpty(OperationContext* opCtx) {
    return Status::OK();
}

Status SortedDataBuilderStandard::addKey(const KeyString::Value& keyString) {
    dassert(KeyString::decodeRecordIdAtEnd(keyString.getBuffer(), keyString.getSize()).isValid());
    StringStore* workingCopy(RecoveryUnit::get(_opCtx)->getHead());
    RecordId loc = KeyString::decodeRecordIdAtEnd(keyString.getBuffer(), keyString.getSize());

    std::string key = createRadixKeyWithLocFromKS(keyString, loc, _prefix);
    bool inserted =
        workingCopy->insert({std::move(key), IndexDataEntry::create(loc, keyString.getTypeBits())})
            .second;
    if (inserted)
        RecoveryUnit::get(_opCtx)->makeDirty();
    return Status::OK();
}

SortedDataBuilderInterface* SortedDataInterfaceStandard::getBulkBuilder(OperationContext* opCtx,
                                                                        bool dupsAllowed) {
    return new SortedDataBuilderStandard(opCtx,
                                         dupsAllowed,
                                         _ordering,
                                         _prefix,
                                         _identEnd,
                                         _desc,
                                         _indexName,
                                         _keyPattern,
                                         _collation);
}

// We append \1 to all idents we get, and therefore the KeyString with ident + \0 will only be
// before elements in this ident, and the KeyString with ident + \2 will only be after elements in
// this ident.
SortedDataInterfaceStandard::SortedDataInterfaceStandard(OperationContext* opCtx,
                                                         StringData ident,
                                                         const IndexDescriptor* desc)
    : SortedDataInterfaceBase(opCtx, ident, desc) {
    // This is the string representation of the KeyString before elements in this ident, which is
    // ident + \0. This is before all elements in this ident.
    _KSForIdentStart = createRadixKeyWithLocFromObj(
        BSONObj(), RecordId::min(), ident.toString().append(1, '\0'), _ordering);
    // Similarly, this is the string representation of the KeyString for something greater than
    // all other elements in this ident.
    _KSForIdentEnd = createRadixKeyWithLocFromObj(BSONObj(), RecordId::min(), _identEnd, _ordering);
}

SortedDataInterfaceStandard::SortedDataInterfaceStandard(const Ordering& ordering, StringData ident)
    : SortedDataInterfaceBase(ordering, ident) {
    _KSForIdentStart = createRadixKeyWithLocFromObj(
        BSONObj(), RecordId::min(), ident.toString().append(1, '\0'), _ordering);
    _KSForIdentEnd = createRadixKeyWithLocFromObj(BSONObj(), RecordId::min(), _identEnd, _ordering);
}

Status SortedDataInterfaceStandard::insert(OperationContext* opCtx,
                                           const KeyString::Value& keyString,
                                           bool dupsAllowed) {
    StringStore* workingCopy(RecoveryUnit::get(opCtx)->getHead());
    RecordId loc = KeyString::decodeRecordIdAtEnd(keyString.getBuffer(), keyString.getSize());

    std::string key = createRadixKeyWithLocFromKS(keyString, loc, _prefix);
    bool inserted =
        workingCopy->insert({std::move(key), IndexDataEntry::create(loc, keyString.getTypeBits())})
            .second;
    if (inserted)
        RecoveryUnit::get(opCtx)->makeDirty();
    return Status::OK();
}

void SortedDataInterfaceStandard::unindex(OperationContext* opCtx,
                                          const KeyString::Value& keyString,
                                          bool dupsAllowed) {
    StringStore* workingCopy(RecoveryUnit::get(opCtx)->getHead());
    RecordId loc = KeyString::decodeRecordIdAtEnd(keyString.getBuffer(), keyString.getSize());

    auto key = createRadixKeyWithLocFromKS(keyString, loc, _prefix);
    if (workingCopy->erase(key))
        RecoveryUnit::get(opCtx)->makeDirty();
}

Status SortedDataInterfaceStandard::dupKeyCheck(OperationContext* opCtx,
                                                const KeyString::Value& key) {
    invariant(false);
    return Status::OK();
}

void SortedDataInterfaceStandard::fullValidate(OperationContext* opCtx,
                                               long long* numKeysOut,
                                               ValidateResults* fullResults) const {
    StringStore* workingCopy(RecoveryUnit::get(opCtx)->getHead());
    long long numKeys = 0;
    auto it = workingCopy->lower_bound(_KSForIdentStart);
    while (it != workingCopy->end() && it->first.compare(_KSForIdentEnd) < 0) {
        ++numKeys;
        ++it;
    }
    *numKeysOut = numKeys;
}

std::unique_ptr<mongo::SortedDataInterface::Cursor> SortedDataInterfaceStandard::newCursor(
    OperationContext* opCtx, bool isForward) const {
    StringStore* workingCopy(RecoveryUnit::get(opCtx)->getHead());

    return std::make_unique<CursorStandard>(opCtx,
                                            isForward,
                                            _prefix,
                                            _identEnd,
                                            workingCopy,
                                            _ordering,
                                            _KSForIdentStart,
                                            _KSForIdentEnd);
}

}  // namespace ephemeral_for_test
}  // namespace mongo
