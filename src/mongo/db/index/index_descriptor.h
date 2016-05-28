// index_descriptor.cpp

/**
*    Copyright (C) 2013 10gen Inc.
*
*    This program is free software: you can redistribute it and/or  modify
*    it under the terms of the GNU Affero General Public License, version 3,
*    as published by the Free Software Foundation.
*
*    This program is distributed in the hope that it will be useful,
*    but WITHOUT ANY WARRANTY; without even the implied warranty of
*    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
*    GNU Affero General Public License for more details.
*
*    You should have received a copy of the GNU Affero General Public License
*    along with this program.  If not, see <http://www.gnu.org/licenses/>.
*
*    As a special exception, the copyright holders give permission to link the
*    code of portions of this program with the OpenSSL library under certain
*    conditions as described in each individual source file and distribute
*    linked combinations including the program with the OpenSSL library. You
*    must comply with the GNU Affero General Public License in all respects for
*    all of the code used other than as permitted herein. If you modify file(s)
*    with this exception, you may extend this exception to your version of the
*    file(s), but you are not obligated to do so. If you do not wish to do so,
*    delete this exception statement from your version. If you delete this
*    exception statement from all source files in the program, then also delete
*    it in the license file.
*/

#pragma once

#include <string>

#include "mongo/db/catalog/collection.h"
#include "mongo/db/index/multikey_paths.h"
#include "mongo/db/jsobj.h"

#include "mongo/util/stacktrace.h"

namespace mongo {

class IndexCatalog;
class IndexCatalogEntry;
class IndexCatalogEntryContainer;

/**
 * A cache of information computed from the memory-mapped per-index data (OnDiskIndexData).
 * Contains accessors for the various immutable index parameters, and an accessor for the
 * mutable "head" pointer which is index-specific.
 *
 * All synchronization is the responsibility of the caller.
 */
class IndexDescriptor {
public:
    /**
     * OnDiskIndexData is a pointer to the memory mapped per-index data.
     * infoObj is a copy of the index-describing BSONObj contained in the OnDiskIndexData.
     */
    IndexDescriptor(Collection* collection, const std::string& accessMethodName, BSONObj infoObj)
        : _magic(123987),
          _collection(collection),
          _accessMethodName(accessMethodName),
          _infoObj(infoObj.getOwned()),
          _numFields(infoObj.getObjectField("key").nFields()),
          _keyPattern(infoObj.getObjectField("key").getOwned()),
          _indexName(infoObj.getStringField("name")),
          _parentNS(infoObj.getStringField("ns")),
          _isIdIndex(isIdIndexPattern(_keyPattern)),
          _sparse(infoObj["sparse"].trueValue()),
          _unique(_isIdIndex || infoObj["unique"].trueValue()),
          _partial(!infoObj["partialFilterExpression"].eoo()),
          _cachedEntry(NULL) {
        _indexNamespace = makeIndexNamespace(_parentNS, _indexName);

        _version = 0;
        BSONElement e = _infoObj["v"];
        if (e.isNumber()) {
            _version = e.numberInt();
        }
    }

    ~IndexDescriptor() {
        _magic = 555;
    }

    //
    // Information about the key pattern.
    //

    /**
     * Return the user-provided index key pattern.
     * Example: {geo: "2dsphere", nonGeo: 1}
     * Example: {foo: 1, bar: -1}
     */
    const BSONObj& keyPattern() const {
        _checkOk();
        return _keyPattern;
    }

    /**
     * Test only command for testing behavior resulting from an incorrect key
     * pattern.
     */
    void setKeyPatternForTest(BSONObj newKeyPattern) {
        _keyPattern = newKeyPattern;
    }

    // How many fields do we index / are in the key pattern?
    int getNumFields() const {
        _checkOk();
        return _numFields;
    }

    //
    // Information about the index's namespace / collection.
    //

    // Return the name of the index.
    const std::string& indexName() const {
        _checkOk();
        return _indexName;
    }

    // Return the name of the indexed collection.
    const std::string& parentNS() const {
        return _parentNS;
    }

    // Return the name of this index's storage area (database.table.$index)
    const std::string& indexNamespace() const {
        return _indexNamespace;
    }

    // Return the name of the access method we must use to access this index's data.
    const std::string& getAccessMethodName() const {
        return _accessMethodName;
    }

    //
    // Properties every index has
    //

    // Return what version of index this is.
    int version() const {
        return _version;
    }

    // May each key only occur once?
    bool unique() const {
        return _unique;
    }

    // Is this index sparse?
    bool isSparse() const {
        return _sparse;
    }

    // Is this a partial index?
    bool isPartial() const {
        return _partial;
    }

    // Is this index multikey?
    bool isMultikey(OperationContext* opCtx) const {
        _checkOk();
        return _collection->getIndexCatalog()->isMultikey(opCtx, this);
    }

    MultikeyPaths getMultikeyPaths(OperationContext* opCtx) const {
        _checkOk();
        return _collection->getIndexCatalog()->getMultikeyPaths(opCtx, this);
    }

    bool isIdIndex() const {
        _checkOk();
        return _isIdIndex;
    }

    //
    // Properties that are Index-specific.
    //

    // Allow access to arbitrary fields in the per-index info object.  Some indices stash
    // index-specific data there.
    BSONElement getInfoElement(const std::string& name) const {
        return _infoObj[name];
    }

    //
    // "Internals" of accessing the index, used by IndexAccessMethod(s).
    //

    // Return a (rather compact) std::string representation.
    std::string toString() const {
        _checkOk();
        return _infoObj.toString();
    }

    // Return the info object.
    const BSONObj& infoObj() const {
        _checkOk();
        return _infoObj;
    }

    // Both the collection and the catalog must outlive the IndexDescriptor
    const Collection* getCollection() const {
        return _collection;
    }
    const IndexCatalog* getIndexCatalog() const {
        return _collection->getIndexCatalog();
    }

    bool areIndexOptionsEquivalent(const IndexDescriptor* other) const;

    static bool isIdIndexPattern(const BSONObj& pattern) {
        BSONObjIterator i(pattern);
        BSONElement e = i.next();
        //_id index must have form exactly {_id : 1} or {_id : -1}.
        // Allows an index of form {_id : "hashed"} to exist but
        // do not consider it to be the primary _id index
        if (!(strcmp(e.fieldName(), "_id") == 0 && (e.numberInt() == 1 || e.numberInt() == -1)))
            return false;
        return i.next().eoo();
    }

    static std::string makeIndexNamespace(StringData ns, StringData name) {
        return ns.toString() + ".$" + name.toString();
    }

private:
    void _checkOk() const;

    int _magic;

    // Related catalog information of the parent collection
    Collection* _collection;

    // What access method should we use for this index?
    std::string _accessMethodName;

    // The BSONObj describing the index.  Accessed through the various members above.
    const BSONObj _infoObj;

    // --- cached data from _infoObj

    int64_t _numFields;  // How many fields are indexed?
    BSONObj _keyPattern;
    std::string _indexName;
    std::string _parentNS;
    std::string _indexNamespace;
    bool _isIdIndex;
    bool _sparse;
    bool _unique;
    bool _partial;
    int _version;

    // only used by IndexCatalogEntryContainer to do caching for perf
    // users not allowed to touch, and not part of API
    IndexCatalogEntry* _cachedEntry;

    friend class IndexCatalog;
    friend class IndexCatalogEntry;
    friend class IndexCatalogEntryContainer;
};

}  // namespace mongo
