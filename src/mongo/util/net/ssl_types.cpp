// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0


#include "mongo/util/net/ssl_types.h"

#include "mongo/util/net/ssl_options.h"

namespace mongo {

const SSLParams& getSSLGlobalParams() {
    return sslGlobalParams;
}

}  // namespace mongo
