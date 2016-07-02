/**
 *    Copyright (C) 2015 MongoDB Inc.
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

#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kNetwork

#include "mongo/platform/basic.h"

#include "mongo/client/mongo_uri.h"

#include "mongo/base/status_with.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/client/dbclientinterface.h"
#include "mongo/client/sasl_client_authenticate.h"
#include "mongo/util/mongoutils/str.h"
#include "mongo/util/password_digest.h"

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

const char kAuthMechMongoCR[] = "MONGODB-CR";
const char kAuthMechScramSha1[] = "SCRAM-SHA-1";
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

std::string authKeyCopyDBMongoCR(const std::string& username,
                                 const std::string& password,
                                 const std::string& nonce) {
    md5digest d;
    std::string passwordDigest = createPasswordDigest(username, password);
    {
        md5_state_t st;
        md5_init(&st);
        md5_append(&st, reinterpret_cast<const md5_byte_t*>(nonce.c_str()), nonce.size());
        md5_append(&st, reinterpret_cast<const md5_byte_t*>(username.data()), username.length());
        md5_append(&st,
                   reinterpret_cast<const md5_byte_t*>(passwordDigest.c_str()),
                   passwordDigest.size());
        md5_finish(&st, d);
    }
    return digestToString(d);
}

}  // namespace

BSONObj MongoURI::_makeAuthObjFromOptions(int maxWireVersion) const {
    BSONObjBuilder bob;

    // Add the username and optional password
    invariant(!_user.empty());
    std::string username(_user);  // may have to tack on service realm before we append

    if (!_password.empty())
        bob.append(saslCommandPasswordFieldName, _password);

    OptionsMap::const_iterator it;

    it = _options.find("authSource");
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
    } else if (maxWireVersion >= 3) {
        bob.append(saslCommandMechanismFieldName, kAuthMechScramSha1);
    } else {
        bob.append(saslCommandMechanismFieldName, kAuthMechMongoCR);
    }

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
            username.append("@").append(parsed[kAuthServiceRealm].String());
        }
    }

    it = _options.find("gssapiServiceName");
    if (it != _options.end()) {
        bob.append(saslCommandServiceNameFieldName, it->second);
    }

    bob.append("user", username);

    return bob.obj();
}

DBClientBase* MongoURI::connect(std::string& errmsg) const {
    double socketTimeout = 0.0;

    OptionsMap::const_iterator it = _options.find("socketTimeoutMS");
    if (it != _options.end()) {
        try {
            socketTimeout = std::stod(it->second);
        } catch (const std::exception& e) {
            uasserted(ErrorCodes::BadValue,
                      str::stream() << "Unable to parse socketTimeoutMS value" << causedBy(e));
        }
    }

    auto ret = _connectString.connect(errmsg, socketTimeout);
    if (!ret) {
        return ret;
    }

    if (!_user.empty()) {
        ret->auth(_makeAuthObjFromOptions(ret->getMaxWireVersion()));
    }
    return ret;
}

}  // namespace mongo
