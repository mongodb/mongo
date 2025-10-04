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

#include "mongo/client/authenticate.h"

#include "mongo/bson/bsonmisc.h"
#include "mongo/config.h"  // IWYU pragma: keep
#include "mongo/stdx/type_traits.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/duration.h"
#include "mongo/util/md5.h"
#include "mongo/util/net/hostandport.h"
#include "mongo/util/password_digest.h"

#include <queue>
#include <utility>

#include <boost/move/utility_core.hpp>

namespace {

using namespace mongo;

/**
 * Utility class to support tests in this file.  Allows caller to load
 * with pre-made responses and requests to interject into authentication methods.
 */
class AuthClientTest : public mongo::unittest::Test {
public:
    AuthClientTest()
        : _mockHost(),
          _millis(100),
          _username("PinkPanther"),
          _password("shhhhhhh"),
          _password_digest(createPasswordDigest(_username, _password)),
          _nonce("7ca422a24f326f2a"),
          _requests(),
          _responses() {
        _runCommandCallback = [this](OpMsgRequest request) {
            return runCommand(std::move(request));
        };

        // create our digest
        md5digest d;
        {
            md5_state_t st;
            md5_init_state(&st);
            md5_append(&st, (const md5_byte_t*)_nonce.c_str(), _nonce.size());
            md5_append(&st, (const md5_byte_t*)_username.c_str(), _username.size());
            md5_append(&st, (const md5_byte_t*)_password_digest.c_str(), _password_digest.size());
            md5_finish(&st, d);
        }
        _digest = digestToString(d);
    }

    // protected:
    Future<BSONObj> runCommand(OpMsgRequest request) {
        // Validate the received request
        ASSERT(!_requests.empty());
        auto& expected = _requests.front();
        ASSERT_EQ(expected.parseDbName(), request.parseDbName());
        ASSERT_BSONOBJ_EQ(expected.body, request.body);
        _requests.pop();

        // Then pop a response and call the handler
        ASSERT(!_responses.empty());
        auto ret = _responses.front();
        _responses.pop();
        return ret;
    }

    void reset() {
        // If there are things left then we did something wrong.
        ASSERT(_responses.empty());
        ASSERT(_requests.empty());
    }

    void pushResponse(const BSONObj& cmd) {
        _responses.emplace(cmd);
    }

    void pushRequest(const DatabaseName& dbname, const BSONObj& cmd) {
        _requests.emplace(
            OpMsgRequestBuilder::create(auth::ValidatedTenancyScope::kNotRequired, dbname, cmd));
    }

    BSONObj loadX509Conversation() {
        // 1. Client sends 'authenticate' command
        pushRequest(DatabaseName::kExternal,
                    BSON("authenticate" << 1 << "mechanism"
                                        << "MONGODB-X509"
                                        << "user" << _username));

        // 2. Client receives 'ok'
        pushResponse(BSON("ok" << 1));

        // Call clientAuthenticate()
        return BSON("mechanism" << "MONGODB-X509"
                                << "db"
                                << "$external"
                                << "user" << _username);
    }


    auth::RunCommandHook _runCommandCallback;

    // Auth code doesn't use HostAndPort information.
    HostAndPort _mockHost;
    Milliseconds _millis;

    // Some credentials
    std::string _username;
    std::string _password;
    std::string _password_digest;
    std::string _digest;
    std::string _nonce;

    std::queue<OpMsgRequest> _requests;
    std::queue<BSONObj> _responses;
};

#ifdef MONGO_CONFIG_SSL
TEST_F(AuthClientTest, X509) {
    auto params = loadX509Conversation();
    auth::authenticateClient(params, HostAndPort(), _username, _runCommandCallback).get();
}

TEST_F(AuthClientTest, asyncX509) {
    auto params = loadX509Conversation();
    ASSERT_OK(auth::authenticateClient(params, HostAndPort(), _username, _runCommandCallback)
                  .getNoThrow());
}
#endif

}  // namespace
