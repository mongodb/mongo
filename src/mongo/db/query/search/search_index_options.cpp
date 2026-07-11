// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/query/search/search_index_options.h"

#include "mongo/db/query/search/search_index_options_gen.h"
#include "mongo/util/net/hostandport.h"

#include <string_view>

namespace mongo {

SearchIndexParams globalSearchIndexParams;

Status SearchIndexParams::onValidateHost(std::string_view str, const boost::optional<TenantId>&) {
    // Unset value is OK
    if (str.empty()) {
        return Status::OK();
    }

    // `searchIndexManagementHostAndPort` must be able to parse into a HostAndPort.
    if (auto status = HostAndPort::parse(str); !status.isOK()) {
        return status.getStatus().withContext(
            "searchIndexManagementHostAndPort must be of the form \"host:port\"");
    }

    return Status::OK();
}

}  // namespace mongo
