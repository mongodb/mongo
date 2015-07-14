/**
 *    Copyright (C) 2012 10gen Inc.
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
 *    must comply with the GNU Affero General Public License in all respects
 *    for all of the code used other than as permitted herein. If you modify
 *    file(s) with this exception, you may extend this exception to your
 *    version of the file(s), but you are not obligated to do so. If you do not
 *    wish to do so, delete this exception statement from your version. If you
 *    delete this exception statement from all source files in the program,
 *    then also delete it in the license file.
 */
#include "mongo/s/catalog/type_lockpings.h"

#include "mongo/base/status_with.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/util/bson_extract.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/mongoutils/str.h"

namespace mongo {
const std::string LockpingsType::ConfigNS = "config.lockpings";

const BSONField<std::string> LockpingsType::process("_id");
const BSONField<Date_t> LockpingsType::ping("ping");

StatusWith<LockpingsType> LockpingsType::fromBSON(const BSONObj& source) {
    LockpingsType lpt;

    {
        std::string lptProcess;
        Status status = bsonExtractStringField(source, process.name(), &lptProcess);
        if (!status.isOK())
            return status;
        lpt._process = lptProcess;
    }

    {
        BSONElement lptPingElem;
        Status status = bsonExtractTypedField(source, ping.name(), BSONType::Date, &lptPingElem);
        if (!status.isOK())
            return status;
        lpt._ping = lptPingElem.date();
    }

    return lpt;
}

Status LockpingsType::validate() const {
    if (!_process.is_initialized() || _process->empty()) {
        return {ErrorCodes::NoSuchKey, str::stream() << "missing " << process.name() << " field"};
    }

    if (!_ping.is_initialized()) {
        return {ErrorCodes::NoSuchKey, str::stream() << "missing " << ping.name() << " field"};
    }

    return Status::OK();
}

BSONObj LockpingsType::toBSON() const {
    BSONObjBuilder builder;

    if (_process)
        builder.append(process.name(), getProcess());
    if (_ping)
        builder.append(ping.name(), getPing());

    return builder.obj();
}

void LockpingsType::setProcess(const std::string& process) {
    invariant(!process.empty());
    _process = process;
}

void LockpingsType::setPing(const Date_t ping) {
    _ping = ping;
}

std::string LockpingsType::toString() const {
    return toBSON().toString();
}

}  // namespace mongo
