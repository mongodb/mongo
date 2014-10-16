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
        invariant( _frames.empty() );
    }

    void Heap1RecoveryUnit::beginUnitOfWork() {
        _frames.push_back( Frame() );
    }

    void Heap1RecoveryUnit::commitUnitOfWork() {
        if ( _frames.size() == 1 ) {
            _rollbackPossible = true;
        }
        else {
            size_t last = _frames.size() - 1;
            size_t next = last - 1;
            _frames[next].indexMods.insert( _frames[next].indexMods.end(),
                                            _frames[last].indexMods.begin(),
                                            _frames[last].indexMods.end() );
        }
        _frames.back().indexMods.clear();
    }

    void Heap1RecoveryUnit::endUnitOfWork() {

        // invariant( _rollbackPossible ); // todo

        const Frame& frame = _frames.back();
        for ( size_t i = frame.indexMods.size() ; i > 0; i-- ) {
            const IndexInfo& ii = frame.indexMods[i-1];
            SortedDataInterface* idx = ii.idx;
            if ( ii.insert )
                idx->unindex( NULL, ii.obj, ii.loc, true );
            else
                idx->insert( NULL, ii.obj, ii.loc, true );
        }

        _frames.pop_back();
    }

    void Heap1RecoveryUnit::notifyIndexMod( SortedDataInterface* idx,
                                            const BSONObj& obj, const DiskLoc& loc, bool insert ) {
        IndexInfo ii = { idx, obj, loc, insert };
        _frames.back().indexMods.push_back( ii );
    }

    // static
    void Heap1RecoveryUnit::notifyIndexInsert( OperationContext* ctx, SortedDataInterface* idx,
                                               const BSONObj& obj, const DiskLoc& loc ) {
        if ( !ctx )
            return;

        Heap1RecoveryUnit* ru = dynamic_cast<Heap1RecoveryUnit*>( ctx->recoveryUnit() );
        ru->notifyIndexMod( idx, obj, loc, true );
    }

    // static
    void Heap1RecoveryUnit::notifyIndexRemove( OperationContext* ctx, SortedDataInterface* idx,
                                               const BSONObj& obj, const DiskLoc& loc ) {
        if ( !ctx )
            return;

        Heap1RecoveryUnit* ru = dynamic_cast<Heap1RecoveryUnit*>( ctx->recoveryUnit() );
        ru->notifyIndexMod( idx, obj, loc, false );
    }


}
