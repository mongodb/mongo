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

#pragma once

#include <memory>
#include <string>
#include <utility>

#include "mongo/base/disallow_copying.h"
#include "mongo/base/status_with.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/db/record_id.h"

namespace mongo {
namespace repl {

class OplogInterface {
    MONGO_DISALLOW_COPYING(OplogInterface);

public:
    class Iterator;

    OplogInterface() = default;
    virtual ~OplogInterface() = default;

    /**
     * Diagnostic information.
     */
    virtual std::string toString() const = 0;

    /**
     * Produces an iterator over oplog collection in reverse natural order.
     */
    virtual std::unique_ptr<Iterator> makeIterator() const = 0;
};

class OplogInterface::Iterator {
    MONGO_DISALLOW_COPYING(Iterator);

public:
    using Value = std::pair<BSONObj, RecordId>;

    Iterator() = default;
    virtual ~Iterator() = default;

    /**
     * Returns next operation and record id (if applicable) in the oplog.
     */
    virtual StatusWith<Value> next() = 0;
};

}  // namespace repl
}  // namespace mongo
