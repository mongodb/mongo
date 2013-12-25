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

#include "mongo/db/storage/index_details.h"  // For IndexDetails.
#include "mongo/db/jsobj.h"
#include "mongo/db/namespace_details.h"  // For NamespaceDetails.
#include "mongo/db/structure/collection.h"

#include "mongo/util/stacktrace.h"

namespace mongo {

    class IndexCatalog;

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
        IndexDescriptor(Collection* collection, int indexNumber,BSONObj infoObj)
            : _magic(123987),
              _collection(collection), _indexNumber(indexNumber),
              _infoObj(infoObj.getOwned()),
              _numFields(infoObj.getObjectField("key").nFields()),
              _keyPattern(infoObj.getObjectField("key").getOwned()),
              _indexName(infoObj.getStringField("name")),
              _parentNS(infoObj.getStringField("ns")),
              _isIdIndex(IndexDetails::isIdIndexPattern( _keyPattern )),
              _sparse(infoObj["sparse"].trueValue()),
              _dropDups(infoObj["dropDups"].trueValue()),
              _unique( _isIdIndex || infoObj["unique"].trueValue() )
        {
            _indexNamespace = _parentNS + ".$" + _indexName;

            _version = 0;
            BSONElement e = _infoObj["v"];
            if ( e.isNumber() ) {
                _version = e.numberInt();
            }
        }

        ~IndexDescriptor() {
            _magic = 555;
        }

        // XXX this is terrible
        IndexDescriptor* clone() const {
            return new IndexDescriptor(_collection, _indexNumber,_infoObj);
        }

        //
        // Information about the key pattern.
        //

        /**
         * Return the user-provided index key pattern.
         * Example: {geo: "2dsphere", nonGeo: 1}
         * Example: {foo: 1, bar: -1}
         */
        const BSONObj& keyPattern() const { _checkOk(); return _keyPattern; }

        // How many fields do we index / are in the key pattern?
        int getNumFields() const { _checkOk(); return _numFields; }

        //
        // Information about the index's namespace / collection.
        //

        // Return the name of the index.
        const string& indexName() const { _checkOk(); return _indexName; }

        // Return the name of the indexed collection.
        const string& parentNS() const { return _parentNS; }

        // Return the name of this index's storage area (database.table.$index)
        const string& indexNamespace() const { return _indexNamespace; }

        //
        // Properties every index has
        //

        // Return what version of index this is.
        int version() const { return _version; }

        // May each key only occur once?
        bool unique() const { return _unique; }

        // Is dropDups set on this index?
        bool dropDups() const { return _dropDups; }

        // Is this index sparse?
        bool isSparse() const { return _sparse; }

        // Is this index multikey?
        bool isMultikey() const { _checkOk(); return _collection->details()->isMultikey(_indexNumber); }

        bool isIdIndex() const { _checkOk(); return _isIdIndex; }

        //
        // Properties that are Index-specific.
        //

        // Allow access to arbitrary fields in the per-index info object.  Some indices stash
        // index-specific data there.
        BSONElement getInfoElement(const string& name) const { return _infoObj[name]; }

        //
        // "Internals" of accessing the index, used by IndexAccessMethod(s).
        //

        // Return a (rather compact) string representation.
        string toString() const { _checkOk(); return _infoObj.toString(); }

        // Return the info object.
        const BSONObj& infoObj() const { _checkOk(); return _infoObj; }

        // Set multikey attribute.  We never unset it.
        void setMultikey() {
            _collection->getIndexCatalog()->markMultikey( this );
        }

        // Is this index being created in the background?
        bool isBackgroundIndex() const {
            return _indexNumber >= _collection->details()->getCompletedIndexCount();
        }

        // this is the collection over which the index is over
        Collection* getIndexedCollection() const { return _collection; }

        // this is the owner of this IndexDescriptor
        IndexCatalog* getIndexCatalog() const { return _collection->getIndexCatalog(); }

    private:

        void _checkOk() const {
            if ( _magic == 123987 )
                return;
            log() << "uh oh: " << (void*)(this) << " " << _magic;
            verify(0);
        }

        int getIndexNumber() const { return _indexNumber; }

        int _magic;

        // Related catalog information of the parent collection
        Collection* _collection;

        // What # index are we in the catalog represented by _namespaceDetails?  Needed for setting
        // and getting multikey.
        int _indexNumber;

        // The BSONObj describing the index.  Accessed through the various members above.
        const BSONObj _infoObj;

        // --- cached data from _infoObj

        int64_t _numFields; // How many fields are indexed?
        BSONObj _keyPattern;
        string _indexName;
        string _parentNS;
        string _indexNamespace;
        bool _isIdIndex;
        bool _sparse;
        bool _dropDups;
        bool _unique;
        int _version;

        friend class IndexCatalog;
    };

}  // namespace mongo
