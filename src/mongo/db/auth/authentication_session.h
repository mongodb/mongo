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

#include "mongo/db/auth/sasl_mechanism_registry.h"
#include "mongo/db/stats/counters.h"

namespace mongo {

class Client;

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

        AuthenticationSession* _session;
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
     * This returns true if the session currently believes itself to be a cluster member.
     */
    bool isClusterMember() const {
        if (_mech && _mech->isClusterMember()) {
            // If we're doing sasl and we have a mechanism, then we know.
            return true;
        }

        // Otherwise, rely on what the implementation has told us directly.
        return _isClusterMember;
    }

    /**
     * This returns true once either markFailed or markSuccessful is invoked.
     */
    bool isFinished() const {
        return _isFinished;
    }

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
     *
     * TODO(SERVER-52862) This should increment counters and log.
     */
    void markSuccessful();

    /**
     * Mark the session as unable to authenticate.
     *
     * TODO(SERVER-52862) This should increment counters and log.
     */
    void markFailed(const Status& status);

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
            session->markFailed(ex.toStatus());
            throw;
        } catch (...) {
            // Swallow other errors.
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

    Client* const _client;

    bool _isSpeculative = false;
    bool _isClusterMember = false;
    bool _isFinished = false;

    std::unique_ptr<ServerMechanismBase> _mech;
};

}  // namespace mongo
