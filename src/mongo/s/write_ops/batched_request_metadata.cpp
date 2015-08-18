/**
 *    Copyright (C) 2013 MongoDB Inc.
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

#include "mongo/s/write_ops/batched_request_metadata.h"

#include "mongo/db/field_parser.h"

namespace mongo {

using std::unique_ptr;
using std::string;

const BSONField<ChunkVersion> BatchedRequestMetadata::shardVersion("shardVersion");
const BSONField<long long> BatchedRequestMetadata::session("session");

BatchedRequestMetadata::BatchedRequestMetadata() : _session(0), _isSessionSet(false) {}

BatchedRequestMetadata::~BatchedRequestMetadata() {}

bool BatchedRequestMetadata::isValid(string* errMsg) const {
    // all fields are mandatory.
    return true;
}

BSONObj BatchedRequestMetadata::toBSON() const {
    BSONObjBuilder metadataBuilder;

    if (_shardVersion) {
        _shardVersion.get().appendForCommands(&metadataBuilder);
    }

    if (_isSessionSet)
        metadataBuilder << session(_session);

    return metadataBuilder.obj();
}

bool BatchedRequestMetadata::parseBSON(const BSONObj& source, string* errMsg) {
    clear();

    string dummy;
    if (!errMsg)
        errMsg = &dummy;

    {
        auto verAndOpTStatus = ChunkVersionAndOpTime::parseFromBSONForCommands(source);
        if (!verAndOpTStatus.isOK()) {
            return false;
        }

        _shardVersion = verAndOpTStatus.getValue();
    }

    FieldParser::FieldState fieldState = FieldParser::extract(source, session, &_session, errMsg);
    if (fieldState == FieldParser::FIELD_INVALID)
        return false;
    _isSessionSet = fieldState == FieldParser::FIELD_SET;

    return true;
}

void BatchedRequestMetadata::clear() {
    _shardVersion.reset();

    _session = 0;
    _isSessionSet = false;
}

string BatchedRequestMetadata::toString() const {
    return toBSON().toString();
}

void BatchedRequestMetadata::cloneTo(BatchedRequestMetadata* other) const {
    other->_shardVersion = _shardVersion;
    other->_session = _session;
    other->_isSessionSet = _isSessionSet;
}

void BatchedRequestMetadata::setShardVersion(const ChunkVersionAndOpTime& shardVersion) {
    _shardVersion = shardVersion;
}

bool BatchedRequestMetadata::isShardVersionSet() const {
    return _shardVersion.is_initialized();
}

const ChunkVersion& BatchedRequestMetadata::getShardVersion() const {
    return _shardVersion.get().getVersion();
}

void BatchedRequestMetadata::setSession(long long session) {
    _session = session;
    _isSessionSet = true;
}

void BatchedRequestMetadata::unsetSession() {
    _isSessionSet = false;
}

bool BatchedRequestMetadata::isSessionSet() const {
    return _isSessionSet;
}

long long BatchedRequestMetadata::getSession() const {
    dassert(_isSessionSet);
    return _session;
}
}
