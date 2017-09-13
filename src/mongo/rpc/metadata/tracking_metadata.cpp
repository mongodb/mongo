/**
 *    Copyright (C) 2016 MongoDB Inc.
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

#include "mongo/platform/basic.h"

#include "mongo/rpc/metadata/tracking_metadata.h"

#include "mongo/bson/util/bson_check.h"
#include "mongo/bson/util/bson_extract.h"
#include "mongo/db/jsobj.h"
#include "mongo/rpc/metadata.h"

namespace mongo {
namespace rpc {

namespace {

const char kOperIdFieldName[] = "operId";
const char kOperNameFieldName[] = "operName";
const char kParentOperIdFieldName[] = "parentOperId";

}  // unnamed namespace

const OperationContext::Decoration<TrackingMetadata> TrackingMetadata::get =
    OperationContext::declareDecoration<TrackingMetadata>();

TrackingMetadata::TrackingMetadata(OID operId, std::string operName)
    : _operId(std::move(operId)), _operName(std::move(operName)) {}

TrackingMetadata::TrackingMetadata(OID operId, std::string operName, std::string parentOperId)
    : _operId(std::move(operId)),
      _operName(std::move(operName)),
      _parentOperId(std::move(parentOperId)) {}

StatusWith<TrackingMetadata> TrackingMetadata::readFromMetadata(const BSONObj& metadataObj) {
    return readFromMetadata(metadataObj.getField(fieldName()));
}

void TrackingMetadata::initWithOperName(const std::string& name) {
    // _operId to be already initialized if it was created with constructChildMetadata.
    if (!_operId) {
        OID operId;
        operId.init();
        _operId = operId;
    }
    _operName = name;
}

std::string TrackingMetadata::toString() const {
    invariant(_operId);
    invariant(_operName);
    std::ostringstream stream;
    if (_parentOperId) {
        stream << "Cmd: " << *_operName << ", TrackingId: " << *_parentOperId << "|" << *_operId;
    } else {
        stream << "Cmd: " << *_operName << ", TrackingId: " << *_operId;
    }
    return stream.str();
}

TrackingMetadata TrackingMetadata::constructChildMetadata() const {
    OID newOperId;
    newOperId.init();
    std::string newParentOperId =
        _parentOperId ? *_parentOperId + "|" + _operId->toString() : _operId->toString();

    return TrackingMetadata(newOperId, std::string(), newParentOperId);
}

StatusWith<TrackingMetadata> TrackingMetadata::readFromMetadata(const BSONElement& metadataElem) {
    if (metadataElem.eoo()) {
        return TrackingMetadata{};
    } else if (metadataElem.type() != mongo::Object) {
        return {ErrorCodes::TypeMismatch,
                str::stream() << "TrackingMetadata element has incorrect type: expected"
                              << mongo::Object
                              << " but got "
                              << metadataElem.type()};
    }

    BSONObj metadataObj = metadataElem.Obj();

    OID operId;
    auto status = bsonExtractOIDField(metadataObj, kOperIdFieldName, &operId);
    if (!status.isOK()) {
        return status;
    }

    std::string operName;
    status = bsonExtractStringField(metadataObj, kOperNameFieldName, &operName);
    if (!status.isOK()) {
        return status;
    }

    std::string parentOperId;
    status = bsonExtractStringField(metadataObj, kParentOperIdFieldName, &parentOperId);
    if (!status.isOK()) {
        if (status != ErrorCodes::NoSuchKey) {
            return status;
        }
        return TrackingMetadata(std::move(operId), std::move(operName));
    }

    return TrackingMetadata(std::move(operId), std::move(operName), std::move(parentOperId));
}

void TrackingMetadata::writeToMetadata(BSONObjBuilder* builder) const {
    BSONObjBuilder metadataBuilder(builder->subobjStart(fieldName()));

    invariant(_operId);
    invariant(_operName);
    metadataBuilder.append(kOperIdFieldName, *_operId);
    metadataBuilder.append(kOperNameFieldName, *_operName);

    if (_parentOperId) {
        metadataBuilder.append(kParentOperIdFieldName, *_parentOperId);
    }
}

BSONObj TrackingMetadata::removeTrackingData(BSONObj metadata) {
    BSONObjBuilder builder;
    for (auto elem : metadata) {
        if (elem.fieldNameStringData() != rpc::TrackingMetadata::fieldName()) {
            builder.append(elem);
        }
    }
    return builder.obj();
}

}  // namespace rpc
}  // namespace mongo
