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

#include "bongo/executor/network_interface_factory.h"

#include "bongo/base/init.h"
#include "bongo/base/status.h"
#include "bongo/config.h"
#include "bongo/db/server_parameters.h"
#include "bongo/executor/async_secure_stream_factory.h"
#include "bongo/executor/async_stream_factory.h"
#include "bongo/executor/async_stream_interface.h"
#include "bongo/executor/async_timer_asio.h"
#include "bongo/executor/connection_pool.h"
#include "bongo/executor/network_connection_hook.h"
#include "bongo/executor/network_interface_asio.h"
#include "bongo/rpc/metadata/metadata_hook.h"
#include "bongo/stdx/memory.h"
#include "bongo/util/net/ssl_manager.h"

namespace bongo {
namespace executor {

std::unique_ptr<NetworkInterface> makeNetworkInterface(std::string instanceName) {
    return makeNetworkInterface(std::move(instanceName), nullptr, nullptr);
}

std::unique_ptr<NetworkInterface> makeNetworkInterface(
    std::string instanceName,
    std::unique_ptr<NetworkConnectionHook> hook,
    std::unique_ptr<rpc::EgressMetadataHook> metadataHook,
    ConnectionPool::Options connPoolOptions) {
    NetworkInterfaceASIO::Options options{};
    options.instanceName = std::move(instanceName);
    options.networkConnectionHook = std::move(hook);
    options.metadataHook = std::move(metadataHook);
    options.timerFactory = stdx::make_unique<AsyncTimerFactoryASIO>();
    options.connectionPoolOptions = connPoolOptions;

#ifdef BONGO_CONFIG_SSL
    if (SSLManagerInterface* manager = getSSLManager()) {
        options.streamFactory = stdx::make_unique<AsyncSecureStreamFactory>(manager);
    }
#endif

    if (!options.streamFactory)
        options.streamFactory = stdx::make_unique<AsyncStreamFactory>();

    return stdx::make_unique<NetworkInterfaceASIO>(std::move(options));
}

}  // namespace executor
}  // namespace bongo
