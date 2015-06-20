/*
 *    Copyright (C) 2012 10gen, Inc.
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

#include <cstdint>
#include <string>
#include <vector>

#include "mongo/base/disallow_copying.h"
#include "mongo/base/status.h"
#include "mongo/base/string_data.h"
#include "mongo/db/auth/authentication_session.h"
#include "mongo/db/auth/authorization_session.h"
#include "mongo/stdx/functional.h"

namespace mongo {

class AuthorizationSession;
class OperationContext;

/**
 * Authentication session data for the server side of SASL authentication.
 */
class SaslAuthenticationSession : public AuthenticationSession {
    MONGO_DISALLOW_COPYING(SaslAuthenticationSession);

public:
    typedef stdx::function<SaslAuthenticationSession*(AuthorizationSession*, const std::string&)>
        SaslAuthenticationSessionFactoryFn;
    static SaslAuthenticationSessionFactoryFn create;

    // Mechanism name constants.
    static const char mechanismCRAMMD5[];
    static const char mechanismDIGESTMD5[];
    static const char mechanismSCRAMSHA1[];
    static const char mechanismGSSAPI[];
    static const char mechanismPLAIN[];

    explicit SaslAuthenticationSession(AuthorizationSession* authSession);
    virtual ~SaslAuthenticationSession();

    /**
     * Start the server side of a SASL authentication.
     *
     * "authenticationDatabase" is the database against which the user is authenticating.
     * "mechanism" is the SASL mechanism to use.
     * "serviceName" is the SASL service name to use.
     * "serviceHostname" is the FQDN of this server.
     * "conversationId" is the conversation identifier to use for this session.
     *
     * If "autoAuthorize" is set to true, the server will automatically acquire all privileges
     * for a successfully authenticated user.  If it is false, the client will need to
     * explicilty acquire privileges on resources it wishes to access.
     *
     * Must be called only once on an instance.
     */
    virtual Status start(StringData authenticationDatabase,
                         StringData mechanism,
                         StringData serviceName,
                         StringData serviceHostname,
                         int64_t conversationId,
                         bool autoAuthorize) = 0;

    /**
     * Perform one step of the server side of the authentication session,
     * consuming "inputData" and producing "*outputData".
     *
     * A return of Status::OK() indiciates succesful progress towards authentication.
     * Any other return code indicates that authentication has failed.
     *
     * Must not be called before start().
     */
    virtual Status step(StringData inputData, std::string* outputData) = 0;

    /**
     * Returns the the operation context associated with the currently executing command.
     * Authentication commands must set this on their associated
     * SaslAuthenticationSession.
     */
    OperationContext* getOpCtxt() const {
        return _txn;
    }
    void setOpCtxt(OperationContext* txn) {
        _txn = txn;
    }

    /**
     * Gets the name of the database against which this authentication conversation is running.
     *
     * Not meaningful before a successful call to start().
     */
    StringData getAuthenticationDatabase() const;

    /**
     * Get the conversation id for this authentication session.
     *
     * Must not be called before start().
     */
    int64_t getConversationId() const {
        return _conversationId;
    }

    /**
     * If the last call to step() returned Status::OK(), this method returns true if the
     * authentication conversation has completed, from the server's perspective.  If it returns
     * false, the server expects more input from the client.  If the last call to step() did not
     * return Status::OK(), returns true.
     *
     * Behavior is undefined if step() has not been called.
     */
    bool isDone() const {
        return _done;
    }

    /**
     * Gets the string identifier of the principal being authenticated.
     *
     * Returns the empty string if the session does not yet know the identity being
     * authenticated.
     */
    virtual std::string getPrincipalId() const = 0;

    /**
     * Gets the name of the SASL mechanism in use.
     *
     * Returns "" if start() has not been called or if start() did not return Status::OK().
     */
    virtual const char* getMechanism() const = 0;

    /**
     * Returns true if automatic privilege acquisition should be used for this principal, after
     * authentication.  Not meaningful before a successful call to start().
     */
    bool shouldAutoAuthorize() const {
        return _autoAuthorize;
    }

    AuthorizationSession* getAuthorizationSession() {
        return _authzSession;
    }

protected:
    OperationContext* _txn;
    AuthorizationSession* _authzSession;
    std::string _authenticationDatabase;
    std::string _serviceName;
    std::string _serviceHostname;
    int _saslStep;
    int64_t _conversationId;
    bool _autoAuthorize;
    bool _done;
};

}  // namespace mongo
