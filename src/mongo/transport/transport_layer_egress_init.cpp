/**
 *    Copyright 2018 MongoDB Inc.
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

#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kDefault

#include "mongo/platform/basic.h"

#include "mongo/base/init.h"
#include "mongo/db/service_context.h"
#include "mongo/transport/transport_layer_asio.h"

namespace mongo {
namespace {
// Linking with this file will configure an egress-only TransportLayer on a ServiceContextNoop.
// Use this for unit/integration tests that require only egress networking.
MONGO_INITIALIZER(ConfigureEgressTransportLayer)
(InitializerContext* context) {
    auto sc = getGlobalServiceContext();
    invariant(!sc->getTransportLayer());

    transport::TransportLayerASIO::Options opts;
    opts.mode = transport::TransportLayerASIO::Options::kEgress;
    sc->setTransportLayer(std::make_unique<transport::TransportLayerASIO>(opts, nullptr));
    auto status = sc->getTransportLayer()->setup();
    if (!status.isOK()) {
        return status;
    }

    return sc->getTransportLayer()->start();
}

}  // namespace
}  // namespace
