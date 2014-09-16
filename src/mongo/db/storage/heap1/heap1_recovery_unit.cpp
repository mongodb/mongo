// heap1_recovery_unit.cpp

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

#include "mongo/db/storage/heap1/heap1_recovery_unit.h"

#include "mongo/db/storage/sorted_data_interface.h"

namespace mongo {

    Heap1RecoveryUnit::~Heap1RecoveryUnit() {
        invariant( _depth == 0 );
    }

    void Heap1RecoveryUnit::beginUnitOfWork() {
        _depth++;
    }

    void Heap1RecoveryUnit::commitUnitOfWork() {
        if ( _depth == 1 ) {
            _rollbackPossible = true;
            _indexInserts.clear();
            _indexRemoves.clear();
        }
    }

    void Heap1RecoveryUnit::endUnitOfWork() {
        _depth--;

        // effectively do a rollback

        invariant( _rollbackPossible );

        for ( size_t i = 0; i < _indexInserts.size(); i++ ) {
            invariant( _depth == 0 ); // todo: fix me
            SortedDataInterface* idx = _indexInserts[i].idx;
            idx->unindex( NULL, _indexInserts[i].obj, _indexInserts[i].loc );
        }

        for ( size_t i = 0; i < _indexRemoves.size(); i++ ) {
            invariant( _depth == 0 ); // todo: fix me
            SortedDataInterface* idx = _indexRemoves[i].idx;
            idx->insert( NULL, _indexRemoves[i].obj, _indexRemoves[i].loc, true );
        }

    }

    void Heap1RecoveryUnit::notifyIndexInsert( SortedDataInterface* idx,
                                               const BSONObj& obj, const DiskLoc& loc ) {
        IndexInfo ii = { idx, obj, loc };
        _indexInserts.push_back( ii );
    }

    // static
    void Heap1RecoveryUnit::notifyIndexInsert( OperationContext* ctx, SortedDataInterface* idx,
                                               const BSONObj& obj, const DiskLoc& loc ) {
        if ( !ctx )
            return;

        Heap1RecoveryUnit* ru = dynamic_cast<Heap1RecoveryUnit*>( ctx->recoveryUnit() );
        ru->notifyIndexInsert( idx, obj, loc );
    }

    void Heap1RecoveryUnit::notifyIndexRemove( SortedDataInterface* idx,
                                               const BSONObj& obj, const DiskLoc& loc ) {
        IndexInfo ii = { idx, obj, loc };
        _indexRemoves.push_back( ii );
    }

    // static
    void Heap1RecoveryUnit::notifyIndexRemove( OperationContext* ctx, SortedDataInterface* idx,
                                               const BSONObj& obj, const DiskLoc& loc ) {
        if ( !ctx )
            return;

        Heap1RecoveryUnit* ru = dynamic_cast<Heap1RecoveryUnit*>( ctx->recoveryUnit() );
        ru->notifyIndexRemove( idx, obj, loc );
    }


}
