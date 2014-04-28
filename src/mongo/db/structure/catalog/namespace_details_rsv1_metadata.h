// namespace_details_rsv1_metadata.h

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

#include "mongo/db/structure/catalog/namespace_details.h"
#include "mongo/db/structure/record_store_v1_base.h"

namespace mongo {

    /*
     * NOTE: NamespaceDetails will become a struct
     *      all dur, etc... will move here
     */
    class NamespaceDetailsRSV1MetaData : public RecordStoreV1MetaData {
    public:
        explicit NamespaceDetailsRSV1MetaData( NamespaceDetails* details ) {
            _details = details;
        }

        virtual ~NamespaceDetailsRSV1MetaData(){}

        virtual int bucket(int size) const {
            return _details->bucket( size );
        }

        virtual const DiskLoc& capExtent() const {
            return _details->capExtent();
        }

        virtual void setCapExtent( TransactionExperiment* txn, const DiskLoc& loc ) {
            _details->setCapExtent( loc );
        }

        virtual const DiskLoc& capFirstNewRecord() const {
            return _details->capFirstNewRecord();
        }

        virtual void setCapFirstNewRecord( TransactionExperiment* txn, const DiskLoc& loc ) {
            _details->setCapFirstNewRecord( loc );
        }

        virtual bool capLooped() const {
            return _details->capLooped();
        }

        virtual void clearSystemFlags(TransactionExperiment* txn) {
            _details->clearSystemFlags();
        }

        virtual long long dataSize() const {
            return _details->dataSize();
        }
        virtual long long numRecords() const {
            return _details->numRecords();
        }

        virtual void incrementStats( TransactionExperiment* txn,
                                     long long dataSizeIncrement,
                                     long long numRecordsIncrement ) {
            _details->incrementStats( dataSizeIncrement, numRecordsIncrement );
        }

        virtual void setStats( TransactionExperiment* txn,
                               long long dataSizeIncrement,
                               long long numRecordsIncrement ) {
            _details->setStats( dataSizeIncrement,
                                numRecordsIncrement );
        }

        virtual const DiskLoc& deletedListEntry( int bucket ) const {
            return _details->deletedListEntry( bucket );
        }

        virtual void setDeletedListEntry( TransactionExperiment* txn,
                                          int bucket,
                                          const DiskLoc& loc ) {
            _details->setDeletedListEntry( bucket, loc );
        }

        virtual void orphanDeletedList(TransactionExperiment* txn) {
            _details->orphanDeletedList();
        }

        virtual const DiskLoc& firstExtent() const {
            return _details->firstExtent();
        }

        virtual void setFirstExtent( TransactionExperiment* txn, const DiskLoc& loc ) {
            _details->setFirstExtent( loc );
        }

        virtual const DiskLoc& lastExtent() const {
            return _details->lastExtent();
        }

        virtual void setLastExtent( TransactionExperiment* txn, const DiskLoc& loc ) {
            _details->setLastExtent( loc );
        }

        virtual bool isCapped() const {
            return _details->isCapped();
        }

        virtual bool isUserFlagSet( int flag ) const {
            return _details->isUserFlagSet( flag );
        }

        virtual int lastExtentSize() const {
            return _details->lastExtentSize();
        }

        virtual void setLastExtentSize( TransactionExperiment* txn, int newMax ) {
            _details->setLastExtentSize( newMax );
        }

        virtual long long maxCappedDocs() const {
            return _details->maxCappedDocs();
        }

        virtual double paddingFactor() const {
            return _details->paddingFactor();
        }

        virtual void setPaddingFactor( TransactionExperiment* txn, double paddingFactor ) {
            _details->setPaddingFactor( paddingFactor );
        }

        virtual int quantizePowerOf2AllocationSpace(int allocSize) const {
            return _details->quantizePowerOf2AllocationSpace( allocSize );
        }

    private:
        NamespaceDetails* _details;
    };

}
