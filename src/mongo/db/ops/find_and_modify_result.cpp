/**
 *    Copyright (C) 2017 MongoDB Inc.
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

#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kWrite

#include "mongo/platform/basic.h"

#include "mongo/db/ops/find_and_modify_result.h"

#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/lasterror.h"

namespace mongo {
namespace find_and_modify {
namespace {

void appendValue(const boost::optional<BSONObj>& value, BSONObjBuilder* builder) {
    if (value) {
        builder->append("value", *value);
    } else {
        builder->appendNull("value");
    }
}

}  // namespace

void serializeRemove(size_t n, const boost::optional<BSONObj>& value, BSONObjBuilder* builder) {
    BSONObjBuilder lastErrorObjBuilder(builder->subobjStart("lastErrorObject"));
    builder->appendNumber("n", n);
    lastErrorObjBuilder.doneFast();

    appendValue(value, builder);
}

void serializeUpsert(size_t n,
                     const boost::optional<BSONObj>& value,
                     bool updatedExisting,
                     const BSONObj& objInserted,
                     BSONObjBuilder* builder) {
    BSONObjBuilder lastErrorObjBuilder(builder->subobjStart("lastErrorObject"));
    lastErrorObjBuilder.appendNumber("n", n);
    lastErrorObjBuilder.appendBool("updatedExisting", updatedExisting);
    if (!objInserted.isEmpty()) {
        lastErrorObjBuilder.appendAs(objInserted["_id"], kUpsertedFieldName);
    }
    lastErrorObjBuilder.doneFast();

    appendValue(value, builder);
}

}  // namespace find_and_modify
}  // namespace mongo
