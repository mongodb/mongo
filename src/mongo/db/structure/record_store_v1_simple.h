// record_store_v1_simple.h

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
#include "mongo/db/structure/record_store_v1_base.h"

namespace mongo {

    class SimpleRecordStoreV1Iterator;

    // used by index and original collections
    class SimpleRecordStoreV1 : public RecordStoreV1Base {
    public:
        SimpleRecordStoreV1( const StringData& ns,
                             NamespaceDetails* details,
                             ExtentManager* em,
                             bool isSystemIndexes );

        virtual ~SimpleRecordStoreV1();

        virtual RecordIterator* getIterator( const DiskLoc& start, bool tailable,
                                             const CollectionScanParams::Direction& dir) const;

        virtual Status truncate();
    protected:
        virtual StatusWith<DiskLoc> allocRecord( int lengthWithHeaders, int quotaMax );

        virtual void addDeletedRec(const DiskLoc& dloc);
    private:
        DiskLoc _allocFromExistingExtents( int lengthWithHeaders );

        bool _normalCollection;

        friend class SimpleRecordStoreV1Iterator;
    };

}
