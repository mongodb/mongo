// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#if MONGO_CONFIG_SSL_PROVIDER == MONGO_CONFIG_SSL_PROVIDER_WINDOWS

#include "mongo/util/net/ssl/context_schannel.hpp"

#elif MONGO_CONFIG_SSL_PROVIDER == MONGO_CONFIG_SSL_PROVIDER_OPENSSL

#include "mongo/util/net/ssl/context_openssl.hpp"

#elif MONGO_CONFIG_SSL_PROVIDER == MONGO_CONFIG_SSL_PROVIDER_APPLE

#include "mongo/util/net/ssl/context_apple.hpp"

#else
#error "Unknown SSL Provider"
#endif
