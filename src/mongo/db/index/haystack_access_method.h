/**
 *    Copyright (C) 2018-present MongoDB, Inc.
 *
 *    This program is free software: you can redistribute it and/or modify
 *    it under the terms of the Server Side Public License, version 1,
 *    as published by MongoDB, Inc.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    Server Side Public License for more details.
 *
 *    You should have received a copy of the Server Side Public License
 *    along with this program. If not, see
 *    <http://www.mongodb.com/licensing/server-side-public-license>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the Server Side Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */

#pragma once

#include "mongo/base/status.h"
#include "mongo/db/index/index_access_method.h"
#include "mongo/db/index/index_descriptor.h"
#include "mongo/db/jsobj.h"

namespace mongo {

class Collection;
class OperationContext;

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
class HaystackAccessMethod : public AbstractIndexAccessMethod {
public:
    HaystackAccessMethod(IndexCatalogEntry* btreeState, std::unique_ptr<SortedDataInterface> btree);

protected:
    friend class GeoHaystackSearchCommand;
    void searchCommand(OperationContext* opCtx,
                       Collection* collection,
                       const BSONObj& nearObj,
                       double maxDistance,
                       const BSONObj& search,
                       BSONObjBuilder* result,
                       unsigned limit) const;

private:
    /**
     * Fills 'keys' with the keys that should be generated for 'obj' on this index.
     *
     * This function ignores the 'multikeyPaths' and 'multikeyMetadataKeys' pointers because
     * geoHaystack indexes don't support tracking path-level multikey information.
     */
    void doGetKeys(const BSONObj& obj,
                   KeyStringSet* keys,
                   KeyStringSet* multikeyMetadataKeys,
                   MultikeyPaths* multikeyPaths,
                   boost::optional<RecordId> id) const final;

    std::string _geoField;
    std::vector<std::string> _otherFields;
    double _bucketSize;
};

}  // namespace mongo
