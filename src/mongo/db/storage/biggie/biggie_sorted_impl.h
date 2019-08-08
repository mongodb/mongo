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

#include "mongo/db/index/index_descriptor_fwd.h"
#include "mongo/db/storage/biggie/store.h"
#include "mongo/db/storage/key_string.h"
#include "mongo/db/storage/sorted_data_interface.h"

namespace mongo {
namespace biggie {

class SortedDataBuilderInterface : public ::mongo::SortedDataBuilderInterface {
public:
    SortedDataBuilderInterface(OperationContext* opCtx,
                               bool unique,
                               bool dupsAllowed,
                               Ordering order,
                               const std::string& prefix,
                               const std::string& identEnd,
                               const NamespaceString& collectionNamespace,
                               const std::string& indexName,
                               const BSONObj& keyPattern);
    void commit(bool mayInterrupt) override;
    virtual Status addKey(const BSONObj& key, const RecordId& loc);
    virtual Status addKey(const KeyString::Value& keyString, const RecordId& loc);

private:
    OperationContext* _opCtx;
    bool _unique;
    bool _dupsAllowed;
    // Order of the keys.
    Ordering _order;
    // Prefix and identEnd for the ident.
    std::string _prefix;
    std::string _identEnd;
    // Index metadata.
    const NamespaceString _collectionNamespace;
    const std::string _indexName;
    const BSONObj _keyPattern;
    // Whether or not we've already added something before.
    bool _hasLast;
    // This is the KeyString of the last key added.
    std::string _lastKeyToString;
    // This is the last recordId added.
    int64_t _lastRID;
};

class SortedDataInterface : public ::mongo::SortedDataInterface {
public:
    // Truncate is not required at the time of writing but will be when the truncate command is
    // created
    Status truncate(OperationContext* opCtx);
    SortedDataInterface(OperationContext* opCtx, StringData ident, const IndexDescriptor* desc);
    SortedDataInterface(const Ordering& ordering, bool isUnique, StringData ident);
    virtual SortedDataBuilderInterface* getBulkBuilder(OperationContext* opCtx,
                                                       bool dupsAllowed) override;
    virtual Status insert(OperationContext* opCtx,
                          const BSONObj& key,
                          const RecordId& loc,
                          bool dupsAllowed) override;
    virtual Status insert(OperationContext* opCtx,
                          const KeyString::Value& keyString,
                          const RecordId& loc,
                          bool dupsAllowed) override;
    virtual void unindex(OperationContext* opCtx,
                         const BSONObj& key,
                         const RecordId& loc,
                         bool dupsAllowed) override;
    virtual void unindex(OperationContext* opCtx,
                         const KeyString::Value& keyString,
                         const RecordId& loc,
                         bool dupsAllowed) override;
    virtual Status dupKeyCheck(OperationContext* opCtx, const BSONObj& key) override;
    virtual void fullValidate(OperationContext* opCtx,
                              long long* numKeysOut,
                              ValidateResults* fullResults) const override;
    virtual bool appendCustomStats(OperationContext* opCtx,
                                   BSONObjBuilder* output,
                                   double scale) const override;
    virtual long long getSpaceUsedBytes(OperationContext* opCtx) const override;
    virtual bool isEmpty(OperationContext* opCtx) override;
    virtual std::unique_ptr<mongo::SortedDataInterface::Cursor> newCursor(
        OperationContext* opCtx, bool isForward = true) const override;
    virtual Status initAsEmpty(OperationContext* opCtx) override;

    /*
     * This is the cursor class required by the sorted data interface.
     */
    class Cursor final : public ::mongo::SortedDataInterface::Cursor {
    public:
        // All the following public functions just implement the interface.
        Cursor(OperationContext* opCtx,
               bool isForward,
               // This is the ident.
               std::string _prefix,
               // This is a string immediately after the ident and before other idents.
               std::string _identEnd,
               StringStore* workingCopy,
               Ordering order,
               bool isUnique,
               std::string prefixBSON,
               std::string KSForIdentEnd);
        virtual void setEndPosition(const BSONObj& key, bool inclusive) override;
        virtual boost::optional<IndexKeyEntry> next(RequestedInfo parts = kKeyAndLoc) override;
        virtual boost::optional<IndexKeyEntry> seek(const BSONObj& key,
                                                    bool inclusive,
                                                    RequestedInfo parts = kKeyAndLoc) override;
        virtual boost::optional<IndexKeyEntry> seek(const IndexSeekPoint& seekPoint,
                                                    RequestedInfo parts = kKeyAndLoc) override;
        virtual void save() override;
        virtual void restore() override;
        virtual void detachFromOperationContext() override;
        virtual void reattachToOperationContext(OperationContext* opCtx) override;

    private:
        // This is a helper function to check if the cursor was explicitly set by the user or not.
        bool endPosSet();
        // This is a helper function to check if the cursor is valid or not.
        bool checkCursorValid();
        // This is a helper function for seek.
        boost::optional<IndexKeyEntry> seekAfterProcessing(BSONObj finalKey, bool inclusive);
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
        // This stores whether or not the index is unique.
        bool _isUnique;
        // The next two are the same as above.
        std::string _KSForIdentStart;
        std::string _KSForIdentEnd;
    };

private:
    /**
     * Returns false only when the index is partial and the IndexKeyEntry's record id does not match
     * the provided rid from the given key.
     *
     * Returns true in all other cases.
     */
    bool ifPartialCheckRecordIdEquals(OperationContext* opCtx,
                                      const std::string key,
                                      const RecordId rid) const;

    bool keyExists(OperationContext* opCtx, const BSONObj& key);

    // These two are the same as before.
    std::string _prefix;
    std::string _identEnd;
    // Index metadata.
    const NamespaceString _collectionNamespace;
    const std::string _indexName;
    const BSONObj _keyPattern;
    // These are the keystring representations of the _prefix and the _identEnd.
    std::string _KSForIdentStart;
    std::string _KSForIdentEnd;
    // This stores whether or not the end position is inclusive.
    bool _isUnique;
    // Whether or not the index is partial
    bool _isPartial;
};
}  // namespace biggie
}  // namespace mongo
