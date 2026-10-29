// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/util/modules.h"

#include <string>
#include <string_view>

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

    void serializeToBSON(std::string_view fieldName, BSONObjBuilder* builder) const {
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
class [[MONGO_MOD_PRIVATE]] MapReduceJavascriptCodeOrNull {
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

    void serializeToBSON(std::string_view fieldName, BSONObjBuilder* builder) const {
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
