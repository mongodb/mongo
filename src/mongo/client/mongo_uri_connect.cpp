// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/base/error_codes.h"
#include "mongo/base/status.h"
#include "mongo/base/status_with.h"
#include "mongo/client/client_api_version_parameters_gen.h"
#include "mongo/client/connection_string.h"
#include "mongo/client/dbclient_base.h"
#include "mongo/client/mongo_uri.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/str.h"

#include <exception>
#include <map>
#include <memory>
#include <string>
#include <string_view>
#include <utility>

#include <boost/move/utility_core.hpp>
#include <boost/optional/optional.hpp>

namespace mongo {

DBClientBase* MongoURI::connect(std::string_view applicationName,
                                std::string& errmsg,
                                boost::optional<double> socketTimeoutSecs,
                                const ClientAPIVersionParameters* apiParameters,
                                const boost::optional<TransientSSLParams>& transientSSLParams,
                                ErrorCodes::Error* errcode) const {
    OptionsMap::const_iterator it = _options.find("socketTimeoutMS");
    if (it != _options.end() && !socketTimeoutSecs) {
        try {
            socketTimeoutSecs = std::stod(it->second) / 1000;
        } catch (const std::exception& e) {
            uasserted(ErrorCodes::BadValue,
                      str::stream() << "Unable to parse socketTimeoutMS value" << causedBy(e));
        }
    }

    auto swConn = _connectString.connect(applicationName,
                                         socketTimeoutSecs.value_or(0.0),
                                         this,
                                         apiParameters,
                                         transientSSLParams ? &*transientSSLParams : nullptr);
    if (!swConn.isOK()) {
        errmsg = swConn.getStatus().reason();
        if (errcode) {
            *errcode = swConn.getStatus().code();
        }
        return nullptr;
    }

    if (!getSetName().empty()) {
        // When performing initial topology discovery, don't bother authenticating
        // since we will be immediately restarting our connect loop to a single node.
        return swConn.getValue().release();
    }

    auto connection = std::move(swConn.getValue());
    if (!connection->authenticatedDuringConnect()) {
        auto optAuthObj = makeAuthObjFromOptions(connection->getMaxWireVersion(),
                                                 connection->getIsPrimarySaslMechanisms());
        if (optAuthObj) {
            connection->auth(optAuthObj.value());
        }
    }

    return connection.release();
}

}  // namespace mongo
