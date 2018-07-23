/**
 * Copyright (C) 2018 MongoDB Inc.
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

#include "mongo/util/net/ssl_manager.h"

#include "mongo/config.h"
#include "mongo/db/commands/server_status.h"


#ifdef MONGO_CONFIG_SSL

namespace mongo {
namespace {

/**
 * Status section of which tls versions connected to MongoDB and completed an SSL handshake.
 * Note: Clients are only not counted if they try to connect to the server with a unsupported TLS
 * version. They are still counted if the server rejects them for certificate issues in
 * parseAndValidatePeerCertificate.
 */
class TLSVersionSatus : public ServerStatusSection {
public:
    TLSVersionSatus() : ServerStatusSection("transportSecurity") {}

    bool includeByDefault() const final {
        return true;
    }

    BSONObj generateSection(OperationContext* txn, const BSONElement& configElement) const final {
        auto& counts = TLSVersionCounts::get();

        BSONObjBuilder builder;
        builder.append("1.0", counts.tls10.load());
        builder.append("1.1", counts.tls11.load());
        builder.append("1.2", counts.tls12.load());
        return builder.obj();
    }
} tlsVersionStatus;

}  // namespace
}  // namespace mongo

#endif
