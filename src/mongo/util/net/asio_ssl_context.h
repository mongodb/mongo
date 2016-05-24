/**
 * Copyright (C) 2016 MongoDB Inc.
 *
 * This program is free software: you can redistribute it and/or  modify
 * it under the terms of the GNU Affero General Public License, version 3,
 * as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Affero General Public License for more details.
 *
 * You should have received a copy of the GNU Affero General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 * As a special exception, the copyright holders give permission to link the
 * code of portions of this program with the OpenSSL library under certain
 * conditions as described in each individual source file and distribute
 * linked combinations including the program with the OpenSSL library. You
 * must comply with the GNU Affero General Public License in all respects
 * for all of the code used other than as permitted herein. If you modify
 * file(s) with this exception, you may extend this exception to your
 * version of the file(s), but you are not obligated to do so. If you do not
 * wish to do so, delete this exception statement from your version. If you
 * delete this exception statement from all source files in the program,
 * then also delete it in the license file.
 */

#pragma once

#include "mongo/base/disallow_copying.h"
#include "mongo/config.h"
#include "mongo/util/net/ssl_manager.h"
#include "mongo/util/net/ssl_options.h"

#ifdef MONGO_CONFIG_SSL
namespace asio {
namespace ssl {
class context;
}  // namespace ssl
}  // namespace asio

namespace mongo {

class ASIOSSLContext {
public:
    MONGO_DISALLOW_COPYING(ASIOSSLContext);

    /**
     * A class to house the ASIO SSL context as well as the mongo SSL mode. This will be decorated
     * on to the SSLManager.
     */
    ASIOSSLContext();

    ASIOSSLContext(ASIOSSLContext&& other);
    ASIOSSLContext& operator=(ASIOSSLContext&& other);

    /**
     * This must be called before calling `getContext()`. This does all of the setup that requires
     * the SSLManager (which can't be done in construction due to this class being a decoration).
     */
    void init(SSLManagerInterface::ConnectionDirection direction);

    /**
     * A copy of the ASIO SSL context generated upon construction from the mongo::SSLParams.
     */
    asio::ssl::context& getContext();

    /**
     * The SSL operation mode. See enum SSLModes in ssl_options.h
     */
    SSLParams::SSLModes sslMode() const;

private:
    std::unique_ptr<asio::ssl::context> _context;
    SSLParams::SSLModes _mode;
};
}  // namespace mongo
#else
namespace mongo {

// This is a dummy class for when we build without SSL.
class ASIOSSLContext {};
}  // namespace mongo
#endif  // MONGO_CONFIG_SSL
