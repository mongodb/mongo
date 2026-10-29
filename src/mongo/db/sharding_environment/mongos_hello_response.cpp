// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/sharding_environment/mongos_hello_response.h"

#include "mongo/bson/bsonobjbuilder.h"

namespace mongo {

MongosHelloResponse::MongosHelloResponse(TopologyVersion topologyVersion) {
    _topologyVersion = topologyVersion;
    _isWritablePrimary = true;
    _msg = "isdbgrid";
}

void MongosHelloResponse::appendToBuilder(BSONObjBuilder* builder,
                                          bool useLegacyResponseFields) const {
    if (useLegacyResponseFields) {
        builder->append(kIsMasterFieldName, _isWritablePrimary);
    } else {
        builder->append(kIsWritablePrimaryFieldName, _isWritablePrimary);
    }
    builder->append(kMsgFieldName, _msg);

    BSONObjBuilder topologyVersionBuilder(builder->subobjStart(kTopologyVersionFieldName));
    _topologyVersion.serialize(&topologyVersionBuilder);
}

}  // namespace mongo
