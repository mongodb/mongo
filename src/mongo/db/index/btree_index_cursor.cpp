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

#include "mongo/db/index/btree_index_cursor.h"

#include <vector>

#include "mongo/base/status.h"
#include "mongo/db/index/index_cursor.h"
#include "mongo/db/index/index_descriptor.h"
#include "mongo/db/jsobj.h"
#include "mongo/db/record_id.h"
#include "mongo/platform/unordered_set.h"

namespace mongo {

    using std::string;
    using std::vector;

    BtreeIndexCursor::BtreeIndexCursor(SortedDataInterface::Cursor* cursor) : _cursor(cursor) { }

    bool BtreeIndexCursor::isEOF() const { return _cursor->isEOF(); }

    Status BtreeIndexCursor::seek(const BSONObj& position) {
        _cursor->locate(position, 
                        1 == _cursor->getDirection() ? RecordId::min() : RecordId::max());
        return Status::OK();
    }

    void BtreeIndexCursor::seek(const BSONObj& position, bool afterKey) {
        const bool forward = (1 == _cursor->getDirection());
        _cursor->locate(position, (afterKey == forward) ? RecordId::max() : RecordId::min());
    }

    bool BtreeIndexCursor::pointsAt(const BtreeIndexCursor& other) {
        return _cursor->pointsToSamePlaceAs(*other._cursor);
    }

    Status BtreeIndexCursor::seek(const vector<const BSONElement*>& position,
                                  const vector<bool>& inclusive) {

        BSONObj emptyObj;

        _cursor->customLocate(emptyObj,
                              0,
                              false,
                              position,
                              inclusive);
        return Status::OK();
    }

    Status BtreeIndexCursor::skip(const BSONObj &keyBegin,
                                  int keyBeginLen,
                                  bool afterKey,
                                  const vector<const BSONElement*>& keyEnd,
                                  const vector<bool>& keyEndInclusive) {

        _cursor->advanceTo(keyBegin,
                           keyBeginLen,
                           afterKey,
                           keyEnd,
                           keyEndInclusive);
        return Status::OK();
    }

    BSONObj BtreeIndexCursor::getKey() const {
        return _cursor->getKey();
    }

    RecordId BtreeIndexCursor::getValue() const {
        return _cursor->getRecordId();
    }

    void BtreeIndexCursor::next() {
        advance();
    }

    Status BtreeIndexCursor::savePosition() {
        _cursor->savePosition();
        return Status::OK();
    }

    Status BtreeIndexCursor::restorePosition(OperationContext* txn) {
        _cursor->restorePosition(txn);
        return Status::OK();
    }

    string BtreeIndexCursor::toString() {
        // TODO: is this ever called?
        return "I AM A BTREE INDEX CURSOR!\n";
    }

    // Move to the next/prev. key.  Used by normal getNext and also skipping unused keys.
    void BtreeIndexCursor::advance() {
        _cursor->advance();
    }

}  // namespace mongo
