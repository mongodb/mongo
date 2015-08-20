/**
 *    Copyright (C) 2015 MongoDB Inc.
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

#include <ostream>
#include <sstream>
#include <string>
#include <utility>

#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/util/bson_extract.h"
#include "mongo/db/repl/optime.h"

namespace mongo {
namespace repl {
namespace {

const char* kTimestampFieldName = "ts";
const char* kTermFieldName = "t";

}  // namespace

OpTime::OpTime(Timestamp ts, long long term) : _timestamp(std::move(ts)), _term(term) {}

Timestamp OpTime::getTimestamp() const {
    return _timestamp;
}

long long OpTime::getSecs() const {
    return _timestamp.getSecs();
}

long long OpTime::getTerm() const {
    return _term;
}

bool OpTime::isNull() const {
    return _timestamp.isNull();
}

void OpTime::append(BSONObjBuilder* builder) const {
    builder->append(kTimestampFieldName, _timestamp);

    // Don't add term in protocol version 0.
    if (_term != kUninitializedTerm) {
        builder->append(kTermFieldName, _term);
    }
}

StatusWith<OpTime> OpTime::parseFromBSON(const BSONObj& obj) {
    Timestamp ts;
    Status status = bsonExtractTimestampField(obj, kTimestampFieldName, &ts);
    if (!status.isOK())
        return status;

    // Default to -1 if the term is absent.
    long long term;
    status = bsonExtractIntegerFieldWithDefault(obj, kTermFieldName, kUninitializedTerm, &term);
    if (!status.isOK())
        return status;

    return OpTime(ts, term);
}

std::string OpTime::toString() const {
    std::stringstream ss;
    ss << "(term: " << _term << ", timestamp: " << _timestamp.toStringPretty() << ")";
    return ss.str();
}

std::ostream& operator<<(std::ostream& out, const OpTime& opTime) {
    return out << opTime.toString();
}

}  // namespace repl
}  // namespace mongo
