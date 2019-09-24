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

#include "mongo/platform/basic.h"

#include "mongo/client/mongo_uri.h"

#include "mongo/base/status_with.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/client/authenticate.h"
#include "mongo/client/dbclient_base.h"
#include "mongo/db/auth/sasl_command_constants.h"
#include "mongo/util/password_digest.h"
#include "mongo/util/str.h"

#include <boost/algorithm/string/case_conv.hpp>
#include <boost/algorithm/string/classification.hpp>
#include <boost/algorithm/string/predicate.hpp>
#include <boost/algorithm/string/split.hpp>

#include <iterator>

namespace mongo {

namespace {
const char kAuthMechanismPropertiesKey[] = "mechanism_properties";

// CANONICALIZE_HOST_NAME is currently unsupported
const char kAuthServiceName[] = "SERVICE_NAME";
const char kAuthServiceRealm[] = "SERVICE_REALM";

const char kAuthMechDefault[] = "DEFAULT";

const char* const kSupportedAuthMechanismProperties[] = {kAuthServiceName, kAuthServiceRealm};

BSONObj parseAuthMechanismProperties(const std::string& propStr) {
    BSONObjBuilder bob;
    std::vector<std::string> props;
    boost::algorithm::split(props, propStr, boost::algorithm::is_any_of(",:"));
    for (std::vector<std::string>::const_iterator it = props.begin(); it != props.end(); ++it) {
        std::string prop((boost::algorithm::to_upper_copy(*it)));  // normalize case
        uassert(ErrorCodes::FailedToParse,
                str::stream() << "authMechanismProperty: " << *it << " is not supported",
                std::count(kSupportedAuthMechanismProperties,
                           std::end(kSupportedAuthMechanismProperties),
                           prop));
        ++it;
        uassert(ErrorCodes::FailedToParse,
                str::stream() << "authMechanismProperty: " << prop << " must have a value",
                it != props.end());
        bob.append(prop, *it);
    }
    return bob.obj();
}

}  // namespace

boost::optional<BSONObj> MongoURI::_makeAuthObjFromOptions(
    int maxWireVersion, const std::vector<std::string>& saslMechsForAuth) const {
    // Usually, a username is required to authenticate.
    // However X509 based authentication may, and typically does,
    // omit the username, inferring it from the client certificate instead.
    bool usernameRequired = true;

    BSONObjBuilder bob;
    if (!_password.empty()) {
        bob.append(saslCommandPasswordFieldName, _password);
    }

    auto it = _options.find("authSource");
    if (it != _options.end()) {
        bob.append(saslCommandUserDBFieldName, it->second);
    } else if (!_database.empty()) {
        bob.append(saslCommandUserDBFieldName, _database);
    } else {
        bob.append(saslCommandUserDBFieldName, "admin");
    }

    it = _options.find("authMechanism");
    if (it != _options.end()) {
        bob.append(saslCommandMechanismFieldName, it->second);
        if (it->second == auth::kMechanismMongoX509) {
            usernameRequired = false;
        }
    } else if (!saslMechsForAuth.empty()) {
        if (std::find(saslMechsForAuth.begin(),
                      saslMechsForAuth.end(),
                      auth::kMechanismScramSha256) != saslMechsForAuth.end()) {
            bob.append(saslCommandMechanismFieldName, auth::kMechanismScramSha256);
        } else {
            bob.append(saslCommandMechanismFieldName, auth::kMechanismScramSha1);
        }
    } else if (maxWireVersion >= 3) {
        bob.append(saslCommandMechanismFieldName, auth::kMechanismScramSha1);
    } else {
        bob.append(saslCommandMechanismFieldName, auth::kMechanismMongoCR);
    }

    if (usernameRequired && _user.empty()) {
        return boost::none;
    }

    std::string username(_user);  // may have to tack on service realm before we append

    it = _options.find("authMechanismProperties");
    if (it != _options.end()) {
        BSONObj parsed(parseAuthMechanismProperties(it->second));

        bool hasNameProp = parsed.hasField(kAuthServiceName);
        bool hasRealmProp = parsed.hasField(kAuthServiceRealm);

        uassert(ErrorCodes::FailedToParse,
                "Cannot specify both gssapiServiceName and SERVICE_NAME",
                !(hasNameProp && _options.count("gssapiServiceName")));
        // we append the parsed object so that mechanisms that don't accept it can assert.
        bob.append(kAuthMechanismPropertiesKey, parsed);
        // we still append using the old way the SASL code expects it
        if (hasNameProp) {
            bob.append(saslCommandServiceNameFieldName, parsed[kAuthServiceName].String());
        }
        // if we specified a realm, we just append it to the username as the SASL code
        // expects it that way.
        if (hasRealmProp) {
            if (username.empty()) {
                // In practice, this won't actually occur since
                // this block corresponds to GSSAPI, while username
                // may only be omitted with MOGNODB-X509.
                return boost::none;
            }
            username.append("@").append(parsed[kAuthServiceRealm].String());
        }
    }

    it = _options.find("gssapiServiceName");
    if (it != _options.end()) {
        bob.append(saslCommandServiceNameFieldName, it->second);
    }

    if (!username.empty()) {
        bob.append("user", username);
    }

    return bob.obj();
}

DBClientBase* MongoURI::connect(StringData applicationName,
                                std::string& errmsg,
                                boost::optional<double> socketTimeoutSecs) const {
    OptionsMap::const_iterator it = _options.find("socketTimeoutMS");
    if (it != _options.end() && !socketTimeoutSecs) {
        try {
            socketTimeoutSecs = std::stod(it->second) / 1000;
        } catch (const std::exception& e) {
            uasserted(ErrorCodes::BadValue,
                      str::stream() << "Unable to parse socketTimeoutMS value" << causedBy(e));
        }
    }

    auto ret = std::unique_ptr<DBClientBase>(
        _connectString.connect(applicationName, errmsg, socketTimeoutSecs.value_or(0.0), this));
    if (!ret) {
        return nullptr;
    }

    if (!getSetName().empty()) {
        // When performing initial topology discovery, don't bother authenticating
        // since we will be immediately restarting our connect loop to a single node.
        return ret.release();
    }

    auto optAuthObj =
        _makeAuthObjFromOptions(ret->getMaxWireVersion(), ret->getIsMasterSaslMechanisms());
    if (optAuthObj) {
        ret->auth(optAuthObj.get());
    }

    return ret.release();
}

}  // namespace mongo
