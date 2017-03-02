/**
 *    Copyright (C) 2015 BongoDB Inc.
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

#include "bongo/platform/basic.h"

#include "bongo/executor/async_secure_stream_factory.h"

#include "bongo/config.h"
#include "bongo/executor/async_secure_stream.h"
#include "bongo/executor/async_stream.h"
#include "bongo/stdx/memory.h"
#include "bongo/util/net/ssl_manager.h"
#include "bongo/util/net/ssl_options.h"

#ifdef BONGO_CONFIG_SSL

namespace bongo {
namespace executor {

AsyncSecureStreamFactory::AsyncSecureStreamFactory(SSLManagerInterface* sslManager)
    : _sslContext(asio::ssl::context::sslv23) {
    // We use sslv23, which corresponds to OpenSSLs SSLv23_method, for compatibility with older
    // versions of OpenSSL. This mirrors the call to SSL_CTX_new in ssl_manager.cpp. In
    // initAsyncSSLContext we explicitly disable all protocols other than TLSv1, TLSv1.1,
    // and TLSv1.2.
    uassertStatusOK(
        sslManager->initSSLContext(_sslContext.native_handle(),
                                   getSSLGlobalParams(),
                                   SSLManagerInterface::ConnectionDirection::kOutgoing));
}

std::unique_ptr<AsyncStreamInterface> AsyncSecureStreamFactory::makeStream(
    asio::io_service::strand* strand, const HostAndPort&) {
    int sslModeVal = getSSLGlobalParams().sslMode.load();
    if (sslModeVal == SSLParams::SSLMode_preferSSL || sslModeVal == SSLParams::SSLMode_requireSSL) {
        return stdx::make_unique<AsyncSecureStream>(strand, &_sslContext);
    }
    return stdx::make_unique<AsyncStream>(strand);
}

}  // namespace executor
}  // namespace bongo

#endif
