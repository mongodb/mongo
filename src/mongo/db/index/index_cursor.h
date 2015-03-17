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

#include <memory>
#include <vector>

#include "mongo/base/status.h"
#include "mongo/db/index/index_cursor.h"
#include "mongo/db/index/index_descriptor.h"
#include "mongo/db/jsobj.h"
#include "mongo/db/record_id.h"
#include "mongo/db/storage/sorted_data_interface.h"

namespace mongo {

    struct CursorOptions;

    /**
     * TODO remove this class in favor of direct usage of SortedDataInterface::Cursor.
     *
     * An IndexCursor is the interface through which one traverses the entries of a given
     * index. The internal structure of an index is kept isolated.
     *
     * The cursor must be initialized by seek()ing to a given entry in the index.  The index is
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
        Status seek(const BSONObj& position);

        Status seek(const std::vector<const BSONElement*>& position,
                    const std::vector<bool>& inclusive);

        /**
         * Seek to the key 'position'.  If 'afterKey' is true, seeks to the first
         * key that is oriented after 'position'.
         *
         * Btree-specific.
         */
        void seek(const BSONObj& position, bool afterKey);

        Status skip(const BSONObj& keyBegin,
                    int keyBeginLen,
                    bool afterKey,
                    const std::vector<const BSONElement*>& keyEnd,
                    const std::vector<bool>& keyEndInclusive);

        /**
         * Returns true if 'this' points at the same exact key as 'other'.
         * Returns false otherwise.
         */
        bool pointsAt(const IndexCursor& other);

        //
        // Iteration support
        //

        // Are we out of documents?
        bool isEOF() const;

        // Move to the next key/value pair.  Assumes !isEOF().
        void next();
        
        //
        // Accessors
        //

        // Current key we point at.  Assumes !isEOF().
        BSONObj getKey() const;

        // Current value we point at.  Assumes !isEOF().
        RecordId getValue() const;

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
         * Save our current position in the index.
         */
        Status savePosition();

        /**
         * Restore the saved position.  Errors if there is no saved position.
         * The cursor may be EOF after a restore.
         */
        Status restorePosition(OperationContext* txn);

        /**
         * Return a std::string describing the cursor.
         */
        std::string toString();

    private:
        // We keep the constructor private and only allow the AM to create us.
        friend class IndexAccessMethod;

        /**
         * interface is an abstraction to hide the fact that we have two types of Btrees.
         *
         * Intentionally private, we're friends with the only class allowed to call it.
         */
        IndexCursor(SortedDataInterface::Cursor* cursor);

        bool isSavedPositionValid();

        /**
         * Move to the next (or previous depending on the direction) key.  Used by normal getNext
         * and also skipping unused keys.
         */
        void advance();

        const std::unique_ptr<SortedDataInterface::Cursor> _cursor;
    };

    // Temporary typedef to old name
    using BtreeIndexCursor = IndexCursor;

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
