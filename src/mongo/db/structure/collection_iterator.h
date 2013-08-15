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
 */

#pragma once

#include "mongo/db/exec/collection_scan_common.h"

namespace mongo {

    class CollectionTemp;
    class DiskLoc;
    class NamespaceDetails;

    /**
     * A CollectionIterator provides an interface for walking over a collection.
     * The details of navigating the collection's structure are below this interface.
     */
    class CollectionIterator {
    public:
        virtual ~CollectionIterator() { }

        // True if getNext will produce no more data, false otherwise.
        virtual bool isEOF() = 0;

        // Return the next item from the collection.  Returns DiskLoc() if isEOF.
        virtual DiskLoc getNext() = 0;

        // Can only be called after prepareToYield and before recoverFromYield.
        virtual void invalidate(const DiskLoc& dl) = 0;

        // Save any state required to resume operation (without crashing) after DiskLoc deletion or
        // a collection drop.
        virtual void prepareToYield() = 0;

        // Returns true if collection still exists, false otherwise.
        virtual bool recoverFromYield() = 0;
    };

    /**
     * This class iterates over a non-capped collection identified by 'ns'.
     * The collection must exist when the constructor is called.
     *
     * If start is not DiskLoc(), the iteration begins at that DiskLoc.
     */
    class FlatIterator : public CollectionIterator {
    public:
        FlatIterator(const CollectionTemp* collection, const DiskLoc& start,
                     const CollectionScanParams::Direction& dir);
        virtual ~FlatIterator() { }

        virtual bool isEOF();
        virtual DiskLoc getNext();

        virtual void invalidate(const DiskLoc& dl);
        virtual void prepareToYield();
        virtual bool recoverFromYield();

    private:
        // The result returned on the next call to getNext().
        DiskLoc _curr;

        const CollectionTemp* _collection;

        CollectionScanParams::Direction _direction;
    };

    /**
     * This class iterates over a capped collection identified by 'ns'.
     * The collection must exist when the constructor is called.
     *
     * If start is not DiskLoc(), the iteration begins at that DiskLoc.
     *
     * If tailable is true, getNext() can be called after isEOF.  It will use the last valid
     * returned DiskLoc and try to find the next record from that.
     */
    class CappedIterator : public CollectionIterator {
    public:
        CappedIterator(const string& ns, const DiskLoc& start, bool tailable,
                       const CollectionScanParams::Direction& dir);
        virtual ~CappedIterator() { }

        // If this is a tailable cursor, isEOF could change its mind after a call to getNext().
        virtual bool isEOF();
        virtual DiskLoc getNext();

        virtual void invalidate(const DiskLoc& dl);
        virtual void prepareToYield();
        virtual bool recoverFromYield();

    private:
        /**
         * Internal collection navigation helper methods.
         */
        static DiskLoc getNextCapped(const DiskLoc& dl, CollectionScanParams::Direction direction,
                              NamespaceDetails* nsd);
        static DiskLoc prevLoop(NamespaceDetails* nsd, const DiskLoc& curr);
        static DiskLoc nextLoop(NamespaceDetails* nsd, const DiskLoc& prev);

        // The collection we're iterating over.
        string _ns;

        // Not valid between calls to prepareToYield and recoverFromYield.
        // The actual on-disk catalog information for the collection referred to by '_ns'.
        // We need the extent information in this to navigate capped collections.
        NamespaceDetails* _details;

        // The result returned on the next call to getNext().
        DiskLoc _curr;

        // If we're tailable, we try to progress from the last valid result when we hit the end.
        DiskLoc _prev;
        bool _tailable;

        CollectionScanParams::Direction _direction;

        // If invalidate kills the DiskLoc we need to move forward, we kill the iterator.  See the
        // comment in the body of invalidate(...).
        bool _killedByInvalidate;
    };

}  // namespace mongo
