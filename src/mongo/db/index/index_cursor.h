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

#include <vector>
#include "mongo/db/diskloc.h"
#include "mongo/db/jsobj.h"

namespace mongo {

    struct CursorOptions;

    /**
     * An IndexCursor is the interface through which one traverses the entries of a given
     * index. The internal structure of an index is kept isolated.
     *
     * The cursor must be initiailized by seek()ing to a given entry in the index.  The index is
     * traversed by calling next() or skip()-ping ahead.
     *
     * The set of predicates a given index can understand is known a priori.  These predicates may
     * be simple (a key location for a Btree index) or rich ($within for a geo index).
     *
     * Locking is the responsibility of the caller.  The IndexCursor keeps state.  If the caller
     * wishes to yield or unlock, it must call savePosition() first.  When it decides to unyield it
     * must call restorePosition().  The cursor may be EOF after a restorePosition().
     */
    class IndexCursor {
    public:
        virtual ~IndexCursor() { }

        // XXX SHORT TERM HACKS THAT MUST DIE: 2d index
        virtual DiskLoc getBucket() const { return DiskLoc(); }

        // XXX SHORT TERM HACKS THAT MUST DIE: 2d index
        virtual int getKeyOfs() const { return 0; }

        /**
         * Set options on the cursor (direction).  See CursorOptions below.
         */
        virtual Status setOptions(const CursorOptions& options) = 0;

        /**
         * A cursor doesn't point anywhere by default.  You must seek to the start position.
         * The provided position must be a predicate that the index understands.  The
         * predicate must describe one value, though there may be several instances
         *
         * Possible return values:
         * 1. Success: seeked to the position.
         * 2. Success: seeked to 'closest' key oriented according to the cursor's direction.
         * 3. Error: can't seek to the position.
         */
        virtual Status seek(const BSONObj& position) = 0;

        //
        // Iteration support
        //

        // Are we out of documents?
        virtual bool isEOF() const = 0;

        // Move to the next key/value pair.  Assumes !isEOF().
        virtual void next() = 0;
        
        //
        // Accessors
        //

        // Current key we point at.  Assumes !isEOF().
        virtual BSONObj getKey() const = 0;

        // Current value we point at.  Assumes !isEOF().
        virtual DiskLoc getValue() const = 0;

        //
        // Yielding support
        //

        /**
         * Yielding semantics:
         * If the entry that a cursor points at is not deleted during a yield, the cursor will
         * point at that entry after a restore.
         * An entry inserted during a yield may or may not be returned by an in-progress scan.
         * An entry deleted during a yield may or may not be returned by an in-progress scan.
         * An entry modified during a yield may or may not be returned by an in-progress scan.
         * An entry that is not inserted or deleted during a yield will be returned, and only once.
         * If the index returns entries in a given order (Btree), this order will be mantained even
         * if the entry corresponding to a saved position is deleted during a yield.
         */

        /**
         * Save our current position in the index.  Assumes that we are currently pointing to a
         * valid position in the index.
         * If not, we error.  Otherwise, succeed.
         */
        virtual Status savePosition() = 0;

        /**
         * Restore the saved position.  Errors if there is no saved position.
         * The cursor may be EOF after a restore.
         */
        virtual Status restorePosition() = 0;

        // Return a string describing the cursor.
        virtual string toString() = 0;

        /**
         *  Add debugging info to the provided builder.
         * TODO(hk/alerner): We can do this better, perhaps with a more structured format.
         */
        virtual void explainDetails(BSONObjBuilder* b) { }
    };

    // All the options we might want to set on a cursor.
    struct CursorOptions {
        // Set the direction of the scan.  Ignored if the cursor doesn't have directions (geo).
        enum Direction {
            DECREASING = -1,
            INCREASING = 1,
        };

        Direction direction;

        // 2d indices need to know exactly how many results you want beforehand.
        // Ignored by every other index.
        int numWanted;
    };

}  // namespace mongo
