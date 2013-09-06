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

#include "mongo/db/index.h"  // For IndexDetails.
#include "mongo/db/jsobj.h"
#include "mongo/db/namespace_details.h"  // For NamespaceDetails.

namespace mongo {

    /**
     * OnDiskIndexData (aka IndexDetails) is memory-mapped on-disk index data.
     * It contains two DiskLocs:
     * The first points to the head of the index.  This is currently turned into a Btree node.
     * The second points to a BSONObj which describes the index.
     */
    typedef IndexDetails OnDiskIndexData;

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
        IndexDescriptor(NamespaceDetails* namespaceDetails, int indexNumber, OnDiskIndexData* data,
                        BSONObj infoObj)
            : _namespaceDetails(namespaceDetails), _indexNumber(indexNumber), _onDiskData(data),
              _infoObj(infoObj), _numFields(infoObj.getObjectField("key").nFields()) { }

        //
        // Information about the key pattern.
        //

        /**
         * Return the user-provided index key pattern.
         * Example: {geo: "2dsphere", nonGeo: 1}
         * Example: {foo: 1, bar: -1}
         */
        BSONObj keyPattern() const { return _infoObj.getObjectField("key"); }

        // How many fields do we index / are in the key pattern?
        int getNumFields() const { return _numFields; }

        //
        // Information about the index's namespace / collection.
        //

        // Return the name of the index.
        string indexName() const { return _infoObj.getStringField("name"); }

        // Return the name of the indexed collection.
        string parentNS() const { return _infoObj.getStringField("ns"); }

        // Return the name of this index's storage area (database.table.$index)
        string indexNamespace() const {
            string s = parentNS();
            verify(!s.empty());
            s += ".$";
            s += indexName();
            return s;
        }

        //
        // Properties every index has
        //

        // Return what version of index this is.
        int version() const {
            BSONElement e = _infoObj["v"];
            if (NumberInt == e.type()) {
                return e.Int();
            } else {
                return 0;
            }
        }

        // May each key only occur once?
        bool unique() const { return _infoObj["unique"].trueValue(); }

        // Is dropDups set on this index?
        bool dropDups() const { return _infoObj.getBoolField("dropDups"); }

        // Is this index sparse?
        bool isSparse() const { return _infoObj["sparse"].trueValue(); }

        // Is this index multikey?
        bool isMultikey() const { return _namespaceDetails->isMultikey(_indexNumber); }

        //
        // Properties that are Index-specific.
        //

        // Allow access to arbitrary fields in the per-index info object.  Some indices stash
        // index-specific data there.
        BSONElement getInfoElement(const string& name) { return _infoObj[name]; }

        //
        // "Internals" of accessing the index, used by IndexAccessMethod(s).
        //

        // Return the memory-mapped index data block.
        OnDiskIndexData& getOnDisk() { return* _onDiskData; }

        // Return the mutable head of the index.
        DiskLoc& getHead() { return _onDiskData->head; }

        // Return a (rather compact) string representation.
        string toString() { return _infoObj.toString(); }

        // Return the info object.
        BSONObj infoObj() { return _infoObj; }

        // Set multikey attribute.  We never unset it.
        void setMultikey() {
            _namespaceDetails->setIndexIsMultikey(parentNS().c_str(), _indexNumber);
        }

        // Is this index being created in the background?
        bool isBackgroundIndex() {
            return _indexNumber >= _namespaceDetails->getCompletedIndexCount();
        }

    private:
        // Related catalog information.
        NamespaceDetails* _namespaceDetails;

        // What # index are we in the catalog represented by _namespaceDetails?  Needed for setting
        // and getting multikey.
        int _indexNumber;

        OnDiskIndexData* _onDiskData;

        // The BSONObj describing the index.  Accessed through the various members above.
        const BSONObj _infoObj;

        // How many fields are indexed?
        int64_t _numFields;
    };

}  // namespace mongo
