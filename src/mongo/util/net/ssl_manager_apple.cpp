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

// CFAbsoluteTime and X.509 is relative to Jan 1 2001 00:00:00 GMT
// Unix Epoch (and thereby Date_t) is relative to Jan 1, 1970 00:00:00 GMT
static const ::CFAbsoluteTime k20010101_000000_GMT = 978307200;

::CFStringRef kMongoDBRolesOID = nullptr;

StatusWith<std::string> toString(::CFStringRef str) {
    const auto len =
        ::CFStringGetMaximumSizeForEncoding(::CFStringGetLength(str), ::kCFStringEncodingUTF8);
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

std::ostringstream& operator<<(std::ostringstream& ss, ::OSStatus status) {
    static_assert(std::is_signed<int>::value == std::is_signed<::OSStatus>::value,
                  "Must cast status to same signedness");
    static_assert(sizeof(int) >= sizeof(::OSStatus), "Must cast status to same or wider type");
    auto swStr = toString(status);
    if (swStr.isOK()) {
        ss << swStr.getValue();
    } else {
        ss << "Unknown error: " << static_cast<int>(status);
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

StatusWith<std::string> mapSubjectLabel(::CFStringRef label) {
    if (!::CFStringCompare(label, ::kSecOIDCommonName, 0)) {
        return {"CN"};
    } else if (!::CFStringCompare(label, ::kSecOIDCountryName, 0)) {
        return {"C"};
    } else if (!::CFStringCompare(label, ::kSecOIDStateProvinceName, 0)) {
        return {"ST"};
    } else if (!::CFStringCompare(label, ::kSecOIDLocalityName, 0)) {
        return {"L"};
    } else if (!::CFStringCompare(label, ::kSecOIDOrganizationName, 0)) {
        return {"O"};
    } else if (!::CFStringCompare(label, ::kSecOIDOrganizationalUnitName, 0)) {
        return {"OU"};
    } else if (!::CFStringCompare(label, ::kSecOIDStreetAddress, 0)) {
        return {"STREET"};
    } else if (!::CFStringCompare(label, CFSTR("0.9.2342.19200300.100.1.25"), 0)) {
        return {"DC"};
    } else if (!::CFStringCompare(label, CFSTR("0.9.2342.19200300.100.1.1"), 0)) {
        return {"UID"};
    }
    // RFC 2253 specifies #hexstring encoding for unknown OIDs,
    // however for backward compatibility purposes, we omit these.
    return {ErrorCodes::InvalidSSLConfiguration, str::stream() << "Unknown OID: " << label};
}

// Encode a raw DER subject sequence into a human readable subject name (RFC 2253).
StatusWith<std::string> extractSubjectName(::CFDictionaryRef dict) {
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
    StringBuilder ret;
    for (auto i = nElems; i; --i) {
        auto elem = reinterpret_cast<::CFDictionaryRef>(::CFArrayGetValueAtIndex(elems, i - 1));
        invariant(elem);
        if (::CFGetTypeID(elem) != ::CFDictionaryGetTypeID()) {
            return {ErrorCodes::InvalidSSLConfiguration,
                    "Subject name element is not a dictionary"};
        }

        auto swLabel = extractDictionaryValue<::CFStringRef>(elem, ::kSecPropertyKeyLabel);
        if (!swLabel.isOK()) {
            return swLabel.getStatus();
        }
        auto swLabelStr = mapSubjectLabel(swLabel.getValue());
        if (!swLabelStr.isOK()) {
            return swLabelStr.getStatus();
        }

        auto swValue = extractDictionaryValue<::CFStringRef>(elem, ::kSecPropertyKeyValue);
        if (!swValue.isOK()) {
            return swValue.getStatus();
        }
        auto swValueStr = toString(swValue.getValue());
        if (!swValueStr.isOK()) {
            return swValueStr.getStatus();
        }

        ret << swLabelStr.getValue() << '=' << swValueStr.getValue();
        if (i > 1) {
            ret << ",";
        }
    }
    return ret.str();
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
                                    << (passphrase.empty() ? "" : " with passphrase: ")
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
        return retFail(str::stream() << "Failing importing certificate(s): " << status);
    }

    auto count = ::CFArrayGetCount(cfcerts.get());
    if ((count > 0) && (mode == kLoadPEMBindIdentities)) {
        // Turn Certificate/Key pairs into identities.
        CFUniquePtr<::CFMutableArrayRef> bind(
            ::CFArrayCreateMutable(nullptr, count, &kCFTypeArrayCallBacks));
        for (::CFIndex i = 0; i < count; ++i) {
            auto elem = ::CFArrayGetValueAtIndex(cfcerts.get(), i);
            invariant(elem);
            const auto type = ::CFGetTypeID(elem);
            if (type == ::SecIdentityGetTypeID()) {
                // Our import had a proper identity in it, ready to go.
                ::CFArrayAppendValue(bind.get(), elem);
                continue;
            }
            if (type != ::SecCertificateGetTypeID()) {
                continue;
            }

            // Attempt to match the certificate to a private key in the aggregate we just imported.
            CFUniquePtr<::SecIdentityRef> cfid;
            for (::CFIndex j = 0; j < count; ++j) {
                auto key = ::CFArrayGetValueAtIndex(cfcerts.get(), j);
                invariant(key);
                if (::CFGetTypeID(key) != ::SecKeyGetTypeID()) {
                    continue;
                }
                auto id =
                    ::SecIdentityCreate(nullptr,
                                        static_cast<::SecCertificateRef>(const_cast<void*>(elem)),
                                        static_cast<::SecKeyRef>(const_cast<void*>(key)));
                if (id) {
                    cfid.reset(id);
                    break;
                }
            }
            if (cfid) {
                ::CFArrayAppendValue(bind.get(), cfid.get());
            } else {
                ::CFArrayAppendValue(bind.get(), elem);
            }
        }
        // Reencapsulate to allow the inner type to change.
        cfcerts.reset(bind.release());
        count = ::CFArrayGetCount(cfcerts.get());
    }

    if ((count > 0) && (mode == kLoadPEMStripKeys)) {
        // Strip unpaired keys and identities.
        CFUniquePtr<::CFMutableArrayRef> strip(
            ::CFArrayCreateMutable(nullptr, count, &kCFTypeArrayCallBacks));
        for (::CFIndex i = 0; i < count; ++i) {
            auto elem = ::CFArrayGetValueAtIndex(cfcerts.get(), i);
            if (!elem) {
                continue;
            }
            const auto type = ::CFGetTypeID(elem);
            if (type == ::SecCertificateGetTypeID()) {
                // Preserve Certificates.
                ::CFArrayAppendValue(strip.get(), elem);
                continue;
            }
            if (type != ::SecIdentityGetTypeID()) {
                continue;
            }

            // Extract public certificate from Identity.
            ::SecCertificateRef cert = nullptr;
            const auto status = ::SecIdentityCopyCertificate(
                static_cast<::SecIdentityRef>(const_cast<void*>(elem)), &cert);
            CFUniquePtr<::SecCertificateRef> cfcert(cert);
            if (status != ::errSecSuccess) {
                return {ErrorCodes::InternalError,
                        str::stream() << "Unable to extract certificate from identity: " << status};
            }
            ::CFArrayAppendValue(strip.get(), cfcert.get());
        }
        // Reencapsulate to allow the inner type to change.
        cfcerts.reset(strip.release());
        count = ::CFArrayGetCount(cfcerts.get());
    }

    if (count <= 0) {
        return {ErrorCodes::InvalidSSLConfiguration,
                str::stream() << "PEM file '" << keyfilepath << "' has no certificates"};
    }

    // Rewrap the return to be the non-mutable type.
    return std::move(cfcerts);
}

StatusWith<std::string> loadAndValidatePEM(const std::string& key,
                                           const std::string& pass,
                                           Date_t* expire = nullptr) {
    auto swCerts = loadPEM(key, pass);
    if (!swCerts.isOK()) {
        return swCerts.getStatus();
    }

    auto certs = std::move(swCerts.getValue());
    if (::CFArrayGetCount(certs.get()) <= 0) {
        return {ErrorCodes::InvalidSSLConfiguration, "No certificates in certificate list"};
    }

    auto root = ::CFArrayGetValueAtIndex(certs.get(), 0);
    if (!root || (::CFGetTypeID(root) != ::SecIdentityGetTypeID())) {
        return {ErrorCodes::InvalidSSLConfiguration, "Root certificate not an identity pair"};
    }

    ::SecCertificateRef idcert = nullptr;
    auto status = ::SecIdentityCopyCertificate(
        static_cast<::SecIdentityRef>(const_cast<void*>(root)), &idcert);
    if (status != ::errSecSuccess) {
        return {ErrorCodes::InvalidSSLConfiguration,
                str::stream() << "Unable to get certificate from identity: " << status};
    }
    CFUniquePtr<::SecCertificateRef> cert(idcert);

    // Fetch expiry range and full subject name.
    CFUniquePtr<::CFMutableArrayRef> oids(
        ::CFArrayCreateMutable(nullptr, expire ? 3 : 1, &::kCFTypeArrayCallBacks));
    ::CFArrayAppendValue(oids.get(), ::kSecOIDX509V1SubjectName);
    if (expire) {
        ::CFArrayAppendValue(oids.get(), ::kSecOIDX509V1ValidityNotBefore);
        ::CFArrayAppendValue(oids.get(), ::kSecOIDX509V1ValidityNotAfter);
    }

    ::CFErrorRef cferror = nullptr;
    CFUniquePtr<::CFDictionaryRef> cfdict(
        ::SecCertificateCopyValues(cert.get(), oids.get(), &cferror));
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
};

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
        size_t len = 0;
        auto status = ::SSLCopyRequestedPeerNameLength(_ssl.get(), &len);
        if (status != ::errSecSuccess) {
            return "";
        }
        std::string ret;
        ret.resize(len + 1);
        status = ::SSLCopyRequestedPeerName(_ssl.get(), &ret[0], &len);
        if (status != ::errSecSuccess) {
            return "";
        }
        ret.resize(len);
        return ret;
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
                                                          const std::string& remoteHost) final;

    StatusWith<boost::optional<SSLPeerInfo>> parseAndValidatePeerCertificate(
        ::SSLContextRef conn, const std::string& remoteHost) final;

    const SSLConfiguration& getSSLConfiguration() const final {
        return _sslConfiguration;
    }

    int SSL_read(SSLConnectionInterface* conn, void* buf, int num) final;
    int SSL_write(SSLConnectionInterface* conn, const void* buf, int num) final;
    int SSL_shutdown(SSLConnectionInterface* conn) final;

private:
    Status _validatePEMs(const SSLParams& params, bool isServer);

    bool _weakValidation;
    bool _allowInvalidCertificates;
    bool _allowInvalidHostnames;
    asio::ssl::apple::Context _clientCtx;
    asio::ssl::apple::Context _serverCtx;
    CFUniquePtr<::CFArrayRef> _ca;
    SSLConfiguration _sslConfiguration;
};

SSLManagerApple::SSLManagerApple(const SSLParams& params, bool isServer)
    : _weakValidation(params.sslWeakCertificateValidation),
      _allowInvalidCertificates(params.sslAllowInvalidCertificates),
      _allowInvalidHostnames(params.sslAllowInvalidHostnames) {
    uassertStatusOK(initSSLContext(&_clientCtx, params, ConnectionDirection::kOutgoing));
    if (isServer) {
        uassertStatusOK(initSSLContext(&_serverCtx, params, ConnectionDirection::kIncoming));
    }
    uassertStatusOK(_validatePEMs(params, isServer));

    if (!params.sslCAFile.empty()) {
        auto ca = uassertStatusOK(loadPEM(params.sslCAFile, "", kLoadPEMStripKeys));
        _ca = std::move(ca);
        _sslConfiguration.hasCA = _ca && ::CFArrayGetCount(_ca.get());
    }
}

Status SSLManagerApple::_validatePEMs(const SSLParams& params, bool isServer) {
    // pick the certificate for use in outgoing connections,
    std::string clientPEM, clientPassword;
    if (!isServer || params.sslClusterFile.empty()) {
        // We are either a client, or a server without a cluster key,
        // so use the PEM key file, if specified.
        clientPEM = params.sslPEMKeyFile;
        clientPassword = params.sslPEMKeyPassword;
    } else {
        // We are a server with a cluster key, so use the cluster key file
        clientPEM = params.sslClusterFile;
        clientPassword = params.sslClusterPassword;
    }
    if (!clientPEM.empty()) {
        auto swSubject = loadAndValidatePEM(clientPEM, clientPassword);
        if (!swSubject.isOK()) {
            return swSubject.getStatus();
        }
        _sslConfiguration.clientSubjectName = swSubject.getValue();
    }

    if (isServer) {
        auto swSubject = loadAndValidatePEM(params.sslPEMKeyFile,
                                            params.sslPEMKeyPassword,
                                            &_sslConfiguration.serverCertificateExpirationDate);
        if (!swSubject.isOK()) {
            return swSubject.getStatus();
        }
        _sslConfiguration.serverSubjectName = swSubject.getValue();

        static auto task =
            CertificateExpirationMonitor(_sslConfiguration.serverCertificateExpirationDate);
    }

    return Status::OK();
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
    // Protocol Version.
    const auto swProto = parseProtocolRange(params);
    if (!swProto.isOK()) {
        return swProto.getStatus();
    }
    const auto proto = swProto.getValue();
    context->protoMin = proto.first;
    context->protoMax = proto.second;

    // Certificate.
    if (direction == ConnectionDirection::kOutgoing && !params.sslClusterFile.empty()) {
        auto swCertificates = loadPEM(params.sslClusterFile, params.sslClusterPassword);
        if (!swCertificates.isOK()) {
            return swCertificates.getStatus();
        }
        context->certs = std::move(swCertificates.getValue());
    } else if (!params.sslPEMKeyFile.empty()) {
        auto swCertificates = loadPEM(params.sslPEMKeyFile, params.sslPEMKeyPassword);
        if (!swCertificates.isOK()) {
            return swCertificates.getStatus();
        }
        context->certs = std::move(swCertificates.getValue());
    }

    return Status::OK();
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
    const SSLConnectionInterface* conn, const std::string& remoteHost) {
    auto ssl = checked_cast<const SSLConnectionApple*>(conn)->get();

    auto swPeerSubjectName = parseAndValidatePeerCertificate(ssl, remoteHost);
    // We can't use uassertStatusOK here because we need to throw a NetworkException.
    if (!swPeerSubjectName.isOK()) {
        throwSocketError(SocketErrorKind::CONNECT_ERROR, swPeerSubjectName.getStatus().reason());
    }
    return swPeerSubjectName.getValue().get_value_or(SSLPeerInfo());
}

StatusWith<boost::optional<SSLPeerInfo>> SSLManagerApple::parseAndValidatePeerCertificate(
    ::SSLContextRef ssl, const std::string& remoteHost) {

    /* While we always have a system CA via the Keychain,
     * we'll pretend not to in terms of validation if the server
     * was started using a PEM file (legacy mode).
     *
     * When a certificate selector is used, we'll override hasCA to true
     * so that the validation path runs anyway.
     */
    if (!_sslConfiguration.hasCA && isSSLServer) {
        return {boost::none};
    }

    const auto badCert = [](StringData msg,
                            bool warn = false) -> StatusWith<boost::optional<SSLPeerInfo>> {
        constexpr StringData prefix = "SSL peer certificate validation failed: "_sd;
        if (warn) {
            warning() << prefix << msg;
            return {boost::none};
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
        return badCert(str::stream() << "Unable to retreive SSL trust from peer: " << status,
                       _weakValidation);
    }

    if (_ca) {
        auto status = ::SecTrustSetAnchorCertificates(cftrust.get(), _ca.get());
        if (status == ::errSecSuccess) {
            status = ::SecTrustSetAnchorCertificatesOnly(cftrust.get(), true);
        }
        if (status != ::errSecSuccess) {
            return badCert(str::stream() << "Unable to bind CA to trust chain: " << status,
                           _weakValidation);
        }
    }

    auto result = ::kSecTrustResultInvalid;
    uassertOSStatusOK(::SecTrustEvaluate(cftrust.get(), &result), ErrorCodes::SSLHandshakeFailed);
    if ((result != ::kSecTrustResultProceed) && (result != ::kSecTrustResultUnspecified)) {
        if (result == ::kSecTrustResultDeny) {
            return badCert("Certificate trust denied", _allowInvalidCertificates);
        } else if (result == ::kSecTrustResultRecoverableTrustFailure) {
            return badCert("Certificate trust failed (recoverably)", _allowInvalidCertificates);
        } else if (result == ::kSecTrustResultFatalTrustFailure) {
            return badCert("Certificate trust failure", _allowInvalidCertificates);
        } else {
            static_assert(std::is_signed<uint>::value ==
                              std::is_signed<::SecTrustResultType>::value,
                          "Must cast status to same signedness");
            static_assert(sizeof(uint) >= sizeof(::SecTrustResultType),
                          "Must cast result to same or wider type");
            return badCert(str::stream() << "Unknown cause: " << static_cast<uint>(result),
                           _allowInvalidCertificates);
        }
    }

    auto cert = ::SecTrustGetCertificateAtIndex(cftrust.get(), 0);
    if (!cert) {
        return badCert("no SSL certificate provided by peer", _weakValidation);
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
        return boost::make_optional(
            SSLPeerInfo(peerSubjectName, std::move(swPeerCertificateRoles.getValue())));
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

    } else if (peerSubjectName.find("CN=") != std::string::npos) {
        const auto cnBegin = peerSubjectName.find("CN=") + 3;
        auto cnEnd = peerSubjectName.find(",", cnBegin);
        if (cnEnd == std::string::npos) {
            cnEnd = peerSubjectName.size();
        }
        const auto commonName = peerSubjectName.substr(cnBegin, cnEnd - cnBegin);
        cnMatch = hostNameMatchForX509Certificates(remoteHost, commonName);
        certErr << "CN: " << commonName;

    } else {
        certErr << "No Common Name (CN) or Subject Alternate Names (SAN) found";
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

    return boost::make_optional(SSLPeerInfo(peerSubjectName, stdx::unordered_set<RoleName>()));
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
