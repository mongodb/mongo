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
#include "mongo/replay/rawop_document.h"

#include "mongo/util/time_support.h"

namespace mongo {

RawOpDocument::RawOpDocument() {
    // Constructs a fake recorded packet
    BSONObjBuilder rawOpBuilder;
    rawOpBuilder.append("header",
                        BSON("messagelength" << 42 << "requestid" << 3 << "responseto" << 0
                                             << "opcode"
                                             << 2013));  // 2013 means OP_MSG. it is important

    // The body must encode the bson command itself but also some information for the command itself
    // see updateBody method.
    // The most important part is to have well formed body for testing purposes.
    rawOpBuilder.appendBinData("body", 0, mongo::BinDataGeneral, "");
    _rawOp = rawOpBuilder.obj();

    // Initialize other fields.
    BSONObjBuilder rootBuilder;
    rootBuilder.append("rawop", _rawOp);
    rootBuilder.append("seen", BSON("sec" << 63884568370ll << "nsec" << 87));
    rootBuilder.append("session", "{ remote: \"127.0.0.1:54420\", local: \"127.0.0.1:27017\" }");
    rootBuilder.append("order", 87);
    rootBuilder.append("seenconnectionnum", int64_t{16});
    rootBuilder.append("playedconnectionnum", 0);
    rootBuilder.append("generation", 0);
    rootBuilder.append("opType", "fake");

    // Build the main BSON object.
    _document = rootBuilder.obj();
}

RawOpDocument::RawOpDocument(const std::string& opType, const BSONObj& body) : RawOpDocument() {
    updateBody(body);
    updateOpType(opType);
}

BSONObj RawOpDocument::getDocument() const {
    return _document;
}

void RawOpDocument::updateBody(const BSONObj& newBody) {
    BSONObjBuilder rawOpBuilder;
    rawOpBuilder.append("header", _rawOp.getObjectField("header"));
    const auto binData = constructWireProtocolBinData(newBody);
    rawOpBuilder.appendBinData("body", binData.size(), BinDataGeneral, binData.data());
    _rawOp = rawOpBuilder.obj();
    updateField(_document, "rawop", _rawOp);
}

void RawOpDocument::updateHeaderField(const std::string& fieldName, int value) {
    BSONObj header = _rawOp.getObjectField("header");
    BSONObjBuilder updatedHeaderBuilder;
    for (auto& elem : header) {
        if (elem.fieldName() == fieldName) {
            updatedHeaderBuilder.append(fieldName, value);
        } else {
            updatedHeaderBuilder.append(elem);
        }
    }

    // Rebuild the rawOp structure.
    BSONObjBuilder rawOpBuilder;
    rawOpBuilder.append("header", updatedHeaderBuilder.obj());
    int binDataLen = 0;
    const auto binData = _rawOp.getField("body").binData(binDataLen);
    rawOpBuilder.appendBinData("body", binDataLen, BinDataGeneral, binData);
    _rawOp = rawOpBuilder.obj();

    updateField(_document, "rawop", _rawOp);
}

void RawOpDocument::updateSessionId(int64_t id) {
    updateField(_document, "seenconnectionnum", id);
}

void RawOpDocument::updateOpType(const std::string& newOpType) {
    updateField(_document, "opType", newOpType);
}

void RawOpDocument::updateSeenField(const Date_t& time, int64_t nanoseconds /* = 0 */) {
    // total number of seconds
    constexpr long long unixToInternal =
        62135596800LL;  // mongodb date are stored using a ref to start of epoch
    int64_t seconds = time.toMillisSinceEpoch() + unixToInternal;
    BSONObj updatedSeen = BSON("sec" << seconds << "nsec" << nanoseconds);
    updateField(_document, "seen", updatedSeen);
}

std::vector<char> RawOpDocument::constructWireProtocolBinData(const BSONObj& command) const {

    size_t headerSize = 16;                  // Fixed size for header.
    size_t flagBitsSize = 4;                 // Fixed size for flag bits.
    size_t sectionKindSize = 1;              // Section kind byte size.
    size_t payloadSize = command.objsize();  // Size of the BSON payload.
    size_t messageLength = headerSize + flagBitsSize + sectionKindSize + payloadSize;

    std::vector<char> wireProtocolMessage;

    // Header.
    wireProtocolMessage.push_back((char)(messageLength & 0xFF));
    wireProtocolMessage.push_back((char)((messageLength >> 8) & 0xFF));
    wireProtocolMessage.push_back((char)((messageLength >> 16) & 0xFF));
    wireProtocolMessage.push_back((char)((messageLength >> 24) & 0xFF));

    // Example requestId, responseTo, opCode (arbitrary for testing).
    int requestID = 3;
    int responseTo = 0;
    int opCode = 2013;

    wireProtocolMessage.push_back((char)(requestID & 0xFF));
    wireProtocolMessage.push_back((char)((requestID >> 8) & 0xFF));
    wireProtocolMessage.push_back((char)((requestID >> 16) & 0xFF));
    wireProtocolMessage.push_back((char)((requestID >> 24) & 0xFF));

    wireProtocolMessage.push_back((char)(responseTo & 0xFF));
    wireProtocolMessage.push_back((char)((responseTo >> 8) & 0xFF));
    wireProtocolMessage.push_back((char)((responseTo >> 16) & 0xFF));
    wireProtocolMessage.push_back((char)((responseTo >> 24) & 0xFF));

    wireProtocolMessage.push_back((char)(opCode & 0xFF));
    wireProtocolMessage.push_back((char)((opCode >> 8) & 0xFF));
    wireProtocolMessage.push_back((char)((opCode >> 16) & 0xFF));
    wireProtocolMessage.push_back((char)((opCode >> 24) & 0xFF));

    // Add flag bits (0 for normal message).
    wireProtocolMessage.push_back(0x00);
    wireProtocolMessage.push_back(0x00);
    wireProtocolMessage.push_back(0x00);
    wireProtocolMessage.push_back(0x00);

    // Section kind (0 for document payload).
    wireProtocolMessage.push_back((char)0x00);

    // Append serialized BSON payload.
    const char* serializedData = command.objdata();
    wireProtocolMessage.insert(
        wireProtocolMessage.end(), serializedData, serializedData + payloadSize);
    return wireProtocolMessage;
}

}  // namespace mongo
