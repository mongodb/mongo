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
#include "mongo/db/index/btree_access_method_internal.h"
#include "mongo/db/index/index_descriptor.h"
#include "mongo/db/jsobj.h"

namespace mongo {

    /**
     * Maps (lat, lng) to the bucketSize-sided square bucket that contains it.
     * Examines all documents in a given radius of a given point.
     * Returns all documents that match a given search restriction.
     * See http://dochub.mongodb.org/core/haystackindexes
     *
     * Use when you want to look for restaurants within 25 miles with a certain name.
     * Don't use when you want to find the closest open restaurants; see 2d.cpp for that.
     *
     * Usage:
     * db.foo.ensureIndex({ pos : "geoHaystack", type : 1 }, { bucketSize : 1 })
     *   pos is the name of the field to be indexed that has lat/lng data in an array.
     *   type is the name of the secondary field to be indexed. 
     *   bucketSize specifies the dimension of the square bucket for the data in pos.
     * ALL fields are mandatory.
     */
    class HaystackAccessMethod : public BtreeBasedAccessMethod {
    public:
        using BtreeBasedAccessMethod::_descriptor;
        using BtreeBasedAccessMethod::_interface;

        HaystackAccessMethod(IndexDescriptor* descriptor);
        virtual ~HaystackAccessMethod() { }

        // Not implemented.
        virtual Status newCursor(IndexCursor** out);

    protected:
        friend class GeoHaystackSearchCommand;
        void searchCommand(const BSONObj& nearObj, double maxDistance, const BSONObj& search,
                           BSONObjBuilder* result, unsigned limit);

    private:
        virtual void getKeys(const BSONObj& obj, BSONObjSet* keys);

        // Helper methods called by getKeys:
        int hash(const BSONElement& e) const;
        string makeString(int hashedX, int hashedY) const;
        void addKey(const string& root, const BSONElement& e, BSONObjSet* keys) const;

        string _geoField;
        vector<string> _otherFields;
        double _bucketSize;
    };

}  // namespace mongo
