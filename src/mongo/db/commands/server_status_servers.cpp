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

#include <memory>


#include "mongo/base/status_with.h"
#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/config.h"  // IWYU pragma: keep
#include "mongo/db/commands/server_status.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/service_context.h"
#include "mongo/db/stats/counters.h"
#include "mongo/platform/atomic_word.h"
#include "mongo/transport/message_compressor_registry.h"
#include "mongo/transport/service_executor.h"
#include "mongo/transport/session_manager.h"
#include "mongo/transport/transport_layer_manager.h"
#include "mongo/util/assert_util_core.h"
#include "mongo/util/net/hostname_canonicalization.h"
#include "mongo/util/net/socket_utils.h"
#include "mongo/util/net/ssl_manager.h"
#include "mongo/util/net/ssl_types.h"

namespace mongo {
namespace {

// some universal sections

class Network : public ServerStatusSection {
public:
    using ServerStatusSection::ServerStatusSection;

    bool includeByDefault() const override {
        return true;
    }

    BSONObj generateSection(OperationContext* opCtx,
                            const BSONElement& configElement) const override {
        BSONObjBuilder b;
        networkCounter.append(b);
        appendMessageCompressionStats(&b);

        auto svcCtx = opCtx->getServiceContext();

        {
            BSONObjBuilder section = b.subobjStart("serviceExecutors");
            transport::ServiceExecutor::appendAllServerStats(&section, svcCtx);
        }

        if (auto tl = svcCtx->getTransportLayerManager())
            tl->appendStatsForServerStatus(&b);

        return b.obj();
    }
};
auto& network = *ServerStatusSectionBuilder<Network>("network");

class Security : public ServerStatusSection {
public:
    using ServerStatusSection::ServerStatusSection;

    bool includeByDefault() const override {
        return true;
    }

    BSONObj generateSection(OperationContext* opCtx,
                            const BSONElement& configElement) const override {
        BSONObjBuilder result;

        BSONObjBuilder auth;
        authCounter.append(&auth);
        result.append("authentication", auth.obj());

#ifdef MONGO_CONFIG_SSL
        if (SSLManagerCoordinator::get()) {
            SSLManagerCoordinator::get()
                ->getSSLManager()
                ->getSSLConfiguration()
                .getServerStatusBSON(&result);
        }
#endif

        return result.obj();
    }
};
auto& security = *ServerStatusSectionBuilder<Security>("security");

#ifdef MONGO_CONFIG_SSL
/**
 * Status section of which tls versions connected to MongoDB and completed an SSL handshake.
 * Note: Clients are only not counted if they try to connect to the server with a unsupported TLS
 * version. They are still counted if the server rejects them for certificate issues in
 * parseAndValidatePeerCertificate.
 */
class TLSVersionStatus : public ServerStatusSection {
public:
    using ServerStatusSection::ServerStatusSection;

    bool includeByDefault() const override {
        return true;
    }

    BSONObj generateSection(OperationContext* opCtx,
                            const BSONElement& configElement) const override {
        auto& counts = TLSVersionCounts::get(opCtx->getServiceContext());

        BSONObjBuilder builder;
        builder.append("1.0", counts.tls10.load());
        builder.append("1.1", counts.tls11.load());
        builder.append("1.2", counts.tls12.load());
        builder.append("1.3", counts.tls13.load());
        builder.append("unknown", counts.tlsUnknown.load());
        return builder.obj();
    }
};
auto& tlsVersionStatus = *ServerStatusSectionBuilder<TLSVersionStatus>("transportSecurity");
#endif

class AdvisoryHostFQDNs final : public ServerStatusSection {
public:
    using ServerStatusSection::ServerStatusSection;

    bool includeByDefault() const override {
        return false;
    }

    void appendSection(OperationContext* opCtx,
                       const BSONElement& configElement,
                       BSONObjBuilder* out) const override {
        auto statusWith =
            getHostFQDNs(getHostNameCached(), HostnameCanonicalizationMode::kForwardAndReverse);
        if (statusWith.isOK()) {
            out->append("advisoryHostFQDNs", statusWith.getValue());
        }
    }
};
auto& advisoryHostFQDNs = *ServerStatusSectionBuilder<AdvisoryHostFQDNs>("advisoryHostFQDNs");

}  // namespace
}  // namespace mongo
