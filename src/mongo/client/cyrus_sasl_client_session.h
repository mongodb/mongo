/*    Copyright 2014 MongoDB Inc.
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
 *    must comply with the GNU Affero General Public License in all respects
 *    for all of the code used other than as permitted herein. If you modify
 *    file(s) with this exception, you may extend this exception to your
 *    version of the file(s), but you are not obligated to do so. If you do not
 *    wish to do so, delete this exception statement from your version. If you
 *    delete this exception statement from all source files in the program,
 *    then also delete it in the license file.
 */


#include "mongo/client/sasl_client_session.h"

#include <sasl/sasl.h>

namespace mongo {

/**
 * Implementation of the client side of a SASL authentication conversation.
 * using the Cyrus SASL library.
 */
class CyrusSaslClientSession : public SaslClientSession {
    MONGO_DISALLOW_COPYING(CyrusSaslClientSession);

public:
    CyrusSaslClientSession();
    ~CyrusSaslClientSession();

    /**
     * Overriding to store the password data in sasl_secret_t format
     */
    virtual void setParameter(Parameter id, StringData value);

    /**
     * Returns the value of the parameterPassword parameter in the form of a sasl_secret_t, used
     * by the Cyrus SASL library's SASL_CB_PASS callback.  The session object owns the storage
     * referenced by the returned sasl_secret_t*, which will remain in scope according to the
     * same rules as given for SaslClientSession::getParameter().
     */
    sasl_secret_t* getPasswordAsSecret();

    virtual Status initialize();

    virtual Status step(StringData inputData, std::string* outputData);

    virtual bool isDone() const {
        return _done;
    }

private:
    /// Maximum number of Cyrus SASL callbacks stored in _callbacks.
    static const int maxCallbacks = 4;

    /// Underlying Cyrus SASL library connection object.
    sasl_conn_t* _saslConnection;

    // Number of successfully completed conversation steps.
    int _step;

    /// See isDone().
    bool _done;

    /// Stored of password in sasl_secret_t format
    std::unique_ptr<char[]> _secret;

    /// Callbacks registered on _saslConnection for providing the Cyrus SASL library with
    /// parameter values, etc.
    sasl_callback_t _callbacks[maxCallbacks];
};

}  // namespace mongo
