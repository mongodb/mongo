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

#include "mongo/platform/basic.h"

#include "mongo/config.h"
#include "mongo/util/net/asio_ssl_context.h"

#ifdef MONGO_CONFIG_SSL

#include "mongo/base/init.h"
#include "mongo/stdx/memory.h"

#include <asio.hpp>
#include <asio/ssl.hpp>

namespace mongo {

ASIOSSLContext::ASIOSSLContext()
    : _context(stdx::make_unique<asio::ssl::context>(asio::ssl::context::sslv23)),
      _mode(static_cast<SSLParams::SSLModes>(getSSLGlobalParams().sslMode.load())) {}

ASIOSSLContext::ASIOSSLContext(ASIOSSLContext&& other) = default;

ASIOSSLContext& ASIOSSLContext::operator=(ASIOSSLContext&& other) = default;

void ASIOSSLContext::init(SSLManagerInterface::ConnectionDirection direction) {
    if (_mode != SSLParams::SSLMode_disabled) {
        uassertStatusOK(getSSLManager()->initSSLContext(
            _context->native_handle(), getSSLGlobalParams(), direction));
    }
}

asio::ssl::context& ASIOSSLContext::getContext() {
    return *_context;
}

SSLParams::SSLModes ASIOSSLContext::sslMode() const {
    return _mode;
}

}  // namespace mongo

#endif  // MONGO_CONFIG_SSL
