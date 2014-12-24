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

#include <boost/scoped_ptr.hpp>
#include <vector>

#include "mongo/base/status.h"
#include "mongo/db/index/index_cursor.h"
#include "mongo/db/index/index_descriptor.h"
#include "mongo/db/jsobj.h"
#include "mongo/db/record_id.h"
#include "mongo/db/storage/sorted_data_interface.h"

namespace mongo {

    class BtreeIndexCursor : public IndexCursor {
    public:
        bool isEOF() const;

        virtual Status seek(const BSONObj& position);

        // Btree-specific seeking functions.
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

        virtual BSONObj getKey() const;
        virtual RecordId getValue() const;
        virtual void next();

        /**
         * BtreeIndexCursor-only.
         * Returns true if 'this' points at the same exact key as 'other'.
         * Returns false otherwise.
         */
        bool pointsAt(const BtreeIndexCursor& other);

        virtual Status savePosition();

        virtual Status restorePosition(OperationContext* txn);

        virtual std::string toString();

    private:
        // We keep the constructor private and only allow the AM to create us.
        friend class BtreeBasedAccessMethod;

        /**
         * interface is an abstraction to hide the fact that we have two types of Btrees.
         *
         * Intentionally private, we're friends with the only class allowed to call it.
         */
        BtreeIndexCursor(SortedDataInterface::Cursor* cursor);

        bool isSavedPositionValid();

        /**
         * Move to the next (or previous depending on the direction) key.  Used by normal getNext
         * and also skipping unused keys.
         */
        void advance();

        boost::scoped_ptr<SortedDataInterface::Cursor> _cursor;
    };

}  // namespace mongo
