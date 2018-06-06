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

#pragma once

#if MONGO_CONFIG_SSL_PROVIDER == MONGO_CONFIG_SSL_PROVIDER_APPLE

#include <CoreFoundation/CoreFoundation.h>
#include <Security/Security.h>
#include <memory>
#include <string>

namespace asio {
namespace ssl {
namespace apple {

namespace {
template <typename T>
struct CFReleaser {
    void operator()(T ptr) {
        if (ptr) {
            ::CFRelease(ptr);
        }
    }
};
}  // namespace

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
