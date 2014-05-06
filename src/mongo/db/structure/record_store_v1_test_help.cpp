// record_store_v1_test_help.cpp

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

#include "mongo/db/structure/record_store_v1_test_help.h"

#include "mongo/db/storage/extent.h"

namespace mongo {
    bool DummyTransactionExperiment::commitIfNeeded( bool force ) {
        return false;
    }

    bool DummyTransactionExperiment::isCommitNeeded() const {
        return false;
    }

    ProgressMeter* DummyTransactionExperiment::setMessage(const char* msg,
                                                          const std::string& name ,
                                                          unsigned long long progressMeterTotal,
                                                          int secondsBetween) {
        invariant( false );
    }

    void* DummyTransactionExperiment::writingPtr(void* data, size_t len) {
        return data;
    }

    void DummyTransactionExperiment::createdFile(const std::string& filename,
                                                 unsigned long long len) {
    }

    void DummyTransactionExperiment::syncDataAndTruncateJournal() {
    }

    void DummyTransactionExperiment::checkForInterrupt(bool heedMutex ) const {
    }

    Status DummyTransactionExperiment::checkForInterruptNoAssert() const {
        return Status::OK();
    }

    // -----------------------------------------

    DummyRecordStoreV1MetaData::DummyRecordStoreV1MetaData( bool capped, int userFlags ) {
        _dataSize = 0;
        _numRecords = 0;
        _capped = capped;
        _userFlags = userFlags;
        _lastExtentSize = 0;
        _paddingFactor = 1;
    }

    const DiskLoc& DummyRecordStoreV1MetaData::capExtent() const {
        return _capExtent;
    }

    void DummyRecordStoreV1MetaData::setCapExtent( TransactionExperiment* txn,
                                                   const DiskLoc& loc ) {
        _capExtent = loc;
    }

    const DiskLoc& DummyRecordStoreV1MetaData::capFirstNewRecord() const {
        return _capFirstNewRecord;
    }

    void DummyRecordStoreV1MetaData::setCapFirstNewRecord( TransactionExperiment* txn,
                                                           const DiskLoc& loc ) {
        _capFirstNewRecord = loc;
    }

    bool DummyRecordStoreV1MetaData::capLooped() const {
        invariant( false );
    }

    long long DummyRecordStoreV1MetaData::dataSize() const {
        return _dataSize;
    }

    long long DummyRecordStoreV1MetaData::numRecords() const {
        return _numRecords;
    }

    void DummyRecordStoreV1MetaData::incrementStats( TransactionExperiment* txn,
                                                     long long dataSizeIncrement,
                                                     long long numRecordsIncrement ) {
        _dataSize += dataSizeIncrement;
        _numRecords += numRecordsIncrement;
    }

    void DummyRecordStoreV1MetaData::setStats( TransactionExperiment* txn,
                                               long long dataSizeIncrement,
                                               long long numRecordsIncrement ) {
        _dataSize = dataSizeIncrement;
        _numRecords = numRecordsIncrement;
    }

    namespace {
        DiskLoc myNull;
    }

    const DiskLoc& DummyRecordStoreV1MetaData::deletedListEntry( int bucket ) const {
        invariant( bucket >= 0 );
        if ( static_cast<size_t>( bucket ) >= _deletedLists.size() )
            return myNull;
        return _deletedLists[bucket];
    }

    void DummyRecordStoreV1MetaData::setDeletedListEntry( TransactionExperiment* txn,
                                                          int bucket,
                                                          const DiskLoc& loc ) {
        invariant( bucket >= 0 );
        invariant( bucket < 1000 );
        while ( static_cast<size_t>( bucket ) >= _deletedLists.size() )
            _deletedLists.push_back( DiskLoc() );
        _deletedLists[bucket] = loc;
    }

    void DummyRecordStoreV1MetaData::orphanDeletedList(TransactionExperiment* txn) {
        invariant( false );
    }

    const DiskLoc& DummyRecordStoreV1MetaData::firstExtent() const {
        return _firstExtent;
    }

    void DummyRecordStoreV1MetaData::setFirstExtent( TransactionExperiment* txn,
                                                     const DiskLoc& loc ) {
        _firstExtent = loc;
    }

    const DiskLoc& DummyRecordStoreV1MetaData::lastExtent() const {
        return _lastExtent;
    }

    void DummyRecordStoreV1MetaData::setLastExtent( TransactionExperiment* txn,
                                                    const DiskLoc& loc ) {
        _lastExtent = loc;
    }

    bool DummyRecordStoreV1MetaData::isCapped() const {
        return _capped;
    }

    bool DummyRecordStoreV1MetaData::isUserFlagSet( int flag ) const {
        return _userFlags & flag;
    }

    int DummyRecordStoreV1MetaData::lastExtentSize() const {
        return _lastExtentSize;
    }

    void DummyRecordStoreV1MetaData::setLastExtentSize( TransactionExperiment* txn, int newMax ) {
        _lastExtentSize = newMax;
    }

    long long DummyRecordStoreV1MetaData::maxCappedDocs() const {
        invariant( false );
    }

    double DummyRecordStoreV1MetaData::paddingFactor() const {
        return _paddingFactor;
    }

    void DummyRecordStoreV1MetaData::setPaddingFactor( TransactionExperiment* txn,
                                                       double paddingFactor ) {
        _paddingFactor = paddingFactor;
    }

    // -----------------------------------------

    DummyExtentManager::~DummyExtentManager() {
        for ( size_t i = 0; i < _extents.size(); i++ ) {
            if ( _extents[i].data )
                free( _extents[i].data );
        }
    }

    Status DummyExtentManager::init(TransactionExperiment* txn) {
        return Status::OK();
    }

    size_t DummyExtentManager::numFiles() const {
        return _extents.size();
    }

    long long DummyExtentManager::fileSize() const {
        invariant( false );
        return -1;
    }

    void DummyExtentManager::flushFiles( bool sync ) {
    }

    DiskLoc DummyExtentManager::allocateExtent( TransactionExperiment* txn,
                                                bool capped,
                                                int size,
                                                int quotaMax ) {
        size = quantizeExtentSize( size );

        ExtentInfo info;
        info.data = static_cast<char*>( malloc( size ) );
        info.length = size;

        DiskLoc loc( _extents.size(), 0 );
        _extents.push_back( info );

        Extent* e = getExtent( loc );
        e->magic = Extent::extentSignature;
        e->myLoc = loc;
        e->xnext.Null();
        e->xprev.Null();
        e->length = size;
        e->firstRecord.Null();
        e->lastRecord.Null();

        return loc;

    }

    void DummyExtentManager::freeExtents( TransactionExperiment* txn,
                                          DiskLoc firstExt, DiskLoc lastExt ) {
        // XXX
    }

    void DummyExtentManager::freeExtent( TransactionExperiment* txn, DiskLoc extent ) {
        // XXX
    }
    void DummyExtentManager::freeListStats( int* numExtents, int64_t* totalFreeSize ) const {
        invariant( false );
    }

    Record* DummyExtentManager::recordForV1( const DiskLoc& loc ) const {
        invariant( static_cast<size_t>( loc.a() ) < _extents.size() );
        //log() << "DummyExtentManager::recordForV1: " << loc;
        char* root = _extents[loc.a()].data;
        return reinterpret_cast<Record*>( root + loc.getOfs() );
    }

    Extent* DummyExtentManager::extentForV1( const DiskLoc& loc ) const {
        invariant( false );
    }

    DiskLoc DummyExtentManager::extentLocForV1( const DiskLoc& loc ) const {
        return DiskLoc( loc.a(), 0 );
    }

    Extent* DummyExtentManager::getExtent( const DiskLoc& loc, bool doSanityCheck ) const {
        invariant( static_cast<size_t>( loc.a() ) < _extents.size() );
        invariant( loc.getOfs() == 0 );
        return reinterpret_cast<Extent*>( _extents[loc.a()].data );
    }

    int DummyExtentManager::maxSize() const {
        return 1024 * 1024 * 64;
    }


}
