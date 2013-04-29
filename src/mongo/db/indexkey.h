// index_key.h

/**
*    Copyright (C) 2008 10gen Inc.
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
*/

#pragma once

#include <map>

#include "mongo/db/diskloc.h"
#include "mongo/db/index_names.h"
#include "mongo/db/keypattern.h"
#include "mongo/db/jsobj.h"

namespace mongo {

    extern const int DefaultIndexVersionNumber;

    const int ParallelArraysCode = 10088;
    
    class Cursor;
    class IndexSpec;
    class IndexDetails;
    class FieldRangeSet;

    enum IndexSuitability { USELESS = 0 , HELPFUL = 1 , OPTIMAL = 2 };

    /* precomputed details about an index, used for inserting keys on updates
       stored/cached in NamespaceDetailsTransient, or can be used standalone
       */
    class IndexSpec {
    public:
        BSONObj keyPattern; // e.g., { name : 1 }
        BSONObj info; // this is the same as IndexDetails::info.obj()

        IndexSpec()
            : _details(0) , _finishedInit(false) {
        }

        explicit IndexSpec(const BSONObj& k, const BSONObj& m=BSONObj())
            : keyPattern(k) , info(m) , _details(0) , _finishedInit(false) {
            _init();
        }

        /**
           this is a DiscLoc of an IndexDetails info
           should have a key field
         */
        explicit IndexSpec(const DiskLoc& loc) {
            reset(loc);
        }

        void reset(const BSONObj& info);
        void reset(const IndexDetails * details); // determines rules based on pdfile version
        void reset(const DiskLoc& infoLoc) {
            reset(infoLoc.obj());
        }

        string getTypeName() const;

        const IndexDetails * getDetails() const {
            return _details;
        }

        bool isSparse() const { return _sparse; }

        string toString() const;

    protected:

        int indexVersion() const;
        
        BSONSizeTracker _sizeTracker;
        vector<const char*> _fieldNames;
        vector<BSONElement> _fixed;

        BSONObj _nullKey; // a full key with all fields null
        BSONObj _nullObj; // only used for _nullElt
        BSONElement _nullElt; // jstNull

        BSONObj _undefinedObj; // only used for _undefinedElt
        BSONElement _undefinedElt; // undefined

        int _nFields; // number of fields in the index
        bool _sparse; // if the index is sparse
        const IndexDetails * _details;

        void _init();

        friend class KeyGeneratorV0;
        friend class KeyGeneratorV1;
    public:
        bool _finishedInit;
    };


} // namespace mongo
