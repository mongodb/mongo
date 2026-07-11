// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/config.h"
#include "mongo/util/modules.h"
#include "mongo/util/net/ssl_options.h"
#include "mongo/util/net/ssl_types.h"

#include <memory>

namespace asio {

namespace ssl {
class context;
}  // namespace ssl
}  // namespace asio

namespace mongo {

class SSLManagerInterface;

namespace transport {

#ifdef MONGO_CONFIG_SSL
struct [[MONGO_MOD_PUBLIC]] SSLConnectionContext {
    std::unique_ptr<asio::ssl::context> ingress;
    std::unique_ptr<asio::ssl::context> egress;
    std::shared_ptr<SSLManagerInterface> manager;
    // If this Context was created from transient SSL params this contains the URI of the target
    // cluster. It can also be used to determine if the context is indeed transient.
    boost::optional<std::string> targetClusterURI;

    ~SSLConnectionContext();
};
#endif

#ifndef MONGO_CONFIG_SSL
struct SSLConnectionContext {};
#endif

}  // namespace transport
}  // namespace mongo
