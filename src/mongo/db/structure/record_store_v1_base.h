// record_store_v1_base.h

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

#include "mongo/db/diskloc.h"
#include "mongo/db/structure/record_store.h"

namespace mongo {

    class CappedRecordStoreV1Iterator;
    class DocWriter;
    class ExtentManager;
    class NamespaceDetails;
    class Record;

    class RecordStoreV1Base : public RecordStore {
    public:
        RecordStoreV1Base( const StringData& ns,
                           NamespaceDetails* details,
                           ExtentManager* em,
                           bool isSystemIndexes );

        virtual ~RecordStoreV1Base();

        Record* recordFor( const DiskLoc& loc ) const;

        void deleteRecord( const DiskLoc& dl );

        StatusWith<DiskLoc> insertRecord( const char* data, int len, int quotaMax );

        StatusWith<DiskLoc> insertRecord( const DocWriter* doc, int quotaMax );

        void increaseStorageSize( int size, int quotaMax );

        virtual Status validate( bool full, bool scanData,
                                 ValidateAdaptor* adaptor,
                                 ValidateResults* results, BSONObjBuilder* output ) const;

        // TODO: another sad one
        virtual const DeletedRecord* deletedRecordFor( const DiskLoc& loc ) const;

        const NamespaceDetails* details() const { return _details; }

        /**
         * @return the actual size to create
         *         will be >= oldRecordSize
         *         based on padding and any other flags
         */
        int getRecordAllocationSize( int minRecordSize ) const;

    protected:

        virtual bool isCapped() const = 0;

        virtual StatusWith<DiskLoc> allocRecord( int lengthWithHeaders, int quotaMax ) = 0;

        // TODO: document, remove, what have you
        virtual void addDeletedRec( const DiskLoc& dloc) = 0;

        // TODO: another sad one
        virtual DeletedRecord* drec( const DiskLoc& loc ) const;

        // just a haper for _extentManager->getExtent( loc );
        Extent* _getExtent( const DiskLoc& loc ) const;

        /** add a record to the end of the linked list chain within this extent.
            require: you must have already declared write intent for the record header.
        */
        void _addRecordToRecListInExtent(Record* r, DiskLoc loc);

        NamespaceDetails* _details;
        ExtentManager* _extentManager;
        bool _isSystemIndexes;

        friend class CappedRecordStoreV1Iterator;
    };

}
