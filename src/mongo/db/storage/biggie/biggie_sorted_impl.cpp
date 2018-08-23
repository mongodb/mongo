/**
 * Copyright (C) 2018 MongoDB Inc.
 *
 * This program is free software: you can redistribute it and/or  modify
 * it under the terms of the GNU Affero General Public License, version 3,
 * as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Affero General Public License for more details.
 *
 * You should have received a copy of the GNU Affero General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 * As a special exception, the copyright holders give permission to link the
 * code of portions of this program with the OpenSSL library under certain
 * conditions as described in each individual source file and distribute
 * linked combinations including the program with the OpenSSL library. You
 * must comply with the GNU Affero General Public License in all respects
 * for all of the code used other than as permitted herein. If you modify
 * file(s) with this exception, you may extend this exception to your
 * version of the file(s), but you are not obligated to do so. If you do not
 * wish to do so, delete this exception statement from your version. If you
 * delete this exception statement from all source files in the program,
 * then also delete it in the license file.
 */

#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kStorage

#include "mongo/db/storage/biggie/biggie_sorted_impl.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/catalog/index_catalog_entry.h"
#include "mongo/db/storage/biggie/biggie_recovery_unit.h"
#include "mongo/db/storage/biggie/store.h"
#include "mongo/db/storage/index_entry_comparison.h"
#include "mongo/db/storage/key_string.h"
#include "mongo/platform/basic.h"
#include "mongo/stdx/memory.h"
#include "mongo/util/bufreader.h"
#include "mongo/util/hex.h"
#include "mongo/util/log.h"
#include "mongo/util/mongoutils/str.h"
#include "mongo/util/shared_buffer.h"

#include <cstring>
#include <iomanip>
#include <memory>
#include <set>
#include <sstream>
#include <utility>

namespace mongo {
namespace biggie {
namespace {

// This function is the same as the one in record store--basically, using the git analogy, create
// a working branch if one does not exist.
StringStore* getRecoveryUnitBranch_forking(OperationContext* opCtx) {
    RecoveryUnit* biggieRCU = checked_cast<RecoveryUnit*>(opCtx->recoveryUnit());
    invariant(biggieRCU);
    biggieRCU->forkIfNeeded();
    return biggieRCU->getWorkingCopy();
}

// This just checks to see if the field names are empty or not.
bool hasFieldNames(const BSONObj& obj) {
    BSONForEach(e, obj) {
        if (e.fieldName()[0])
            return true;
    }
    return false;
}

// This just makes all the fields in a BSON object equal to "".
BSONObj stripFieldNames(const BSONObj& obj) {
    BSONObjIterator it(obj);
    BSONObjBuilder bob;
    while (it.more()) {
        bob.appendAs(it.next(), "");
    }
    return bob.obj();
}

Status dupKeyError(const BSONObj& key) {
    StringBuilder sb;
    sb << "E11000 duplicate key error ";
    sb << "dup key: " << key;
    return Status(ErrorCodes::DuplicateKey, sb.str());
}

// This function converts a key and an ordering to a KeyString.
std::unique_ptr<KeyString> keyToKeyString(const BSONObj& key, Ordering order) {
    KeyString::Version version = KeyString::Version::V1;
    std::unique_ptr<KeyString> retKs = std::make_unique<KeyString>(version, key, order);
    return retKs;
}

// This combines a key, a record ID, and the ident into a single KeyString. Because we cannot
// compare keys properly (since they require an ordering, because we may have descending keys
// or multi-field keys), we need to convert them into a KeyString first, and then we can just
// compare them. Thus, we use a nested KeyString of keys inside our actual KeyString. The only
// difference between this function and the one below is that this one calls resetToKey first.
std::string combineKeyAndRIDWithReset(const BSONObj& key,
                                      const RecordId& loc,
                                      std::string prefixToUse,
                                      Ordering order) {
    KeyString::Version version = KeyString::Version::V1;
    std::unique_ptr<KeyString> ks = std::make_unique<KeyString>(version);
    ks->resetToKey(key, order);

    BSONObjBuilder b;
    b.append("", prefixToUse);                                  // prefix
    b.append("", std::string(ks->getBuffer(), ks->getSize()));  // key

    Ordering allAscending = Ordering::make(BSONObj());
    std::unique_ptr<KeyString> retKs =
        std::make_unique<KeyString>(version, b.obj(), allAscending, loc);
    return std::string(retKs->getBuffer(), retKs->getSize());
}

std::unique_ptr<KeyString> combineKeyAndRIDKS(const BSONObj& key,
                                              const RecordId& loc,
                                              std::string prefixToUse,
                                              Ordering order) {
    KeyString::Version version = KeyString::Version::V1;
    KeyString ks(version, key, order);
    BSONObjBuilder b;
    b.append("", prefixToUse);                                // prefix
    b.append("", std::string(ks.getBuffer(), ks.getSize()));  // key
    Ordering allAscending = Ordering::make(BSONObj());
    return std::make_unique<KeyString>(version, b.obj(), allAscending, loc);
}

// This is similar to the function above, but it returns a string instead of a KeyString. The
// reason we need both is that we also need to store the typebits, and therefore, we occasionally
// need to return the KeyString (in the function above). However, most of the time the string
// representation of the KeyString is good enough, and therefore we just return the string (this
// function).
std::string combineKeyAndRID(const BSONObj& key,
                             const RecordId& loc,
                             std::string prefixToUse,
                             Ordering order) {
    KeyString::Version version = KeyString::Version::V1;
    KeyString ks(version, key, order);

    BSONObjBuilder b;
    b.append("", prefixToUse);                                // prefix
    b.append("", std::string(ks.getBuffer(), ks.getSize()));  // key
    Ordering allAscending = Ordering::make(BSONObj());
    std::unique_ptr<KeyString> retKs =
        std::make_unique<KeyString>(version, b.obj(), allAscending, loc);
    return std::string(retKs->getBuffer(), retKs->getSize());
}

// This function converts a KeyString into an IndexKeyEntry. We don't need to store the typebits
// for the outer key string (the one consisting of the prefix, the key, and the recordId) since
// those will not be used. However, we do need to store the typebits for the internal keystring
// (made from the key itself), as those typebits are potentially important.
IndexKeyEntry keyStringToIndexKeyEntry(std::string keyString,
                                       std::string typeBitsString,
                                       Ordering order) {
    KeyString::Version version = KeyString::Version::V1;
    KeyString::TypeBits tbInternal = KeyString::TypeBits(version);
    KeyString::TypeBits tbOuter = KeyString::TypeBits(version);

    BufReader brTbInternal(typeBitsString.c_str(), typeBitsString.length());
    tbInternal.resetFromBuffer(&brTbInternal);

    Ordering allAscending = Ordering::make(BSONObj());

    BSONObj bsonObj =
        KeyString::toBsonSafe(keyString.c_str(), keyString.length(), allAscending, tbOuter);

    // First we get the BSONObj key.
    SharedBuffer sb;
    int counter = 0;
    for (auto&& elem : bsonObj) {
        // The key is the second field.
        if (counter == 1) {
            const char* valStart = elem.valuestr();
            int valSize = elem.valuestrsize();
            KeyString ks(version);
            ks.resetFromBuffer(valStart, valSize);

            BSONObj originalKey =
                KeyString::toBsonSafe(ks.getBuffer(), ks.getSize(), order, tbInternal);

            sb = SharedBuffer::allocate(originalKey.objsize());
            std::memcpy(sb.get(), originalKey.objdata(), originalKey.objsize());
            break;
        }
        counter++;
    }
    RecordId rid = KeyString::decodeRecordIdAtEnd(keyString.c_str(), keyString.length());
    ConstSharedBuffer csb(sb);
    BSONObj key(csb);

    return IndexKeyEntry(key, rid);
}

int compareTwoKeys(
    std::string ks1, std::string tbs1, std::string ks2, std::string tbs2, Ordering order) {
    size_t size1 = KeyString::sizeWithoutRecordIdAtEnd(ks1.c_str(), ks1.length());
    size_t size2 = KeyString::sizeWithoutRecordIdAtEnd(ks2.c_str(), ks2.length());
    auto cmpSmallerMemory = std::memcmp(ks1.c_str(), ks2.c_str(), std::min(size1, size2));

    if (cmpSmallerMemory != 0) {
        return cmpSmallerMemory;
    }
    if (size1 == size2) {
        return 0;
    }
    return (size1 > size2);
}

}  // namepsace

SortedDataBuilderInterface::SortedDataBuilderInterface(OperationContext* opCtx,
                                                       bool dupsAllowed,
                                                       Ordering order,
                                                       std::string prefix,
                                                       std::string identEnd)
    : _opCtx(opCtx),
      _dupsAllowed(dupsAllowed),
      _order(order),
      _prefix(prefix),
      _identEnd(identEnd),
      _hasLast(false),
      _lastKeyToString(""),
      _lastRID(-1) {}

SpecialFormatInserted SortedDataBuilderInterface::commit(bool mayInterrupt) {
    biggie::RecoveryUnit* ru = checked_cast<biggie::RecoveryUnit*>(_opCtx->recoveryUnit());
    ru->forkIfNeeded();
    ru->commitUnitOfWork();
    return SpecialFormatInserted::NoSpecialFormatInserted;
}

StatusWith<SpecialFormatInserted> SortedDataBuilderInterface::addKey(const BSONObj& key,
                                                                     const RecordId& loc) {
    StringStore* workingCopy = getRecoveryUnitBranch_forking(_opCtx);

    invariant(loc.isNormal());
    invariant(!hasFieldNames(key));

    std::unique_ptr<KeyString> newKS = keyToKeyString(key, _order);
    std::string newKSToString = std::string(newKS->getBuffer(), newKS->getSize());

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
    if (!_dupsAllowed && twoKeyCmp == 0 && twoRIDCmp != 0) {
        return dupKeyError(key);
    }

    std::string workingCopyInsertKey = combineKeyAndRID(key, loc, _prefix, _order);
    std::unique_ptr<KeyString> workingCopyInternalKs = keyToKeyString(key, _order);
    std::unique_ptr<KeyString> workingCopyOuterKs = combineKeyAndRIDKS(key, loc, _prefix, _order);

    std::string internalTbString(
        reinterpret_cast<const char*>(workingCopyInternalKs->getTypeBits().getBuffer()),
        workingCopyInternalKs->getTypeBits().getSize());

    workingCopy->insert(StringStore::value_type(workingCopyInsertKey, internalTbString));

    _hasLast = true;
    _lastKeyToString = newKSToString;
    _lastRID = loc.repr();

    return StatusWith<SpecialFormatInserted>(SpecialFormatInserted::NoSpecialFormatInserted);
}

SortedDataBuilderInterface* SortedDataInterface::getBulkBuilder(OperationContext* opCtx,
                                                                bool dupsAllowed) {
    return new SortedDataBuilderInterface(opCtx, dupsAllowed, _order, _prefix, _identEnd);
}

// We append \1 to all idents we get, and therefore the KeyString with ident + \0 will only be
// before elements in this ident, and the KeyString with ident + \2 will only be after elements in
// this ident.
SortedDataInterface::SortedDataInterface(const Ordering& ordering, bool isUnique, StringData ident)
    : _order(ordering),
      // All entries in this ident will have a prefix of ident + \1.
      _prefix(ident.toString().append(1, '\1')),
      // Therefore, the string ident + \2 will be greater than all elements in this ident.
      _identEnd(ident.toString().append(1, '\2')),
      _isUnique(isUnique) {
    // This is the string representation of the KeyString before elements in this ident, which is
    // ident + \0. This is before all elements in this ident.
    _KSForIdentStart =
        combineKeyAndRID(BSONObj(), RecordId::min(), ident.toString().append(1, '\0'), ordering);
    // Similarly, this is the string representation of the KeyString for something greater than
    // all other elements in this ident.
    _KSForIdentEnd = combineKeyAndRID(BSONObj(), RecordId::min(), _identEnd, ordering);
}

StatusWith<SpecialFormatInserted> SortedDataInterface::insert(OperationContext* opCtx,
                                                              const BSONObj& key,
                                                              const RecordId& loc,
                                                              bool dupsAllowed) {
    // The KeyString representation of the key.
    std::unique_ptr<KeyString> workingCopyInternalKs = keyToKeyString(key, _order);
    // The KeyString of prefix (which is ident + \1), key, loc.
    std::unique_ptr<KeyString> workingCopyOuterKs = combineKeyAndRIDKS(key, loc, _prefix, _order);
    // The string representation.
    std::string workingCopyInsertKey = combineKeyAndRID(key, loc, _prefix, _order);

    StringStore* workingCopy = getRecoveryUnitBranch_forking(opCtx);

    if (workingCopy->find(workingCopyInsertKey) != workingCopy->end()) {
        return StatusWith<SpecialFormatInserted>(SpecialFormatInserted::NoSpecialFormatInserted);
    }

    // If dups are not allowed, then we need to check that we are not inserting something with an
    // existing key but a different recordId. However, if the combination of key, recordId already
    // exists, then we are fine, since we are allowed to insert duplicates.
    if (!dupsAllowed) {
        std::string workingCopyLowerBound = combineKeyAndRID(key, RecordId::min(), _prefix, _order);
        std::string workingCopyUpperBound = combineKeyAndRID(key, RecordId::max(), _prefix, _order);
        StringStore::const_iterator lowerBoundIterator =
            workingCopy->lower_bound(workingCopyLowerBound);

        if (lowerBoundIterator != workingCopy->end() &&
            lowerBoundIterator->first.compare(_KSForIdentEnd) < 0) {
            IndexKeyEntry ike = keyStringToIndexKeyEntry(
                lowerBoundIterator->first, lowerBoundIterator->second, _order);
            // We need a KeyString comparison here since even if the types are different, if the
            // values are the same, then we need to still return equal.
            auto ks1 = keyToKeyString(ike.key, _order);
            auto ks2 = keyToKeyString(key, _order);
            if (ks1->compare(*ks2) == 0 && ike.loc.repr() != loc.repr()) {
                return dupKeyError(key);
            }
        }
    }

    // The key we insert is the workingCopyOuterKs as described above. The value is the typebits
    // for the internal keystring (created from the key/order), which we will use when decoding the
    // key and creating an IndexKeyEntry.
    std::string internalTbString =
        std::string(reinterpret_cast<const char*>(workingCopyInternalKs->getTypeBits().getBuffer()),
                    workingCopyInternalKs->getTypeBits().getSize());
    workingCopy->insert(StringStore::value_type(workingCopyInsertKey, internalTbString));
    return StatusWith<SpecialFormatInserted>(SpecialFormatInserted::NoSpecialFormatInserted);
}

void SortedDataInterface::unindex(OperationContext* opCtx,
                                  const BSONObj& key,
                                  const RecordId& loc,
                                  bool dupsAllowed) {
    std::string workingCopyInsertKey = combineKeyAndRID(key, loc, _prefix, _order);
    StringStore* workingCopy = getRecoveryUnitBranch_forking(opCtx);
    workingCopy->erase(workingCopyInsertKey);
}

// This function is, as of now, not in the interface, but there exists a server ticket to add
// truncate to the list of commands able to be used.
Status SortedDataInterface::truncate(OperationContext* opCtx) {
    StringStore* workingCopy = getRecoveryUnitBranch_forking(opCtx);
    auto workingCopyLowerBound = workingCopy->lower_bound(_KSForIdentStart);
    auto workingCopyUpperBound = workingCopy->upper_bound(_KSForIdentEnd);
    // workingCopy->erase(workingCopyLowerBound, workingCopyUpperBound);
    while (workingCopyLowerBound != workingCopyUpperBound) {
        workingCopy->erase(workingCopyLowerBound->first);
        ++workingCopyLowerBound;
    }
    return Status::OK();
}

Status SortedDataInterface::dupKeyCheck(OperationContext* opCtx,
                                        const BSONObj& key,
                                        const RecordId& loc) {
    std::string workingCopyCheckKey = combineKeyAndRID(key, loc, _prefix, _order);
    StringStore* workingCopy = getRecoveryUnitBranch_forking(opCtx);

    // We effectively do the same check as in insert. However, we also check to make sure that
    // the iterator returned to us by lower_bound also happens to be inside out ident.
    if (workingCopy->find(workingCopyCheckKey) != workingCopy->end()) {
        return Status::OK();
    }

    std::string workingCopyLowerBound = combineKeyAndRID(key, RecordId::min(), _prefix, _order);
    auto lowerBoundIterator = workingCopy->lower_bound(workingCopyLowerBound);

    if (lowerBoundIterator != workingCopy->end() &&
        lowerBoundIterator->first != workingCopyCheckKey &&
        lowerBoundIterator->first.compare(_KSForIdentEnd) < 0 &&
        lowerBoundIterator->first.compare(
            combineKeyAndRID(key, RecordId::max(), _prefix, _order)) <= 0) {
        return dupKeyError(key);
    }
    return Status::OK();
}

void SortedDataInterface::fullValidate(OperationContext* opCtx,
                                       long long* numKeysOut,
                                       ValidateResults* fullResults) const {
    StringStore* workingCopy = getRecoveryUnitBranch_forking(opCtx);
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
    StringStore* str = getRecoveryUnitBranch_forking(opCtx);
    size_t totalSize = 0;
    StringStore::const_iterator it = str->lower_bound(_KSForIdentStart);
    StringStore::const_iterator end = str->upper_bound(_KSForIdentEnd);
    int64_t numElements = str->distance(it, end);
    for (int i = 0; i < numElements; i++) {
        totalSize += it->first.length();
        ++it;
    }
    return (long long)totalSize;
}

bool SortedDataInterface::isEmpty(OperationContext* opCtx) {
    StringStore* workingCopy = getRecoveryUnitBranch_forking(opCtx);
    return workingCopy->distance(workingCopy->lower_bound(_KSForIdentStart),
                                 workingCopy->upper_bound(_KSForIdentEnd)) == 0;
}

std::unique_ptr<mongo::SortedDataInterface::Cursor> SortedDataInterface::newCursor(
    OperationContext* opCtx, bool isForward) const {
    StringStore* workingCopy = getRecoveryUnitBranch_forking(opCtx);

    return std::make_unique<SortedDataInterface::Cursor>(opCtx,
                                                         isForward,
                                                         _prefix,
                                                         _identEnd,
                                                         workingCopy,
                                                         _order,
                                                         _isUnique,
                                                         _KSForIdentStart,
                                                         _KSForIdentEnd);
}

Status SortedDataInterface::initAsEmpty(OperationContext* opCtx) {
    return Status::OK();
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
            return _endPos.get() == _workingCopy->end() ||
                _forwardIt->first.compare(_endPos.get()->first) < 0;
        }
        return _forwardIt->first.compare(_KSForIdentEnd) <= 0;
    } else {
        // This is a reverse cursor
        if (_reverseIt == _workingCopy->rend()) {
            return false;
        }
        if (endPosSet()) {
            return _endPosReverse.get() == _workingCopy->rend() ||
                _reverseIt->first.compare(_endPosReverse.get()->first) > 0;
        }
        return _reverseIt->first.compare(_KSForIdentStart) >= 0;
    }
}

void SortedDataInterface::Cursor::setEndPosition(const BSONObj& key, bool inclusive) {
    auto finalKey = stripFieldNames(key);
    StringStore* workingCopy = getRecoveryUnitBranch_forking(_opCtx);
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
        _endPosBound = combineKeyAndRID(finalKey, RecordId::max(), _prefix, _order);
    } else {
        _endPosBound = combineKeyAndRID(finalKey, RecordId::min(), _prefix, _order);
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
        workingCopyBound = combineKeyAndRID(finalKey, RecordId::min(), _prefix, _order);
    } else {
        workingCopyBound = combineKeyAndRID(finalKey, RecordId::max(), _prefix, _order);
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
            _forwardIt = _workingCopy->lower_bound(workingCopyBound);
        } else {
            // Reverse iterators work with upper bound since upper bound will return the first
            // element past the argument, so when it becomes a reverse iterator, it goes
            // backwards one, (according to the C++ standard) and we end up in the right place.
            _reverseIt =
                StringStore::const_reverse_iterator(_workingCopy->upper_bound(workingCopyBound));
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
    BSONObj finalKey = stripFieldNames(key);
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
    StringStore* workingCopy = getRecoveryUnitBranch_forking(_opCtx);

    this->_workingCopy = workingCopy;

    // Here, we have to reset the end position if one was set earlier.
    if (endPosSet()) {
        setEndPosition(_endPosKey.get(), _endPosIncl);
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
            // Unique indices cannot return the same key twice. Therefore, if we would normally not
            // advance on the next call to next() by setting _lastMoveWasRestore, we potentially
            // won't set it if that would cause us to return the same value twice.
            int twoKeyCmp = compareTwoKeys(
                _forwardIt->first, _forwardIt->second, _saveKey, _forwardIt->second, _order);
            _lastMoveWasRestore = (twoKeyCmp != 0);
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
            int twoKeyCmp = compareTwoKeys(
                _reverseIt->first, _reverseIt->second, _saveKey, _reverseIt->second, _order);
            _lastMoveWasRestore = (twoKeyCmp != 0);
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
