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

#include "mongo/s/catalog/type_config_version.h"

#include <boost/move/utility_core.hpp>

#include <boost/optional/optional.hpp>

#include "mongo/base/error_codes.h"
#include "mongo/base/status_with.h"
#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/bsontypes.h"
#include "mongo/bson/util/bson_extract.h"

namespace mongo {

const NamespaceString VersionType::ConfigNS(NamespaceString::kConfigVersionNamespace);

const BSONField<OID> VersionType::clusterId("clusterId");

void VersionType::clear() {
    _clusterId = OID{};
}

void VersionType::cloneTo(VersionType* other) const {
    other->clear();
    other->_clusterId = _clusterId;
}

Status VersionType::validate() const {
    return Status::OK();
}

BSONObj VersionType::toBSON() const {
    BSONObjBuilder builder;

    builder.append("_id", 1);
    builder.append(clusterId.name(), getClusterId());
    return builder.obj();
}

StatusWith<VersionType> VersionType::fromBSON(const BSONObj& source) {
    VersionType version;

    {
        BSONElement vClusterIdElem;
        Status status =
            bsonExtractTypedField(source, clusterId.name(), BSONType::jstOID, &vClusterIdElem);
        if (!status.isOK())
            return status;
        version._clusterId = vClusterIdElem.OID();
    }

    return version;
}

void VersionType::setClusterId(const OID& clusterId) {
    _clusterId = clusterId;
}

std::string VersionType::toString() const {
    return toBSON().toString();
}

}  // namespace mongo
