/*
 *    Copyright (C) 2014 MongoDB Inc.
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

#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kAccessControl

#include "mongo/platform/basic.h"

#include "mongo/db/auth/internal_user_auth.h"

#include "mongo/bson/mutable/document.h"
#include "mongo/bson/mutable/element.h"
#include "mongo/client/dbclientinterface.h"
#include "mongo/db/server_options.h"
#include "mongo/util/log.h"

namespace mongo {
    namespace mmb = mongo::mutablebson;

    // not guarded by the authParams mutex never changed in
    // multi-threaded operation
    static bool authParamsSet = false;

    // Store default authentication parameters for internal authentication to cluster members,
    // guarded by the authParams mutex
    static BSONObj authParams;

    static boost::mutex authParamMutex;

    bool isInternalAuthSet() {
       return authParamsSet;
    }

    void setInternalUserAuthParams(const BSONObj& authParamsIn) {
        if (!isInternalAuthSet()) {
            authParamsSet = true;
        }
        boost::mutex::scoped_lock lk(authParamMutex);

        if (authParamsIn["mechanism"].String() != "SCRAM-SHA-1") {
            authParams = authParamsIn.copy();
            return;
        }

        // Create authParams for legacy MONGODB-CR authentication for 2.6/2.8 mixed
        // mode if applicable.
        mmb::Document fallback(authParamsIn);
        fallback.root().findFirstChildNamed("mechanism").setValueString("MONGODB-CR");

        mmb::Document doc(authParamsIn);
        mmb::Element fallbackEl = doc.makeElementObject("fallbackParams");
        fallbackEl.setValueObject(fallback.getObject());
        doc.root().pushBack(fallbackEl);
        authParams = doc.getObject().copy();
    }

    BSONObj getInternalUserAuthParamsWithFallback() {
        if (!authParamsSet) {
            return BSONObj();
        }

        boost::mutex::scoped_lock lk(authParamMutex);
        return authParams.copy();
    }

    BSONObj getFallbackAuthParams(BSONObj params) {
        if (params["fallbackParams"].type() != Object) {
            return BSONObj();
        }
        return params["fallbackParams"].Obj();
    }

    bool authenticateInternalUser(DBClientWithCommands* conn) {
        if (!isInternalAuthSet()) {
            if (!serverGlobalParams.quiet) {
                log() << "ERROR: No authentication parameters set for internal user";
            }
            return false;
        }

        try {
            conn->auth(getInternalUserAuthParamsWithFallback());
            return true;
        } catch(const UserException& ex) {
            if (!serverGlobalParams.quiet) {
                log() << "can't authenticate to " << conn->toString()
                      << " as internal user, error: "<< ex.what();
            }
            return false;
        }
    }
} // namespace mongo
