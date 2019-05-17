
/**
 *    Copyright (C) 2018-present MongoDB, Inc.
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

#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kNetwork

#include "mongo/platform/basic.h"

#include <boost/optional/optional.hpp>
#include <fstream>
#include <stdlib.h>

#include "mongo/base/checked_cast.h"
#include "mongo/base/init.h"
#include "mongo/base/initializer_context.h"
#include "mongo/base/status.h"
#include "mongo/base/status_with.h"
#include "mongo/crypto/sha1_block.h"
#include "mongo/crypto/sha256_block.h"
#include "mongo/platform/random.h"
#include "mongo/stdx/memory.h"
#include "mongo/util/base64.h"
#include "mongo/util/concurrency/mutex.h"
#include "mongo/util/log.h"
#include "mongo/util/net/private/ssl_expiration.h"
#include "mongo/util/net/socket_exception.h"
#include "mongo/util/net/ssl/apple.hpp"
#include "mongo/util/net/ssl_manager.h"
#include "mongo/util/net/ssl_options.h"

using asio::ssl::apple::CFUniquePtr;

/* This API appears in the Security framework even though
 * the SecIdentity.h header doesn't reference it.
 *
 * Use it explicitly for turning Cert/Key pairs into Identities
 * because it's way cheaper than going via keychain and has the
 * handy property of not causing difficult to diagnose heisenbugs.
 */
extern "C" SecIdentityRef SecIdentityCreate(CFAllocatorRef, SecCertificateRef, SecKeyRef);

namespace mongo {

namespace {

template <typename T>
constexpr T cf_cast(::CFTypeRef val) {
    return static_cast<T>(const_cast<void*>(val));
}

// CFAbsoluteTime and X.509 is relative to Jan 1 2001 00:00:00 GMT
// Unix Epoch (and thereby Date_t) is relative to Jan 1, 1970 00:00:00 GMT
static const ::CFAbsoluteTime k20010101_000000_GMT = 978307200;

::CFStringRef kMongoDBRolesOID = nullptr;

StatusWith<std::string> toString(::CFStringRef str) {
    const auto len =
        ::CFStringGetMaximumSizeForEncoding(::CFStringGetLength(str), ::kCFStringEncodingUTF8);
    if (len == 0) {
        return std::string();
    }

    std::string ret;
    ret.resize(len + 1);
    if (!::CFStringGetCString(str, &ret[0], len, ::kCFStringEncodingUTF8)) {
        return Status(ErrorCodes::InternalError, "Unable to convert CoreFoundation string");
    }
    ret.resize(strlen(ret.c_str()));
    return ret;
}

// Never actually errors, but use StatusWith to be
// consistent with other toString() signatures.
StatusWith<std::string> toString(::CFDataRef data) {
    const auto len = ::CFDataGetLength(data);
    const auto* p = (const char*)::CFDataGetBytePtr(data);
    return std::string(p, len);
}

StatusWith<std::string> toString(::OSStatus status) {
    CFUniquePtr<::CFStringRef> errstr(::SecCopyErrorMessageString(status, nullptr));
    if (!errstr) {
        return Status(ErrorCodes::InternalError, "Unable to convert OSStatus");
    }
    auto ret = toString(errstr.get());
    return ret;
}

// Ideally we'd use an operator<< overload,
// but OSStatus is just a uint32_t.
// Be explicit about conversion to provide meaningful
// output in error streams.
std::string stringFromOSStatus(::OSStatus status) {
    static_assert(std::is_same<std::int32_t, ::OSStatus>::value,
                  "CoreFoundation OSStatus has changed type");
    auto ret = toString(status);
    if (!ret.isOK()) {
        return str::stream() << "Unknown error: " << static_cast<std::int32_t>(status);
    }
    return ret.getValue();
}

// CFTypeRef is actually just `void*`.
// So while we could be polymorphic with the other toString() methods,
// it's basically asking for a hard to diagnose type error.
StatusWith<std::string> stringFromCFType(::CFTypeRef val) {
    const auto type = val ? ::CFGetTypeID(val) : ((CFTypeID)-1);
    if (type == ::CFStringGetTypeID()) {
        return toString(static_cast<::CFStringRef>(val));
    } else if (type == ::CFDataGetTypeID()) {
        return toString(static_cast<::CFDataRef>(val));
    } else {
        return Status(ErrorCodes::BadValue, "Value is not translatable to string");
    }
}

std::ostringstream& operator<<(std::ostringstream& ss, ::CFStringRef str) {
    auto swStr = toString(str);
    if (swStr.isOK()) {
        ss << swStr.getValue();
    } else {
        ss << "Unknown error";
    }
    return ss;
}

std::ostringstream& operator<<(std::ostringstream& ss, ::CFErrorRef error) {
    std::string comma;

    CFUniquePtr<::CFStringRef> desc(::CFErrorCopyDescription(error));
    if (desc) {
        ss << comma << desc.get();
        comma = ", ";
    }

    CFUniquePtr<::CFStringRef> reason(::CFErrorCopyFailureReason(error));
    if (reason) {
        ss << comma << reason.get();
        comma = ", ";
    }

    CFUniquePtr<::CFStringRef> suggest(::CFErrorCopyRecoverySuggestion(error));
    if (suggest) {
        ss << comma << suggest.get();
        comma = ", ";
    }

    auto code = ::CFErrorGetCode(error);
    if (code) {
        ss << comma << "Code: " << code;
    }

    return ss;
}

void uassertOSStatusOK(::OSStatus status,
                       ErrorCodes::Error code = ErrorCodes::InvalidSSLConfiguration) {
    if (status == ::errSecSuccess) {
        return;
    }
    auto swMsg = toString(status);
    if (!swMsg.isOK()) {
        uasserted(code, str::stream() << "Unknown SSL error" << static_cast<int>(status));
    }
    uasserted(code, swMsg.getValue());
}

void uassertOSStatusOK(::OSStatus status, SocketErrorKind kind) {
    if (status == ::errSecSuccess) {
        return;
    }
    auto swMsg = toString(status);
    if (!swMsg.isOK()) {
        throwSocketError(kind, str::stream() << "Unknown SSL error" << static_cast<int>(status));
    }
    throwSocketError(kind, swMsg.getValue());
}

bool isUnixDomainSocket(const std::string& hostname) {
    return end(hostname) != std::find(begin(hostname), end(hostname), '/');
}

::OSStatus posixErrno(int err) {
    switch (err) {
        case EAGAIN:
            return ::errSSLWouldBlock;
        case ENOENT:
            return ::errSSLClosedGraceful;
        case ECONNRESET:
            return ::errSSLClosedAbort;
        default:
            return ::errSSLInternal;
    }
}

namespace detail {
template <typename T>
struct CFTypeMap {};
template <>
struct CFTypeMap<::CFStringRef> {
    static constexpr StringData typeName = "string"_sd;
    static ::CFTypeID type() {
        return ::CFStringGetTypeID();
    }
};
constexpr StringData CFTypeMap<::CFStringRef>::typeName;
template <>
struct CFTypeMap<::CFDataRef> {
    static constexpr StringData typeName = "data"_sd;
    static ::CFTypeID type() {
        return ::CFDataGetTypeID();
    }
};
constexpr StringData CFTypeMap<::CFDataRef>::typeName;
template <>
struct CFTypeMap<::CFNumberRef> {
    static constexpr StringData typeName = "number"_sd;
    static ::CFTypeID type() {
        return ::CFNumberGetTypeID();
    }
};
constexpr StringData CFTypeMap<::CFNumberRef>::typeName;
template <>
struct CFTypeMap<::CFArrayRef> {
    static constexpr StringData typeName = "array"_sd;
    static ::CFTypeID type() {
        return ::CFArrayGetTypeID();
    }
};
constexpr StringData CFTypeMap<::CFArrayRef>::typeName;
template <>
struct CFTypeMap<::CFDictionaryRef> {
    static constexpr StringData typeName = "dictionary"_sd;
    static ::CFTypeID type() {
        return ::CFDictionaryGetTypeID();
    }
};
constexpr StringData CFTypeMap<::CFDictionaryRef>::typeName;
}  // namespace detail

template <typename T>
StatusWith<T> extractDictionaryValue(::CFDictionaryRef dict, ::CFStringRef key) {
    const auto badValue = [key](StringData msg) -> Status {
        auto swKey = toString(key);
        if (!swKey.isOK()) {
            return {ErrorCodes::InvalidSSLConfiguration, msg};
        }
        return {ErrorCodes::InvalidSSLConfiguration,
                str::stream() << msg << " for key'" << swKey.getValue() << "'"};
    };

    auto val = ::CFDictionaryGetValue(dict, key);
    if (!val) {
        return badValue("Missing value");
    }
    if (::CFGetTypeID(val) != detail::CFTypeMap<T>::type()) {
        return badValue(str::stream() << "Value is not a " << detail::CFTypeMap<T>::typeName);
    }
    return reinterpret_cast<T>(val);
}

StatusWith<SSLX509Name::Entry> extractSingleOIDEntry(::CFDictionaryRef entry) {
    auto swLabel = extractDictionaryValue<::CFStringRef>(entry, ::kSecPropertyKeyLabel);
    if (!swLabel.isOK()) {
        return swLabel.getStatus();
    }
    auto swLabelStr = toString(swLabel.getValue());
    if (!swLabelStr.isOK()) {
        return swLabelStr.getStatus();
    }

    auto swValue = extractDictionaryValue<::CFStringRef>(entry, ::kSecPropertyKeyValue);
    if (!swValue.isOK()) {
        return swValue.getStatus();
    }
    auto swValueStr = toString(swValue.getValue());
    if (!swValueStr.isOK()) {
        return swValueStr.getStatus();
    }

    // Secure Transport doesn't give us access to the specific string type,
    // so regard all strings as ASN1_PRINTABLESTRING on this platform.
    return SSLX509Name::Entry(
        std::move(swLabelStr.getValue()), 19, std::move(swValueStr.getValue()));
}

// Translate a raw DER subject sequence into a structured subject name.
StatusWith<SSLX509Name> extractSubjectName(::CFDictionaryRef dict) {
    auto swSubject = extractDictionaryValue<::CFDictionaryRef>(dict, ::kSecOIDX509V1SubjectName);
    if (!swSubject.isOK()) {
        return swSubject.getStatus();
    }

    auto swElems =
        extractDictionaryValue<::CFArrayRef>(swSubject.getValue(), ::kSecPropertyKeyValue);
    if (!swElems.isOK()) {
        return swElems.getStatus();
    }

    auto elems = swElems.getValue();
    const auto nElems = ::CFArrayGetCount(elems);
    std::vector<std::vector<SSLX509Name::Entry>> ret;
    for (auto i = nElems; i; --i) {
        auto elem = reinterpret_cast<::CFDictionaryRef>(::CFArrayGetValueAtIndex(elems, i - 1));
        invariant(elem);
        if (::CFGetTypeID(elem) != ::CFDictionaryGetTypeID()) {
            return {ErrorCodes::InvalidSSLConfiguration,
                    "Subject name element is not a dictionary"};
        }

        auto swType = extractDictionaryValue<::CFStringRef>(elem, ::kSecPropertyKeyType);
        if (!swType.isOK()) {
            return swType.getStatus();
        }

        if (!::CFStringCompare(swType.getValue(), CFSTR("section"), 0)) {
            // Multi-value RDN.
            auto swList = extractDictionaryValue<::CFArrayRef>(elem, ::kSecPropertyKeyValue);
            if (!swList.isOK()) {
                return swList.getStatus();
            }
            const auto nRDNAttrs = ::CFArrayGetCount(swList.getValue());
            std::vector<SSLX509Name::Entry> rdn;
            for (auto j = nRDNAttrs; j; --j) {
                auto rdnElem = reinterpret_cast<::CFDictionaryRef>(
                    ::CFArrayGetValueAtIndex(swList.getValue(), j - 1));
                invariant(rdnElem);
                if (::CFGetTypeID(rdnElem) != ::CFDictionaryGetTypeID()) {
                    return {ErrorCodes::InvalidSSLConfiguration,
                            "Subject name sub-element is not a dictionary"};
                }
                auto swEntry = extractSingleOIDEntry(rdnElem);
                if (!swEntry.isOK()) {
                    return swEntry.getStatus();
                }
                rdn.push_back(std::move(swEntry.getValue()));
            }
            ret.push_back(std::move(rdn));

        } else {
            // Single Value RDN.
            auto swEntry = extractSingleOIDEntry(elem);
            if (!swEntry.isOK()) {
                return swEntry.getStatus();
            }
            ret.push_back(std::vector<SSLX509Name::Entry>({std::move(swEntry.getValue())}));
        }
    }

    return SSLX509Name(std::move(ret));
}

StatusWith<mongo::Date_t> extractValidityDate(::CFDictionaryRef dict,
                                              ::CFStringRef oid,
                                              StringData name) {
    auto swVal = extractDictionaryValue<::CFDictionaryRef>(dict, oid);
    if (!swVal.isOK()) {
        return swVal.getStatus();
    }

    auto swNum = extractDictionaryValue<::CFNumberRef>(swVal.getValue(), ::kSecPropertyKeyValue);
    if (!swNum.isOK()) {
        return swNum.getStatus();
    }

    int64_t dateval = 0;
    if (!::CFNumberGetValue(swNum.getValue(), ::kCFNumberSInt64Type, &dateval)) {
        return {ErrorCodes::InvalidSSLConfiguration,
                str::stream() << "Certificate contains invalid " << name
                              << ": OID contains invalid numeric value"};
    }

    return Date_t::fromMillisSinceEpoch((k20010101_000000_GMT + dateval) * 1000);
}

StatusWith<stdx::unordered_set<RoleName>> parsePeerRoles(::CFDictionaryRef dict) {
    if (!::CFDictionaryContainsKey(dict, kMongoDBRolesOID)) {
        return stdx::unordered_set<RoleName>();
    }

    auto swRolesKey = extractDictionaryValue<::CFDictionaryRef>(dict, kMongoDBRolesOID);
    if (!swRolesKey.isOK()) {
        return swRolesKey.getStatus();
    }
    auto swRolesList =
        extractDictionaryValue<::CFArrayRef>(swRolesKey.getValue(), ::kSecPropertyKeyValue);
    if (!swRolesList.isOK()) {
        return swRolesList.getStatus();
    }
    auto rolesList = swRolesList.getValue();
    const auto count = ::CFArrayGetCount(rolesList);
    for (::CFIndex i = 0; i < count; ++i) {
        auto elemval = ::CFArrayGetValueAtIndex(rolesList, i);
        invariant(elemval);
        if (::CFGetTypeID(elemval) != ::CFDictionaryGetTypeID()) {
            return {ErrorCodes::InvalidSSLConfiguration,
                    "Invalid list element in Certificate Roles OID"};
        }

        auto elem = reinterpret_cast<::CFDictionaryRef>(elemval);
        auto swType = extractDictionaryValue<::CFStringRef>(elem, ::kSecPropertyKeyType);
        if (!swType.isOK()) {
            return swType.getStatus();
        }
        if (::CFStringCompare(swType.getValue(), ::kSecPropertyTypeData, 0)) {
            // Non data, ignore.
            continue;
        }
        auto swData = extractDictionaryValue<::CFDataRef>(elem, ::kSecPropertyKeyValue);
        if (!swData.isOK()) {
            return swData.getStatus();
        }
        ConstDataRange rolesData(
            reinterpret_cast<const char*>(::CFDataGetBytePtr(swData.getValue())),
            ::CFDataGetLength(swData.getValue()));
        return parsePeerRoles(rolesData);
    }

    return {ErrorCodes::InvalidSSLConfiguration, "Unable to extract role data from certificate"};
}

StatusWith<std::vector<std::string>> extractSubjectAlternateNames(::CFDictionaryRef dict) {
    const auto badValue = [](StringData msg) -> Status {
        return {ErrorCodes::InvalidSSLConfiguration,
                str::stream() << "Certificate contains invalid SAN: " << msg};
    };
    auto swSANDict = extractDictionaryValue<::CFDictionaryRef>(dict, ::kSecOIDSubjectAltName);
    if (!swSANDict.isOK()) {
        return swSANDict.getStatus();
    }

    auto sanDict = swSANDict.getValue();
    auto swList = extractDictionaryValue<::CFArrayRef>(sanDict, ::kSecPropertyKeyValue);
    if (!swList.isOK()) {
        return swList.getStatus();
    }

    std::vector<std::string> ret;
    auto list = swList.getValue();
    const auto count = ::CFArrayGetCount(list);
    for (::CFIndex i = 0; i < count; ++i) {
        auto elemval = ::CFArrayGetValueAtIndex(list, i);
        invariant(elemval);
        if (::CFGetTypeID(elemval) != ::CFDictionaryGetTypeID()) {
            return badValue("Invalid list element");
        }

        auto elem = reinterpret_cast<::CFDictionaryRef>(elemval);
        auto swLabel = extractDictionaryValue<::CFStringRef>(elem, ::kSecPropertyKeyLabel);
        if (!swLabel.isOK()) {
            return swLabel.getStatus();
        }
        if (::CFStringCompare(swLabel.getValue(), CFSTR("DNS Name"), ::kCFCompareCaseInsensitive) !=
            ::kCFCompareEqualTo) {
            // Skip other elements, e.g. 'Critical'
            continue;
        }
        auto swName = extractDictionaryValue<::CFStringRef>(elem, ::kSecPropertyKeyValue);
        if (!swName.isOK()) {
            return swName.getStatus();
        }
        auto swNameStr = toString(swName.getValue());
        if (!swNameStr.isOK()) {
            return swNameStr.getStatus();
        }
        ret.push_back(swNameStr.getValue());
    }
    return ret;
}

bool isCFDataEqual(::CFDataRef a, ::CFDataRef b) {
    const auto len = ::CFDataGetLength(a);
    if (::CFDataGetLength(b) != len) {
        return false;
    }

    const auto* A = ::CFDataGetBytePtr(a);
    const auto* B = ::CFDataGetBytePtr(b);
    return 0 == memcmp(A, B, len);
}

/**
 * Attempt to merge a security item bundle into an
 * identity and optional cert chain.
 *
 * The file must have exactly one key which will be paired with
 * the first available certificate, or exactly one identity.
 */
StatusWith<::CFArrayRef> bindIdentity(::CFArrayRef certs) {
    auto count = ::CFArrayGetCount(certs);
    if (count == 0) {
        return certs;
    }

    // Ideal case, exactly one identity.
    if (count == 1) {
        auto idElem = ::CFArrayGetValueAtIndex(certs, 0);
        if (::CFGetTypeID(idElem) == ::SecIdentityGetTypeID()) {
            return certs;
        }
    }

    // Optimistic case, exactly one cert-key pair.
    if (count == 2) {
        auto certElem = ::CFArrayGetValueAtIndex(certs, 0);
        auto keyElem = ::CFArrayGetValueAtIndex(certs, 1);
        if (::CFGetTypeID(certElem) == ::SecKeyGetTypeID()) {
            std::swap(certElem, keyElem);
        }
        if ((::CFGetTypeID(certElem) == ::SecCertificateGetTypeID()) &&
            (::CFGetTypeID(keyElem) == ::SecKeyGetTypeID())) {
            CFUniquePtr<::SecIdentityRef> cfid(::SecIdentityCreate(
                nullptr, cf_cast<::SecCertificateRef>(certElem), cf_cast<::SecKeyRef>(keyElem)));
            if (cfid) {
                auto id = static_cast<const void*>(cfid.get());
                return ::CFArrayCreate(nullptr, &id, 1, &kCFTypeArrayCallBacks);
            }
        }
    }

    // Complex case, multiple certs.
    // Find the key, pair it with the first cert, and bundle the remaining certs in.
    std::vector<::SecCertificateRef> intermediateCerts;
    ::SecIdentityRef id = nullptr;
    ::SecCertificateRef leafCert = nullptr;
    ::SecKeyRef key = nullptr;
    for (::CFIndex i = 0; i < count; ++i) {
        auto elem = ::CFArrayGetValueAtIndex(certs, i);
        invariant(elem);

        const auto elemType = ::CFGetTypeID(elem);
        if (elemType == ::SecIdentityGetTypeID()) {
            if (id) {
                return {ErrorCodes::InvalidSSLConfiguration,
                        str::stream() << "Multiple identities found in PEM file"};
            }
            id = cf_cast<::SecIdentityRef>(elem);
            continue;
        }

        if (elemType == ::SecKeyGetTypeID()) {
            if (key) {
                return {ErrorCodes::InvalidSSLConfiguration,
                        str::stream() << "Multiple private keys found in PEM file"};
            }
            key = cf_cast<::SecKeyRef>(elem);
            continue;
        }

        if (elemType != ::SecCertificateGetTypeID()) {
            // Ignore other types.
            continue;
        }

        if (leafCert) {
            intermediateCerts.push_back(cf_cast<::SecCertificateRef>(elem));
        } else {
            leafCert = cf_cast<::SecCertificateRef>(elem);
        }
    }

    if (id && key) {
        return {ErrorCodes::InvalidSSLConfiguration,
                "Found both identity and private key in PEM file"};
    }
    if (key && !leafCert) {
        return {ErrorCodes::InvalidSSLConfiguration, "Found key without certificate in PEM file"};
    }

    CFUniquePtr<::CFMutableArrayRef> ret(
        ::CFArrayCreateMutable(nullptr, count, &kCFTypeArrayCallBacks));

    if (id) {
        ::CFArrayAppendValue(ret.get(), id);
    }

    if (key) {
        CFUniquePtr<::SecIdentityRef> ident(::SecIdentityCreate(nullptr, leafCert, key));
        if (!ident) {
            return {ErrorCodes::InvalidSSLConfiguration, "Unable to create Identity from keyfile"};
        }
        ::CFArrayAppendValue(ret.get(), ident.get());
    } else if (leafCert) {
        ::CFArrayAppendValue(ret.get(), leafCert);
    }

    for (auto& cert : intermediateCerts) {
        ::CFArrayAppendValue(ret.get(), cert);
    }

    return ret.release();
}

/**
 * Strip a security item bundle down to just Certificates.
 * This means ignoring SecKeyRef and splitting SecIdentityRef
 * into just their SecCertificateRef potions.
 */
StatusWith<::CFArrayRef> stripKeys(::CFArrayRef certs) {
    auto count = ::CFArrayGetCount(certs);

    if (count == 0) {
        return certs;
    }

    // Strip unpaired keys and identities.
    CFUniquePtr<::CFMutableArrayRef> ret(
        ::CFArrayCreateMutable(nullptr, count, &kCFTypeArrayCallBacks));
    for (::CFIndex i = 0; i < count; ++i) {
        auto elem = ::CFArrayGetValueAtIndex(certs, i);
        if (!elem) {
            continue;
        }
        const auto type = ::CFGetTypeID(elem);
        if (type == ::SecCertificateGetTypeID()) {
            // Preserve Certificates.
            ::CFArrayAppendValue(ret.get(), elem);
            continue;
        }
        if (type != ::SecIdentityGetTypeID()) {
            continue;
        }

        // Extract public certificate from Identity.
        ::SecCertificateRef cert = nullptr;
        const auto status = ::SecIdentityCopyCertificate(cf_cast<::SecIdentityRef>(elem), &cert);
        CFUniquePtr<::SecCertificateRef> cfcert(cert);
        if (status != ::errSecSuccess) {
            return {ErrorCodes::InternalError,
                    str::stream() << "Unable to extract certificate from identity: "
                                  << stringFromOSStatus(status)};
        }
        ::CFArrayAppendValue(ret.get(), cfcert.get());
    }

    return ret.release();
}

enum LoadPEMMode {
    kLoadPEMBindIdentities = true,
    kLoadPEMStripKeys = false,
};
/**
 * Load a PEM encoded file from disk.
 * This file may contain multiple PEM blocks (e.g. a private key and one or more certificates).
 * Because SecItemImport loads all items as-is, we must manually attempt to pair up
 * corresponding Certificates and Keys.
 * This is done using a temporary Keychain and looping through the results.
 *
 * Depending on the value passed for <mode> any SecKey instances present will be
 * either discarded, or combined with matching SecCertificates to make SecIdentities.
 * Unbound certificates will remain in the CFArray as-is.
 */
StatusWith<CFUniquePtr<::CFArrayRef>> loadPEM(const std::string& keyfilepath,
                                              const std::string& passphrase = "",
                                              const LoadPEMMode mode = kLoadPEMBindIdentities) {
    const auto retFail = [&keyfilepath, &passphrase](const std::string& msg = "") {
        return Status(ErrorCodes::InvalidSSLConfiguration,
                      str::stream() << "Unable to load PEM from '" << keyfilepath << "'"
                                    << (passphrase.empty() ? "" : " with passphrase")
                                    << (msg.empty() ? "" : ": ")
                                    << msg);
    };

    std::ifstream pemFile(keyfilepath, std::ios::binary);
    if (!pemFile.is_open()) {
        return retFail("Failed opening file");
    }

    std::vector<uint8_t> pemdata((std::istreambuf_iterator<char>(pemFile)),
                                 std::istreambuf_iterator<char>());
    CFUniquePtr<CFDataRef> cfdata(::CFDataCreate(nullptr, pemdata.data(), pemdata.size()));
    invariant(cfdata);
    pemdata.clear();

    CFUniquePtr<CFDataRef> cfpass;
    if (!passphrase.empty()) {
        cfpass.reset(::CFDataCreate(
            nullptr, reinterpret_cast<const uint8_t*>(passphrase.c_str()), passphrase.size()));
    }
    ::SecItemImportExportKeyParameters params = {
        SEC_KEY_IMPORT_EXPORT_PARAMS_VERSION, 0, cfpass.get(),
    };

    CFUniquePtr<CFStringRef> cfkeyfile(
        ::CFStringCreateWithCString(nullptr, keyfilepath.c_str(), ::kCFStringEncodingUTF8));
    auto format = ::kSecFormatUnknown;
    auto type = ::kSecItemTypeUnknown;
    ::CFArrayRef certs = nullptr;
    auto status = ::SecItemImport(cfdata.get(),
                                  cfkeyfile.get(),
                                  &format,
                                  &type,
                                  ::kSecItemPemArmour,
                                  &params,
                                  nullptr,
                                  &certs);
    CFUniquePtr<::CFArrayRef> cfcerts(certs);
    if ((status == ::errSecUnknownFormat) && !passphrase.empty()) {
        // The Security framework in OSX doesn't support PKCS#8 encrypted keys
        // using modern encryption algorithms. Give the user a hint about that.
        return Status(ErrorCodes::InvalidSSLConfiguration,
                      "Unable to import PEM key file, possibly due to presence of PKCS#8 encrypted "
                      "key. Consider using a certificate selector or PKCS#12 instead");
    }
    if (status != ::errSecSuccess) {
        return retFail(str::stream() << "Failing importing certificate(s): "
                                     << stringFromOSStatus(status));
    }

    if (mode == kLoadPEMBindIdentities) {
        auto swCerts = bindIdentity(cfcerts.get());
        if (!swCerts.isOK()) {
            return swCerts.getStatus();
        }
        cfcerts.reset(swCerts.getValue());
    } else {
        invariant(mode == kLoadPEMStripKeys);
        auto swCerts = stripKeys(cfcerts.get());
        if (!swCerts.isOK()) {
            return swCerts.getStatus();
        }
        cfcerts.reset(swCerts.getValue());
    }

    if (::CFArrayGetCount(cfcerts.get()) <= 0) {
        return {ErrorCodes::InvalidSSLConfiguration,
                str::stream() << "PEM file '" << keyfilepath << "' has no certificates"};
    }

    // Rewrap the return to be the non-mutable type.
    return std::move(cfcerts);
}

StatusWith<SSLX509Name> certificateGetSubject(::SecCertificateRef cert, Date_t* expire = nullptr) {
    // Fetch expiry range and full subject name.
    CFUniquePtr<::CFMutableArrayRef> oids(
        ::CFArrayCreateMutable(nullptr, expire ? 3 : 1, &::kCFTypeArrayCallBacks));
    ::CFArrayAppendValue(oids.get(), ::kSecOIDX509V1SubjectName);
    if (expire) {
        ::CFArrayAppendValue(oids.get(), ::kSecOIDX509V1ValidityNotBefore);
        ::CFArrayAppendValue(oids.get(), ::kSecOIDX509V1ValidityNotAfter);
    }

    ::CFErrorRef cferror = nullptr;
    CFUniquePtr<::CFDictionaryRef> cfdict(::SecCertificateCopyValues(cert, oids.get(), &cferror));
    if (cferror) {
        CFUniquePtr<::CFErrorRef> deleter(cferror);
        return {ErrorCodes::InvalidSSLConfiguration,
                str::stream() << "Unable to determine certificate validity: " << cferror};
    }

    auto swSubjectName = extractSubjectName(cfdict.get());
    if (!swSubjectName.isOK()) {
        return swSubjectName.getStatus();
    }
    auto subject = swSubjectName.getValue();

    if (!expire) {
        return subject;
    }

    // Marshal expiration.
    auto swValidFrom =
        extractValidityDate(cfdict.get(), ::kSecOIDX509V1ValidityNotBefore, "valid-from");
    auto swValidUntil =
        extractValidityDate(cfdict.get(), ::kSecOIDX509V1ValidityNotAfter, "valid-until");
    if (!swValidFrom.isOK() || !swValidUntil.isOK()) {
        return swValidUntil.getStatus();
    }

    *expire = swValidUntil.getValue();
    const auto now = Date_t::now();
    if ((now < swValidFrom.getValue()) || (*expire < now)) {
        return {ErrorCodes::InvalidSSLConfiguration,
                "The provided SSL certificate is expired or not yet valid"};
    }

    return subject;
}

StatusWith<SSLX509Name> certificateGetSubject(::CFArrayRef certs, Date_t* expire = nullptr) {
    if (::CFArrayGetCount(certs) <= 0) {
        return {ErrorCodes::InvalidSSLConfiguration, "No certificates in certificate list"};
    }

    auto root = ::CFArrayGetValueAtIndex(certs, 0);
    if (!root || (::CFGetTypeID(root) != ::SecIdentityGetTypeID())) {
        return {ErrorCodes::InvalidSSLConfiguration, "Root certificate not an identity pair"};
    }

    ::SecCertificateRef idcert = nullptr;
    auto status = ::SecIdentityCopyCertificate(cf_cast<::SecIdentityRef>(root), &idcert);
    if (status != ::errSecSuccess) {
        return {ErrorCodes::InvalidSSLConfiguration,
                str::stream() << "Unable to get certificate from identity: "
                              << stringFromOSStatus(status)};
    }
    CFUniquePtr<::SecCertificateRef> cert(idcert);
    return certificateGetSubject(cert.get(), expire);
}

StatusWith<CFUniquePtr<::CFArrayRef>> copyMatchingCertificate(
    const SSLParams::CertificateSelector& selector,
    SSLManagerInterface::ConnectionDirection direction) {
    if (selector.subject.empty() && selector.thumbprint.empty()) {
        // In practice, this should never occur, thanks to the selector.empty()
        // checks at the callsites of this function.
        return {ErrorCodes::InvalidSSLConfiguration, "Certificate selector has no values"};
    }
    if (!selector.subject.empty() && !selector.thumbprint.empty()) {
        // This can only happen if the parsing logic in ssl_options.cpp changes.
        // Guard against it to play it safe.
        return {ErrorCodes::InvalidSSLConfiguration, "Certificate selector has multiple values"};
    }

    const bool isServer = (direction == SSLManagerInterface::ConnectionDirection::kIncoming);
    CFUniquePtr<::SecPolicyRef> cfpolicy(::SecPolicyCreateSSL(isServer, nullptr));

    CFUniquePtr<::CFMutableDictionaryRef> cfquery(::CFDictionaryCreateMutable(
        nullptr, 5, &::kCFTypeDictionaryKeyCallBacks, &::kCFTypeDictionaryValueCallBacks));
    ::CFDictionaryAddValue(cfquery.get(), ::kSecClass, ::kSecClassIdentity);
    ::CFDictionaryAddValue(cfquery.get(), ::kSecReturnRef, ::kCFBooleanTrue);
    ::CFDictionaryAddValue(cfquery.get(), ::kSecMatchLimit, ::kSecMatchLimitAll);
    ::CFDictionaryAddValue(cfquery.get(), ::kSecMatchPolicy, cfpolicy.get());

    // Note: These search terms don't ACTUALLY work.
    // We should be able to specify kSecMatchLimitOne, but instead we have to get
    // extra (sometimes duplicate) results, and manually filter them below.
    if (!selector.subject.empty()) {
        invariant(selector.thumbprint.empty());
        CFUniquePtr<::CFStringRef> cfsubject(::CFStringCreateWithCString(
            nullptr, selector.subject.c_str(), ::kCFStringEncodingUTF8));
        if (!cfsubject) {
            return {ErrorCodes::InvalidSSLConfiguration,
                    str::stream() << "Certificate subject name specified is not UTF-8"
                                  << selector.subject};
        }
        ::CFDictionaryAddValue(cfquery.get(), ::kSecAttrLabel, cfsubject.get());
    } else {
        invariant(!selector.thumbprint.empty());
        CFUniquePtr<::CFDataRef> cfdigest(
            ::CFDataCreate(nullptr,
                           static_cast<const uint8_t*>(selector.thumbprint.data()),
                           selector.thumbprint.size()));
        if (!cfdigest) {
            return {ErrorCodes::InvalidSSLConfiguration,
                    "Unable to create Public Key Hash from certificate thumbprint selector value"};
        }
        // Don't be fooled by the name.
        // "Public Key Hash" is actually referring to the digest of the entire Certificate.
        ::CFDictionaryAddValue(cfquery.get(), ::kSecAttrPublicKeyHash, cfdigest.get());
    }

    ::CFTypeRef identities = nullptr;
    auto status = ::SecItemCopyMatching(cfquery.get(), &identities);
    CFUniquePtr<::CFArrayRef> cfident(static_cast<::CFArrayRef>(identities));
    if (status != ::errSecSuccess) {
        return {ErrorCodes::InvalidSSLConfiguration,
                str::stream() << "Failure querying system keychain for certificate selector: "
                              << stringFromOSStatus(status)};
    }

    if (::CFGetTypeID(cfident.get()) != ::CFArrayGetTypeID()) {
        return {ErrorCodes::InvalidSSLConfiguration, "System keychain returned invalid result"};
    }

    // We should be able to return the results at this point,
    // but the search criteria above will return non-matching results in OSX 10.12 and later.
    CFUniquePtr<::CFMutableArrayRef> cfresult(
        ::CFArrayCreateMutable(nullptr, 1, &::kCFTypeArrayCallBacks));
    for (::CFIndex i = 0; i < ::CFArrayGetCount(cfident.get()); ++i) {
        auto ident = cf_cast<::SecIdentityRef>(::CFArrayGetValueAtIndex(cfident.get(), i));
        if (::CFGetTypeID(ident) != ::SecIdentityGetTypeID()) {
            continue;
        }

        ::SecCertificateRef cert = nullptr;
        status = ::SecIdentityCopyCertificate(ident, &cert);
        CFUniquePtr<::SecCertificateRef> cfcert(cert);
        if (status != ::errSecSuccess) {
            return {ErrorCodes::InvalidSSLConfiguration,
                    "Unable to retreive certificate from identity"};
        }

        if (!selector.subject.empty()) {
            // Try matching subject name to short (common name) portion of subject.
            CFUniquePtr<::CFStringRef> certSubject(
                ::SecCertificateCopySubjectSummary(cfcert.get()));
            if (!certSubject) {
                return {ErrorCodes::InvalidSSLConfiguration,
                        "Unable to retreive subject summary from identity"};
            }
            auto swSubjectSummary = toString(certSubject.get());
            if (!swSubjectSummary.isOK()) {
                return swSubjectSummary.getStatus();
            }
            if (swSubjectSummary.getValue() == selector.subject) {
                ::CFArrayAppendValue(cfresult.get(), ident);
                break;
            }

            // Try matching full subject name instead.
            auto swCertSubject = certificateGetSubject(cfcert.get());
            if (!swCertSubject.isOK()) {
                return swCertSubject.getStatus();
            }
            if (swCertSubject.getValue().toString() == selector.subject) {
                ::CFArrayAppendValue(cfresult.get(), ident);
                break;
            }
        }

        if (!selector.thumbprint.empty()) {
            CFUniquePtr<::CFDataRef> cfCertData(::SecCertificateCopyData(cfcert.get()));
            ConstDataRange certData(
                reinterpret_cast<const char*>(::CFDataGetBytePtr(cfCertData.get())),
                ::CFDataGetLength(cfCertData.get()));

            // Attempt to match SHA1 digest.
            if (SHA1Block::kHashLength == selector.thumbprint.size()) {
                const auto certSha1 = SHA1Block::computeHash({certData});
                if (!memcmp(certSha1.data(), selector.thumbprint.data(), certSha1.size())) {
                    ::CFArrayAppendValue(cfresult.get(), ident);
                    break;
                }
            }

            // Attempt to match SHA256 digest.
            if (SHA256Block::kHashLength == selector.thumbprint.size()) {
                const auto certSha256 = SHA256Block::computeHash({certData});
                if (!memcmp(certSha256.data(), selector.thumbprint.data(), certSha256.size())) {
                    ::CFArrayAppendValue(cfresult.get(), ident);
                    break;
                }
            }
        }
    }

    if (::CFArrayGetCount(cfresult.get()) == 0) {
        return {ErrorCodes::InvalidSSLConfiguration, "Certificate selector returned no results"};
    }

    return CFUniquePtr<::CFArrayRef>(cfresult.release());
}


std::string explainTrustFailure(::SecTrustRef trust, ::SecTrustResultType result) {
    const auto ret = [result](auto reason) -> std::string {
        auto ss = str::stream();
        if (result == ::kSecTrustResultDeny) {
            ss << "Certificate trust denied";
        } else if (result == ::kSecTrustResultRecoverableTrustFailure) {
            ss << "Certificate trust failure";
        } else if (result == ::kSecTrustResultFatalTrustFailure) {
            ss << "Certificate trust fatal failure";
        } else {
            static_assert(std::is_signed<uint>::value ==
                              std::is_signed<::SecTrustResultType>::value,
                          "Must cast status to same signedness");
            static_assert(sizeof(uint) >= sizeof(::SecTrustResultType),
                          "Must cast result to same or wider type");
            ss << "Certificate trust failure #" << static_cast<uint>(result);
        }
        ss << ": " << reason;
        return ss;
    };

    CFUniquePtr<::CFArrayRef> cfprops(::SecTrustCopyProperties(trust));
    if (!cfprops) {
        return ret("Unable to retreive cause for trust failure");
    }

    const auto count = ::CFArrayGetCount(cfprops.get());
    for (::CFIndex i = 0; i < count; ++i) {
        auto elem = ::CFArrayGetValueAtIndex(cfprops.get(), i);
        if (::CFGetTypeID(elem) != ::CFDictionaryGetTypeID()) {
            return ret("Unable to parse cause for trust failure");
        }
        auto dict = static_cast<::CFDictionaryRef>(elem);
        auto reason = ::CFDictionaryGetValue(dict, ::kSecPropertyTypeError);
        if (!reason) {
            continue;
        }
        if (::CFGetTypeID(reason) != ::CFStringGetTypeID()) {
            return ret("Unable to parse trust failure error");
        }
        auto swReason = toString(static_cast<::CFStringRef>(reason));
        if (!swReason.isOK()) {
            return ret("Unable to express trust failure error");
        }
        return ret(swReason.getValue());
    }

    return ret("No trust failure reason available");
}

boost::optional<std::string> getRawSNIServerName(::SSLContextRef _ssl) {
    size_t len = 0;
    auto status = ::SSLCopyRequestedPeerNameLength(_ssl, &len);
    if (status != ::errSecSuccess) {
        return boost::none;
    }
    std::string ret;
    ret.resize(len + 1);
    status = ::SSLCopyRequestedPeerName(_ssl, &ret[0], &len);
    if (status != ::errSecSuccess) {
        return boost::none;
    }
    ret.resize(len);
    return ret;
}

}  // namespace

/////////////////////////////////////////////////////////////////////////////
// SSLConnection
namespace {

class SSLConnectionApple : public SSLConnectionInterface {
public:
    SSLConnectionApple(asio::ssl::apple::Context* ctx,
                       Socket* socket,
                       ::SSLProtocolSide side,
                       std::string hostname = "",
                       std::vector<uint8_t> init = {})
        : _sock(socket), _init(std::move(init)) {
        _ssl.reset(::SSLCreateContext(nullptr, side, ::kSSLStreamType));
        uassert(ErrorCodes::InternalError, "Failed creating SSL context", _ssl);

        auto certs = ctx->certs.get();
        if (certs) {
            uassertOSStatusOK(::SSLSetCertificate(_ssl.get(), certs));
        }

        uassertOSStatusOK(::SSLSetConnection(_ssl.get(), static_cast<void*>(this)));
        uassertOSStatusOK(::SSLSetPeerID(_ssl.get(), _ssl.get(), sizeof(_ssl)));
        uassertOSStatusOK(::SSLSetIOFuncs(_ssl.get(), read_func, write_func));
        uassertOSStatusOK(::SSLSetProtocolVersionMin(_ssl.get(), ctx->protoMin));
        uassertOSStatusOK(::SSLSetProtocolVersionMax(_ssl.get(), ctx->protoMax));

        if (!hostname.empty()) {
            uassertOSStatusOK(
                ::SSLSetPeerDomainName(_ssl.get(), hostname.c_str(), hostname.size()));
        }

        // SSLHandshake will return errSSLServerAuthCompleted and let us do our own verify.
        // We'll pretend to have done that, and let our caller invoke verifyPeer later.
        uassertOSStatusOK(::SSLSetClientSideAuthenticate(_ssl.get(), ::kTryAuthenticate));
        uassertOSStatusOK(
            ::SSLSetSessionOption(_ssl.get(), ::kSSLSessionOptionBreakOnServerAuth, true));
        uassertOSStatusOK(
            ::SSLSetSessionOption(_ssl.get(), ::kSSLSessionOptionBreakOnClientAuth, true));

        ::OSStatus status;
        do {
            status = ::SSLHandshake(_ssl.get());
        } while ((status == ::errSSLServerAuthCompleted) ||
                 (status == ::errSSLClientAuthCompleted));
        uassertOSStatusOK(status, ErrorCodes::SSLHandshakeFailed);
    }

    std::string getSNIServerName() const final {
        return getRawSNIServerName(get()).value_or("");
    }

    ::SSLContextRef get() const {
        return const_cast<::SSLContextRef>(_ssl.get());
    }

private:
    static ::OSStatus write_func(::SSLConnectionRef ctx, const void* data, size_t* data_len) {
        const auto* conn = reinterpret_cast<const SSLConnectionApple*>(ctx);
        size_t len = *data_len;
        *data_len = 0;
        while (len > 0) {
            auto wrote =
                ::write(conn->_sock->rawFD(), static_cast<const char*>(data) + *data_len, len);
            if (wrote > 0) {
                *data_len += wrote;
                len -= wrote;
                continue;
            }
            return posixErrno(errno);
        }
        return ::errSecSuccess;
    }

    static ::OSStatus read_func(::SSLConnectionRef ctx, void* data, size_t* data_len) {
        auto* conn =
            const_cast<SSLConnectionApple*>(reinterpret_cast<const SSLConnectionApple*>(ctx));
        auto* dest = static_cast<char*>(data);
        size_t len = *data_len;
        *data_len = 0;
        while (len > 0) {
            if (conn->_init.size()) {
                // Consume any initial bytes first.
                auto& init = conn->_init;
                const auto mvlen = std::max(len, init.size());
                std::copy(init.begin(), init.begin() + mvlen, dest + *data_len);
                init.erase(init.begin(), init.begin() + mvlen);
                *data_len += mvlen;
                len -= mvlen;
                continue;
            }

            // Then go to the network.
            auto didread = ::read(conn->_sock->rawFD(), dest + *data_len, len);
            if (didread > 0) {
                *data_len += didread;
                len -= didread;
                continue;
            }
            return posixErrno(errno);
        }
        return ::errSecSuccess;
    }

    Socket* _sock;

    // When in server mode, _init contains any bytes read prior to
    // starting the SSL handshake process.
    // Once exhausted, this is never refilled.
    std::vector<uint8_t> _init;

    CFUniquePtr<::SSLContextRef> _ssl;
};

}  // namespace

/////////////////////////////////////////////////////////////////////////////
// SSLManager
namespace {

class SSLManagerApple : public SSLManagerInterface {
public:
    explicit SSLManagerApple(const SSLParams& params, bool isServer);

    Status initSSLContext(asio::ssl::apple::Context* context,
                          const SSLParams& params,
                          ConnectionDirection direction) final;

    SSLConnectionInterface* connect(Socket* socket) final;
    SSLConnectionInterface* accept(Socket* socket, const char* initialBytes, int len) final;

    SSLPeerInfo parseAndValidatePeerCertificateDeprecated(const SSLConnectionInterface* conn,
                                                          const std::string& remoteHost,
                                                          const HostAndPort& hostForLogging) final;

    StatusWith<SSLPeerInfo> parseAndValidatePeerCertificate(
        ::SSLContextRef conn,
        const std::string& remoteHost,
        const HostAndPort& hostForLogging) final;

    const SSLConfiguration& getSSLConfiguration() const final {
        return _sslConfiguration;
    }

    int SSL_read(SSLConnectionInterface* conn, void* buf, int num) final;
    int SSL_write(SSLConnectionInterface* conn, const void* buf, int num) final;
    int SSL_shutdown(SSLConnectionInterface* conn) final;

private:
    bool _weakValidation;
    bool _allowInvalidCertificates;
    bool _allowInvalidHostnames;
    bool _suppressNoCertificateWarning;
    asio::ssl::apple::Context _clientCtx;
    asio::ssl::apple::Context _serverCtx;

    /* _clientCA represents the CA to use when acting as a client
     * and validating remotes during outbound connections.
     * This comes from, in order, --tlsCAFile, or the system CA.
     */
    CFUniquePtr<::CFArrayRef> _clientCA;

    /* _serverCA represents the CA to use when acting as a server
     * and validating remotes during inbound connections.
     * This comes from --tlsClusterCAFile, if available,
     * otherwise it inherits from _clientCA.
     */
    CFUniquePtr<::CFArrayRef> _serverCA;

    SSLConfiguration _sslConfiguration;
};

SSLManagerApple::SSLManagerApple(const SSLParams& params, bool isServer)
    : _weakValidation(params.sslWeakCertificateValidation),
      _allowInvalidCertificates(params.sslAllowInvalidCertificates),
      _allowInvalidHostnames(params.sslAllowInvalidHostnames),
      _suppressNoCertificateWarning(params.suppressNoTLSPeerCertificateWarning) {

    uassertStatusOK(initSSLContext(&_clientCtx, params, ConnectionDirection::kOutgoing));
    if (_clientCtx.certs) {
        _sslConfiguration.clientSubjectName =
            uassertStatusOK(certificateGetSubject(_clientCtx.certs.get()));
    }

    if (isServer) {
        uassertStatusOK(initSSLContext(&_serverCtx, params, ConnectionDirection::kIncoming));
        if (_serverCtx.certs) {
            _sslConfiguration.serverSubjectName = uassertStatusOK(certificateGetSubject(
                _serverCtx.certs.get(), &_sslConfiguration.serverCertificateExpirationDate));
            static auto task =
                CertificateExpirationMonitor(_sslConfiguration.serverCertificateExpirationDate);
        }
    }

    if (!params.sslCAFile.empty()) {
        auto ca = uassertStatusOK(loadPEM(params.sslCAFile, "", kLoadPEMStripKeys));
        _clientCA = std::move(ca);
        _sslConfiguration.hasCA = _clientCA && ::CFArrayGetCount(_clientCA.get());
    }

    if (!params.sslCertificateSelector.empty() || !params.sslClusterCertificateSelector.empty()) {
        // By using the system keychain, we acknowledge it exists.
        _sslConfiguration.hasCA = true;
    }

    if (!_clientCA) {
        // No explicit CA was specified, use the Keychain CA explicitly on client connects,
        // even though we're going to pretend it doesn't exist on server.
        ::CFArrayRef certs = nullptr;
        uassertOSStatusOK(SecTrustCopyAnchorCertificates(&certs));
        _clientCA.reset(certs);
    }

    if (!params.sslClusterCAFile.empty()) {
        auto ca = uassertStatusOK(loadPEM(params.sslClusterCAFile, "", kLoadPEMStripKeys));
        _serverCA = std::move(ca);
    } else {
        // No inbound CA specified, share a reference with outbound CA.
        auto ca = _clientCA.get();
        ::CFRetain(ca);
        _serverCA.reset(ca);
    }
}

StatusWith<std::pair<::SSLProtocol, ::SSLProtocol>> parseProtocolRange(const SSLParams& params) {
    // Map disabled protocols to range.
    bool tls10 = true, tls11 = true, tls12 = true;
    for (const SSLParams::Protocols& protocol : params.sslDisabledProtocols) {
        if (protocol == SSLParams::Protocols::TLS1_0) {
            tls10 = false;
        } else if (protocol == SSLParams::Protocols::TLS1_1) {
            tls11 = false;
        } else if (protocol == SSLParams::Protocols::TLS1_2) {
            tls12 = false;
        } else if (protocol == SSLParams::Protocols::TLS1_3) {
            // By ignoring this value, we are disabling support until we have access to the
            // modern library.
        } else {
            return {ErrorCodes::InvalidSSLConfiguration, "Unknown disabled TLS protocol version"};
        }
    }
    // Throw out the invalid cases.
    if (tls10 && !tls11 && tls12) {
        return {ErrorCodes::InvalidSSLConfiguration,
                "Can not disable TLS 1.1 while leaving 1.0 and 1.2 enabled"};
    }
    if (!tls10 && !tls11 && !tls12) {
        return {ErrorCodes::InvalidSSLConfiguration, "All valid TLS modes disabled"};
    }

    auto protoMin = tls10 ? ::kTLSProtocol1 : tls11 ? ::kTLSProtocol11 : ::kTLSProtocol12;
    auto protoMax = tls12 ? ::kTLSProtocol12 : tls11 ? ::kTLSProtocol11 : ::kTLSProtocol1;

    return std::pair<::SSLProtocol, ::SSLProtocol>(protoMin, protoMax);
}

Status SSLManagerApple::initSSLContext(asio::ssl::apple::Context* context,
                                       const SSLParams& params,
                                       ConnectionDirection direction) {
    // Options.
    context->allowInvalidHostnames = _allowInvalidHostnames;

    // Protocol Version.
    const auto swProto = parseProtocolRange(params);
    if (!swProto.isOK()) {
        return swProto.getStatus();
    }
    const auto proto = swProto.getValue();
    context->protoMin = proto.first;
    context->protoMax = proto.second;

    // Certificate.
    const auto selectCertificate = [&context,
                                    direction](const SSLParams::CertificateSelector& selector,
                                               const std::string& PEMFile,
                                               const std::string& PEMPass) -> Status {
        if (!selector.empty()) {
            auto swCerts = copyMatchingCertificate(selector, direction);
            if (!swCerts.isOK()) {
                return swCerts.getStatus();
            }
            context->certs = std::move(swCerts.getValue());
            return Status::OK();
        }
        if (!PEMFile.empty()) {
            auto swCerts = loadPEM(PEMFile, PEMPass);
            if (!swCerts.isOK()) {
                return swCerts.getStatus();
            }
            context->certs = std::move(swCerts.getValue());
            return Status::OK();
        }
        return Status::OK();
    };

    if (direction == ConnectionDirection::kOutgoing) {
        if (params.tlsWithholdClientCertificate) {
            return Status::OK();
        }

        const auto status = selectCertificate(
            params.sslClusterCertificateSelector, params.sslClusterFile, params.sslClusterPassword);
        if (context->certs || !status.isOK()) {
            return status;
        }
        // Fallthrough...
    }

    return selectCertificate(
        params.sslCertificateSelector, params.sslPEMKeyFile, params.sslPEMKeyPassword);
}

SSLConnectionInterface* SSLManagerApple::connect(Socket* socket) {
    return new SSLConnectionApple(
        &_clientCtx, socket, ::kSSLClientSide, socket->remoteAddr().hostOrIp());
}

SSLConnectionInterface* SSLManagerApple::accept(Socket* socket, const char* initialBytes, int len) {
    std::vector<uint8_t> init;
    const auto* p = reinterpret_cast<const uint8_t*>(initialBytes);
    init.insert(init.end(), p, p + len);
    return new SSLConnectionApple(&_serverCtx, socket, ::kSSLServerSide, "", init);
}

SSLPeerInfo SSLManagerApple::parseAndValidatePeerCertificateDeprecated(
    const SSLConnectionInterface* conn,
    const std::string& remoteHost,
    const HostAndPort& hostForLogging) {
    auto ssl = checked_cast<const SSLConnectionApple*>(conn)->get();

    auto swPeerSubjectName = parseAndValidatePeerCertificate(ssl, remoteHost, hostForLogging);
    // We can't use uassertStatusOK here because we need to throw a NetworkException.
    if (!swPeerSubjectName.isOK()) {
        throwSocketError(SocketErrorKind::CONNECT_ERROR, swPeerSubjectName.getStatus().reason());
    }
    return swPeerSubjectName.getValue();
}

StatusWith<TLSVersion> mapTLSVersion(SSLContextRef ssl) {
    ::SSLProtocol protocol;

    uassertOSStatusOK(::SSLGetNegotiatedProtocolVersion(ssl, &protocol));

    switch (protocol) {
        case kTLSProtocol1:
            return TLSVersion::kTLS10;
        case kTLSProtocol11:
            return TLSVersion::kTLS11;
        case kTLSProtocol12:
            return TLSVersion::kTLS12;
        default:  // Some system headers may define additional protocols, so suppress warnings.
            return TLSVersion::kUnknown;
    }
}


StatusWith<SSLPeerInfo> SSLManagerApple::parseAndValidatePeerCertificate(
    ::SSLContextRef ssl, const std::string& remoteHost, const HostAndPort& hostForLogging) {
    auto sniName = getRawSNIServerName(ssl);

    // Record TLS version stats
    auto tlsVersionStatus = mapTLSVersion(ssl);
    if (!tlsVersionStatus.isOK()) {
        return tlsVersionStatus.getStatus();
    }

    recordTLSVersion(tlsVersionStatus.getValue(), hostForLogging);

    /* While we always have a system CA via the Keychain,
     * we'll pretend not to in terms of validation if the server
     * was started using a PEM file (legacy mode).
     *
     * When a certificate selector is used, we'll override hasCA to true
     * so that the validation path runs anyway.
     */
    if (!_sslConfiguration.hasCA && isSSLServer) {
        return SSLPeerInfo(sniName);
    }

    const auto badCert = [&](StringData msg, bool warn = false) -> StatusWith<SSLPeerInfo> {
        constexpr StringData prefix = "SSL peer certificate validation failed: "_sd;
        if (warn) {
            warning() << prefix << msg;
            return SSLPeerInfo(sniName);
        } else {
            std::string m = str::stream() << prefix << msg << "; connection rejected";
            error() << m;
            return Status(ErrorCodes::SSLHandshakeFailed, m);
        }
    };

    ::SecTrustRef trust = nullptr;
    const auto status = ::SSLCopyPeerTrust(ssl, &trust);
    CFUniquePtr<::SecTrustRef> cftrust(trust);
    if ((status != ::errSecSuccess) || (!cftrust)) {
        if (_weakValidation && _suppressNoCertificateWarning) {
            return SSLPeerInfo(sniName);
        } else {
            if (status == ::errSecSuccess) {
                return badCert(str::stream() << "no SSL certificate provided by peer: "
                                             << stringFromOSStatus(status),
                               _weakValidation);
            } else {
                return badCert(str::stream() << "Unable to retreive SSL trust from peer: "
                                             << stringFromOSStatus(status),
                               _weakValidation);
            }
        }
    }

    // When remoteHost is empty, it means we're handling an Inbound connection.
    // In that case, we in a server role, so use the _serverCA,
    // otherwise we're in a client role, so use that.
    auto ca = remoteHost.empty() ? _serverCA.get() : _clientCA.get();
    if (ca) {
        auto status = ::SecTrustSetAnchorCertificates(cftrust.get(), ca);
        if (status == ::errSecSuccess) {
            status = ::SecTrustSetAnchorCertificatesOnly(cftrust.get(), true);
        }
        if (status != ::errSecSuccess) {
            return badCert(str::stream() << "Unable to bind CA to trust chain: "
                                         << stringFromOSStatus(status),
                           _weakValidation);
        }
    }

    auto result = ::kSecTrustResultInvalid;
    uassertOSStatusOK(::SecTrustEvaluate(cftrust.get(), &result), ErrorCodes::SSLHandshakeFailed);
    if ((result != ::kSecTrustResultProceed) && (result != ::kSecTrustResultUnspecified)) {
        return badCert(explainTrustFailure(cftrust.get(), result), _allowInvalidCertificates);
    }

    auto cert = ::SecTrustGetCertificateAtIndex(cftrust.get(), 0);
    if (!cert) {
        return badCert("no SSL certificate found in trust container", _weakValidation);
    }

    CFUniquePtr<::CFMutableArrayRef> oids(
        ::CFArrayCreateMutable(nullptr, remoteHost.empty() ? 3 : 2, &::kCFTypeArrayCallBacks));
    ::CFArrayAppendValue(oids.get(), ::kSecOIDX509V1SubjectName);
    ::CFArrayAppendValue(oids.get(), ::kSecOIDSubjectAltName);
    if (remoteHost.empty()) {
        ::CFArrayAppendValue(oids.get(), kMongoDBRolesOID);
    }

    ::CFErrorRef err = nullptr;
    CFUniquePtr<::CFDictionaryRef> cfdict(::SecCertificateCopyValues(cert, oids.get(), &err));
    CFUniquePtr<::CFErrorRef> cferror(err);
    if (cferror) {
        return badCert(str::stream() << cferror.get(), _weakValidation);
    }

    // Extract SubjectName into a human readable string.
    auto swPeerSubjectName = extractSubjectName(cfdict.get());
    if (!swPeerSubjectName.isOK()) {
        return swPeerSubjectName.getStatus();
    }
    const auto peerSubjectName = std::move(swPeerSubjectName.getValue());
    LOG(2) << "Accepted TLS connection from peer: " << peerSubjectName;

    if (remoteHost.empty()) {
        // If this is an SSL server context (on a mongod/mongos)
        // parse any client roles out of the client certificate.
        auto swPeerCertificateRoles = parsePeerRoles(cfdict.get());
        if (!swPeerCertificateRoles.isOK()) {
            return swPeerCertificateRoles.getStatus();
        }
        return SSLPeerInfo(peerSubjectName, sniName, std::move(swPeerCertificateRoles.getValue()));
    }

    // If this is an SSL client context (on a MongoDB server or client)
    // perform hostname validation of the remote server
    bool sanMatch = false;
    bool cnMatch = false;
    StringBuilder certErr;
    certErr << "The server certificate does not match the host name. "
            << "Hostname: " << remoteHost << " does not match ";

    // Attempt to retreive "Subject Alternative Name"
    std::vector<std::string> sans;
    auto swSANs = extractSubjectAlternateNames(cfdict.get());
    if (swSANs.isOK()) {
        sans = std::move(swSANs.getValue());
    }

    if (!sans.empty()) {
        certErr << "SAN(s): ";
        for (auto& san : sans) {
            if (hostNameMatchForX509Certificates(remoteHost, san)) {
                sanMatch = true;
                break;
            }
            certErr << san << " ";
        }

    } else {
        auto swCN = peerSubjectName.getOID(kOID_CommonName);
        if (swCN.isOK()) {
            auto commonName = std::move(swCN.getValue());
            if (hostNameMatchForX509Certificates(remoteHost, commonName)) {
                cnMatch = true;
            }
            certErr << "CN: " << commonName;
        } else {
            certErr << "No Common Name (CN) or Subject Alternate Names (SAN) found";
        }
    }

    if (!sanMatch && !cnMatch) {
        const auto msg = certErr.str();
        if (_allowInvalidCertificates || _allowInvalidHostnames || isUnixDomainSocket(remoteHost)) {
            warning() << msg;
        } else {
            error() << msg;
            return Status(ErrorCodes::SSLHandshakeFailed, msg);
        }
    }

    return SSLPeerInfo(peerSubjectName);
}

int SSLManagerApple::SSL_read(SSLConnectionInterface* conn, void* buf, int num) {
    auto ssl = checked_cast<SSLConnectionApple*>(conn)->get();
    size_t read = 0;
    uassertOSStatusOK(::SSLRead(ssl, static_cast<uint8_t*>(buf), num, &read),
                      SocketErrorKind::RECV_ERROR);
    return read;
}

int SSLManagerApple::SSL_write(SSLConnectionInterface* conn, const void* buf, int num) {
    auto ssl = checked_cast<SSLConnectionApple*>(conn)->get();
    size_t written = 0;
    uassertOSStatusOK(::SSLWrite(ssl, static_cast<const uint8_t*>(buf), num, &written),
                      SocketErrorKind::SEND_ERROR);
    return written;
}

int SSLManagerApple::SSL_shutdown(SSLConnectionInterface* conn) {
    auto ssl = checked_cast<SSLConnectionApple*>(conn)->get();
    const auto status = ::SSLClose(ssl);
    if (status == ::errSSLWouldBlock) {
        return 0;
    }
    uassertOSStatusOK(status, ErrorCodes::SocketException);
    return 1;
}

}  // namespace

/////////////////////////////////////////////////////////////////////////////

// Global variable indicating if this is a server or a client instance
bool isSSLServer = false;

namespace {
SimpleMutex sslManagerMtx;
SSLManagerInterface* theSSLManager = nullptr;
}  // namespace

std::unique_ptr<SSLManagerInterface> SSLManagerInterface::create(const SSLParams& params,
                                                                 bool isServer) {
    return stdx::make_unique<SSLManagerApple>(params, isServer);
}

MONGO_INITIALIZER(SSLManager)(InitializerContext*) {
    kMongoDBRolesOID = ::CFStringCreateWithCString(
        nullptr, mongodbRolesOID.identifier.c_str(), ::kCFStringEncodingUTF8);

    stdx::lock_guard<SimpleMutex> lck(sslManagerMtx);
    if (!isSSLServer || (sslGlobalParams.sslMode.load() != SSLParams::SSLMode_disabled)) {
        theSSLManager = new SSLManagerApple(sslGlobalParams, isSSLServer);
    }
    return Status::OK();
}

}  // namespace mongo

mongo::SSLManagerInterface* mongo::getSSLManager() {
    stdx::lock_guard<SimpleMutex> lck(sslManagerMtx);
    if (theSSLManager) {
        return theSSLManager;
    }
    return nullptr;
}
