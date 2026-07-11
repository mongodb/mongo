// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/config.h"  // IWYU pragma: keep
#include "mongo/db/commands/server_status/server_status.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/service_context.h"
#include "mongo/db/stats/counters.h"
#include "mongo/platform/atomic.h"
#include "mongo/transport/session_manager.h"
#include "mongo/util/net/ssl_manager.h"
#include "mongo/util/net/ssl_types.h"

namespace mongo {
namespace {

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
auto& security = *ServerStatusSectionBuilder<Security>("security").forShard().forRouter();

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
auto& tlsVersionStatus =
    *ServerStatusSectionBuilder<TLSVersionStatus>("transportSecurity").forShard().forRouter();
#endif

}  // namespace
}  // namespace mongo
