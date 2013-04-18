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
*/

#pragma once

#include "mongo/base/status.h"
#include "mongo/db/index/2d_common.h"
#include "mongo/db/index/btree_access_method_internal.h"
#include "mongo/db/jsobj.h"

namespace mongo {

    class IndexCursor;
    class IndexDescriptor;
    struct TwoDIndexingParams;

    namespace twod_internal {
        class GeoPoint;
        class GeoAccumulator;
        class GeoBrowse;
        class GeoHopper;
        class GeoSearch;
        class GeoCircleBrowse;
        class GeoBoxBrowse;
        class GeoPolygonBrowse;
    }

    class TwoDAccessMethod : public BtreeBasedAccessMethod {
    public:
        using BtreeBasedAccessMethod::_descriptor;
        using BtreeBasedAccessMethod::_interface;
        using BtreeBasedAccessMethod::_ordering;

        TwoDAccessMethod(IndexDescriptor* descriptor);
        virtual ~TwoDAccessMethod() { }

        virtual Status newCursor(IndexCursor** out);

    private:
        friend class TwoDIndexCursor;
        friend class twod_internal::GeoPoint;
        friend class twod_internal::GeoAccumulator;
        friend class twod_internal::GeoBrowse;
        friend class twod_internal::GeoHopper;
        friend class twod_internal::GeoSearch;
        friend class twod_internal::GeoCircleBrowse;
        friend class twod_internal::GeoBoxBrowse;
        friend class twod_internal::GeoPolygonBrowse;

        BtreeInterface* getInterface() { return _interface; }
        IndexDescriptor* getDescriptor() { return _descriptor; }
        TwoDIndexingParams& getParams() { return _params; }

        // This really gets the 'locs' from the provided obj.
        void getKeys(const BSONObj& obj, vector<BSONObj>& locs) const;

        virtual void getKeys(const BSONObj& obj, BSONObjSet* keys);

        // This is called by the two getKeys above.
        void getKeys(const BSONObj &obj, BSONObjSet* keys, vector<BSONObj>* locs) const;

        BSONObj _nullObj;
        BSONElement _nullElt;
        TwoDIndexingParams _params;
    };

}  // namespace mongo
