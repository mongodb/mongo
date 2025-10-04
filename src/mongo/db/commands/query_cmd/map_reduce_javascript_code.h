/**
 *    Copyright (C) 2019-present MongoDB, Inc.
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

#include "mongo/base/string_data.h"
#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/util/modules.h"

#include <string>

#include <boost/optional.hpp>

namespace mongo {

/**
 * Code to run as a component of MapReduce.
 */
class MapReduceJavascriptCode {
public:
    static MapReduceJavascriptCode parseFromBSON(const BSONElement& element) {
        uassert(ErrorCodes::BadValue,
                str::stream() << "'" << element.fieldNameStringData()
                              << "' must be of string or code type",
                element.type() == BSONType::string || element.type() == BSONType::code);
        return MapReduceJavascriptCode(element._asCode());
    }

    MapReduceJavascriptCode() = default;
    MapReduceJavascriptCode(std::string&& code) : code(code) {}

    void serializeToBSON(StringData fieldName, BSONObjBuilder* builder) const {
        (*builder) << fieldName << code;
    }

    auto getCode() const {
        return code;
    }

private:
    std::string code;
};

/**
 * Same as above, but allows for null. This is required for older versions of the Java driver which
 * send finalize: null if the argument is omitted by the user.
 */
class MONGO_MOD_PRIVATE MapReduceJavascriptCodeOrNull {
public:
    static MapReduceJavascriptCodeOrNull parseFromBSON(const BSONElement& element) {
        if (element.type() == BSONType::null) {
            return MapReduceJavascriptCodeOrNull(boost::none);
        }
        uassert(ErrorCodes::BadValue,
                str::stream() << "'" << element.fieldNameStringData()
                              << "' must be of string or code type",
                element.type() == BSONType::string || element.type() == BSONType::code);
        return MapReduceJavascriptCodeOrNull(boost::make_optional(element._asCode()));
    }

    MapReduceJavascriptCodeOrNull() = default;
    MapReduceJavascriptCodeOrNull(boost::optional<std::string> code) : code(code) {}

    void serializeToBSON(StringData fieldName, BSONObjBuilder* builder) const {
        if (code == boost::none) {
            (*builder) << fieldName << "null";
        } else {
            (*builder) << fieldName << code.get();
        }
    }

    auto getCode() const {
        return code;
    }

    bool hasCode() const {
        return code != boost::none;
    }

private:
    boost::optional<std::string> code;
};

}  // namespace mongo
