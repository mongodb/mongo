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

#include <memory>
#include <string>
#include <utility>

#include "mongo/base/status_with.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/db/record_id.h"
#include "mongo/util/net/hostandport.h"

namespace mongo {
namespace repl {

class OplogInterface {
    OplogInterface(const OplogInterface&) = delete;
    OplogInterface& operator=(const OplogInterface&) = delete;

public:
    class Iterator;

    virtual ~OplogInterface() = default;

    /**
     * Diagnostic information.
     */
    virtual std::string toString() const = 0;

    /**
     * Produces an iterator over oplog collection in reverse natural order.
     */
    virtual std::unique_ptr<Iterator> makeIterator() const = 0;

    /**
     * The host and port of the server.
     */
    virtual HostAndPort hostAndPort() const = 0;

protected:
    OplogInterface() = default;
};

class OplogInterface::Iterator {
    Iterator(const Iterator&) = delete;
    Iterator& operator=(const Iterator&) = delete;

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
