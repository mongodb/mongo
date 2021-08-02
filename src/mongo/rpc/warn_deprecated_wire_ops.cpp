/**
 *    Copyright (C) 2021-present MongoDB, Inc.
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

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kCommand

#include "mongo/platform/basic.h"

#include "mongo/rpc/warn_deprecated_wire_ops.h"

#include <fmt/format.h>
#include <string>

#include "mongo/db/client.h"
#include "mongo/logv2/log.h"
#include "mongo/logv2/log_severity_suppressor.h"
#include "mongo/rpc/deprecated_wire_ops_gen.h"
#include "mongo/rpc/metadata/client_metadata.h"
#include "mongo/util/duration.h"
#include "mongo/util/static_immortal.h"

namespace mongo {

using namespace fmt::literals;

void warnDeprecation(Client& client, StringData op) {
    static StaticImmortal<logv2::KeyedSeveritySuppressor<std::string>> bumpedSeverity{
        Seconds{deprecatedWireOpsWarningPeriodInSeconds.load()},
        logv2::LogSeverity::Warning(),
        logv2::LogSeverity::Debug(2)};

    std::string clientKey;
    BSONObj clientInfo;
    if (auto clientMetadata = ClientMetadata::get(&client); clientMetadata) {
        auto clientMetadataDoc = clientMetadata->getDocument();
        clientKey = "{}{}{}"_format(clientMetadata->getApplicationName(),
                                    clientMetadataDoc["driver"]["name"].toString(),
                                    clientMetadataDoc["driver"]["version"].toString());
        clientInfo = clientMetadataDoc;
    } else {
        clientKey = "{}"_format(client.clientAddress());
        clientInfo = BSON("address" << client.clientAddress(/*includePort*/ true));
    }

    LOGV2_DEBUG(5578800,
                (*bumpedSeverity)(clientKey).toInt(),
                "Deprecated operation requested. For more details see "
                "https://dochub.mongodb.org/core/legacy-opcode-compatibility",
                "op"_attr = op,
                "clientInfo"_attr = clientInfo);
}

}  // namespace mongo
