/**
 *    Copyright (C) 2025-present MongoDB, Inc.
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

#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"

#include <string>

namespace mongo {
class RawOpDocument {
public:
    explicit RawOpDocument();
    explicit RawOpDocument(const std::string& opType, const BSONObj& body);
    BSONObj getDocument() const;
    void updateBody(const BSONObj& newBody);
    void updateHeaderField(const std::string& fieldName, int value);
    void updateOpType(const std::string& newOpType);
    void updateSessionId(int64_t id);
    void updateSeenField(const Date_t& time, int64_t nanoseconds = 0);
    void updateEvent(int event);

private:
    /** Represents the `rawop` part. */
    BSONObj _rawOp;
    /** Represents the entire document. */
    BSONObj _document;

    template <typename T>
    void updateField(BSONObj& originalDocument, const std::string& fieldName, const T& newValue) {
        auto obj = BSON(fieldName << newValue);
        auto elem = obj.firstElement();
        originalDocument = originalDocument.addField(elem);
    }

    /*
     * This is the main method that allows any bson command to go inside a recorded packet,
     * simulating a recorded messaged that respects the mongodb binary protocol.
     */
    std::vector<char> constructWireProtocolBinData(const BSONObj& command) const;
};

}  // namespace mongo
