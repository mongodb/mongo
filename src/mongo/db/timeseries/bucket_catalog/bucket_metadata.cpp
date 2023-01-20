/**
 *    Copyright (C) 2023-present MongoDB, Inc.
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

#include "mongo/db/timeseries/bucket_catalog/bucket_metadata.h"

#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/timeseries/bucket_catalog/bucket_catalog_helpers.h"

namespace mongo::timeseries::bucket_catalog {

BucketMetadata::BucketMetadata(BSONElement elem, const StringData::ComparatorInterface* comparator)
    : _metadataElement(elem), _comparator(comparator) {
    if (_metadataElement) {
        BSONObjBuilder objBuilder;
        // We will get an object of equal size, just with reordered fields.
        objBuilder.bb().reserveBytes(_metadataElement.size());
        normalizeMetadata(&objBuilder, _metadataElement, boost::none);
        _metadata = objBuilder.obj();
    }
    // Updates the BSONElement to refer to the copied BSONObj.
    _metadataElement = _metadata.firstElement();
}

bool BucketMetadata::operator==(const BucketMetadata& other) const {
    return _metadataElement.binaryEqualValues(other._metadataElement);
}

const BSONObj& BucketMetadata::toBSON() const {
    return _metadata;
}

const BSONElement& BucketMetadata::element() const {
    return _metadataElement;
}

StringData BucketMetadata::getMetaField() const {
    return StringData(_metadataElement.fieldName());
}

const StringData::ComparatorInterface* BucketMetadata::getComparator() const {
    return _comparator;
}

}  // namespace mongo::timeseries::bucket_catalog
