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

#include "mongo/base/error_codes.h"
#include "mongo/base/status.h"
#include "mongo/base/string_data.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/auth/sasl_mechanism_registry.h"
#include "mongo/db/auth/user_name.h"
#include "mongo/db/client.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/stats/counters.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/timer.h"

#include <memory>
#include <string>
#include <utility>

#include <boost/move/utility_core.hpp>
#include <boost/optional.hpp>
#include <boost/optional/optional.hpp>

namespace mongo {

class Client;

class AuthMetricsRecorder {
public:
    void restart();
    BSONObj capture();
    void appendMetric(const BSONObj& metric);

private:
    Timer _timer;
    BSONArrayBuilder _appendedMetrics;
};

/**
 * Type representing an ongoing authentication session.
 */
class AuthenticationSession {
    AuthenticationSession(const AuthenticationSession&) = delete;
    AuthenticationSession& operator=(const AuthenticationSession&) = delete;

public:
    /**
     * This enum enumerates the various steps that need access to the AuthenticationSession.
     */
    enum class StepType {
        kSaslSupportedMechanisms,
        kSaslStart,
        kSaslContinue,
        kAuthenticate,
        kSpeculativeSaslStart,
        kSpeculativeAuthenticate,
    };

    AuthenticationSession(Client* client) : _client(client) {}

    /**
     * This guard creates and destroys the session as appropriate for the currentStep.
     */
    class StepGuard {
    public:
        StepGuard(OperationContext* opCtx, StepType currentStep);
        ~StepGuard();

        AuthenticationSession* getSession() {
            return _session;
        }

    private:
        OperationContext* const _opCtx;
        const StepType _currentStep;

        AuthenticationSession* _session = nullptr;
    };

    /**
     * Gets the authentication session for the given "client".
     *
     * This function always returns a valid pointer.
     */
    static AuthenticationSession* get(Client* client);
    static AuthenticationSession* get(OperationContext* opCtx) {
        return get(opCtx->getClient());
    }

    /**
     * Return an identifer of the type of session, so that a caller can safely cast it and
     * extract the type-specific data stored within.
     *
     * If a mechanism has not already been set, this may return nullptr.
     */
    ServerMechanismBase* getMechanism() const {
        return _mech.get();
    }

    /**
     * This returns true if the session started with StepType::kSpeculativeSaslStart or
     * StepType::kSpeculativeAuthenticate.
     */
    bool isSpeculative() const {
        return _isSpeculative;
    }

    /**
     * This returns the mechanism name for this session.
     */
    StringData getMechanismName() const {
        return _mechName;
    }

    /**
     * This returns the user portion of the UserName which may be an empty StringData.
     */
    StringData getUserName() const {
        return _userName.getUser();
    }

    /**
     * This returns the database portion of the UserName which may be an empty StringData.
     */
    StringData getDatabase() const {
        return _userName.getDB();
    }

    /**
     * This returns the last processed step of this session.
     */
    boost::optional<StepType> getLastStep() const {
        return _lastStep;
    }

    /**
     * Set the mechanism name for this session.
     *
     * If the mechanism name is not recognized, this will throw.
     */
    void setMechanismName(StringData mechanismName);

    /**
     * Update the database for this session.
     *
     * The database will be validated against the current database for this session.
     */
    void updateDatabase(StringData database, bool isMechX509) {
        updateUserName(UserName("", std::string{database}), isMechX509);
    }

    /**
     * Update the user name for this session.
     *
     * The user name will be validated against the current user name for this session.
     */
    void updateUserName(UserName userName, bool isMechX509);

    /**
     * Set the last user name used with `saslSupportedMechs` for this session.
     *
     * This user name does no emit an exception when it conflicts, but it does create an audit
     * entry.
     */
    void setUserNameForSaslSupportedMechanisms(UserName userName);

    /**
     * Mark the session as a cluster member.
     *
     * This is used for x509 authentication since it lacks a mechanism in the traditional sense.
     */
    void setAsClusterMember();

    /**
     * Set the mechanism for the session.
     *
     * This function is only valid to invoke when there is no current mechanism.
     */
    void setMechanism(std::unique_ptr<ServerMechanismBase> mech, boost::optional<BSONObj> options);

    /**
     * Mark the session as succssfully authenticated.
     */
    void markSuccessful();

    /**
     * Mark the session as unable to authenticate.
     */
    void markFailed(const Status& status);

    /**
     * Returns the metrics recorder for this Authentication Session.
     * The session retains ownership of this pointer.
     */
    AuthMetricsRecorder* metrics() {
        return &_metricsRecorder;
    }

    /**
     * This function invokes a functor with a StepGuard on the stack and observes any exceptions
     * emitted.
     */
    template <typename F>
    static auto doStep(OperationContext* opCtx, StepType state, F&& f) {
        auto guard = StepGuard(opCtx, state);
        auto session = guard.getSession();

        try {
            return std::forward<F>(f)(session);
        } catch (const DBException& ex) {
            bool specAuthFailed = ex.toStatus().code() == ErrorCodes::Error::AuthenticationFailed &&
                (state == StepType::kSpeculativeAuthenticate ||
                 state == StepType::kSpeculativeSaslStart);
            // If speculative authentication failed, then we do not want to mark the session as
            // failed in order to allow the session to persist into another authentication
            // attempt. If we ran into an exception for another reason, mark the session as failed.
            if (!specAuthFailed) {
                session->markFailed(ex.toStatus());
            }
            throw;
        } catch (...) {
            session->markFailed(
                Status(ErrorCodes::InternalError, "Encountered an unhandleable error"));
            throw;
        }
    }

    /**
     * Convert a StepType to a constant string.
     */
    friend constexpr StringData toString(StepType step) {
        switch (step) {
            case StepType::kSaslSupportedMechanisms:
                return "SaslSupportedMechanisms"_sd;
            case StepType::kSaslStart:
                return "SaslStart"_sd;
            case StepType::kSaslContinue:
                return "SaslContinue"_sd;
            case StepType::kAuthenticate:
                return "Authenticate"_sd;
            case StepType::kSpeculativeSaslStart:
                return "SpeculativeSaslStart"_sd;
            case StepType::kSpeculativeAuthenticate:
                return "SpeculativeAuthenticate"_sd;
        }

        return "Unknown"_sd;
    }

private:
    static boost::optional<AuthenticationSession>& _get(Client* client);

    void _finish();
    void _verifyUserNameFromSaslSupportedMechanisms(const UserName& user, bool isMechX509);

    Client* const _client;

    boost::optional<StepType> _lastStep;

    bool _isSpeculative = false;
    bool _isClusterMember = false;
    bool _isFinished = false;

    // The user name can be provided in advance by saslSupportedMechs.
    UserName _ssmUserName;

    std::string _mechName;
    boost::optional<AuthCounter::MechanismCounterHandle> _mechCounter;

    // The user name can be provided partially by the command namespace or in full by a client
    // certificate. If we have a authN mechanism, we use its principal name instead.
    UserName _userName;
    std::unique_ptr<ServerMechanismBase> _mech;
    AuthMetricsRecorder _metricsRecorder;
};

}  // namespace mongo
