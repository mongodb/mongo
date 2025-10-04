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

#include "mongo/base/string_data.h"

#include <string>

namespace mongo {

class SaslClientSession;
template <typename T>
class StatusWith;

/**
 * Abstract class for implementing the clent-side
 * of a SASL mechanism conversation.
 */
class SaslClientConversation {
    SaslClientConversation(const SaslClientConversation&) = delete;
    SaslClientConversation& operator=(const SaslClientConversation&) = delete;

public:
    /**
     * Implements the client side of a SASL authentication mechanism.
     *
     * "saslClientSession" is the corresponding SASLClientSession.
     * "saslClientSession" must stay in scope until the SaslClientConversation's
     *  destructor completes.
     *
     **/
    explicit SaslClientConversation(SaslClientSession* saslClientSession)
        : _saslClientSession(saslClientSession) {}

    virtual ~SaslClientConversation();

    /**
     * Performs one step of the client side of the authentication session,
     * consuming "inputData" and producing "*outputData".
     *
     * A return of Status::OK() indicates successful progress towards authentication.
     * A return of !Status::OK() indicates failed authentication
     *
     * A return of true means that the authentication process has finished.
     * A return of false means that the authentication process has more steps.
     *
     */
    virtual StatusWith<bool> step(StringData inputData, std::string* outputData) = 0;

protected:
    SaslClientSession* _saslClientSession;
};

}  // namespace mongo
