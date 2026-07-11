// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#if MONGO_CONFIG_SSL_PROVIDER == MONGO_CONFIG_SSL_PROVIDER_APPLE

#include <memory>
#include <string>

#include <CoreFoundation/CoreFoundation.h>
#include <Security/Security.h>

namespace asio {
namespace ssl {
namespace apple {

template <typename T>
struct CFReleaser {
    void operator()(T ptr) {
        if (ptr) {
            ::CFRelease(ptr);
        }
    }
};

/**
 * CoreFoundation types are internally refcounted using CFRetain/CFRelease.
 * Values received from a method using the word "Copy" typically follow "The Copy Rule"
 * which requires that the caller explicitly invoke CFRelease on the obtained value.
 * Values received from a method using the word "Get" typically follow "The Get Rule"
 * which requires that the caller DOES NOT attempt to release any references,
 * though it may invoke CFRetain to hold on to the object for longer.
 *
 * Use of the CFUniquePtr type assumes that a value was wither obtained from a "Copy"
 * method, or that it has been explicitly retained.
 */
template <typename T>
using CFUniquePtr = std::unique_ptr<typename std::remove_pointer<T>::type, CFReleaser<T>>;

/**
 * Equivalent of OpenSSL's SSL_CTX type.
 * Allows loading SecIdentity and SecCertificate chains
 * separate from an SSLContext instance.
 *
 * Unlike OpenSSL, Secure Transport sets protocol range on
 * each connection instance separately, so just stash them aside
 * in the same place for now.
 */
struct Context {
    Context() = default;
    explicit Context(::SSLProtocol p) : protoMin(p), protoMax(p) {}
    Context& operator=(const Context& src) {
        protoMin = src.protoMin;
        protoMax = src.protoMax;
        if (src.certs) {
            ::CFRetain(src.certs.get());
        }
        certs.reset(src.certs.get());
        return *this;
    }

    ::SSLProtocol protoMin = kTLSProtocol1;
    ::SSLProtocol protoMax = kTLSProtocol12;
    CFUniquePtr<::CFArrayRef> certs;
};

}  // namespace apple
}  // namespace ssl
}  // namespace asio

#endif
