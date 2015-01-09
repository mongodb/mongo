// record_store_test_updaterecord.h

/**
 *    Copyright (C) 2014 MongoDB Inc.
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

#include "mongo/db/record_id.h"
#include "mongo/db/storage/record_data.h"
#include "mongo/db/storage/record_store.h"
#include "mongo/unittest/unittest.h"

namespace mongo {
namespace {

    class UpdateMoveNotifierSpy : public UpdateMoveNotifier {
    public:
        UpdateMoveNotifierSpy( OperationContext* txn, const RecordId &loc,
                               const char *buf, size_t size )
                : _txn( txn ), _loc( loc ), _data( buf, size ), nCalls( 0 ) {
        }

        ~UpdateMoveNotifierSpy() { }

        Status recordStoreGoingToMove( OperationContext *txn,
                                       const RecordId &oldLocation,
                                       const char *oldBuffer,
                                       size_t oldSize ) {
            nCalls++;
            ASSERT_EQUALS( _txn, txn );
            ASSERT_EQUALS( _loc, oldLocation );
            ASSERT_EQUALS( _data, oldBuffer );
            return Status::OK();
        }

        int getNumCalls() const { return nCalls; }

    private:
        OperationContext *_txn;
        RecordId _loc;
        std::string _data;

        int nCalls; // to verify that recordStoreGoingToMove() gets called once
    };

} // namespace
} // namespace mongo
