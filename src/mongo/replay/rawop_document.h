// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0
#pragma once

#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/util/modules.h"

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
