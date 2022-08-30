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


#include "mongo/platform/basic.h"

#include "mongo/util/net/ssl_manager.h"

#include <boost/algorithm/string.hpp>
#include <string>
#include <vector>

#include "mongo/base/init.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/client/internal_auth.h"
#include "mongo/config.h"
#include "mongo/db/auth/sasl_command_constants.h"
#include "mongo/db/commands/server_status.h"
#include "mongo/logv2/log.h"
#include "mongo/platform/overflow_arithmetic.h"
#include "mongo/transport/session.h"
#include "mongo/transport/transport_layer_asio.h"
#include "mongo/util/ctype.h"
#include "mongo/util/hex.h"
#include "mongo/util/icu.h"
#include "mongo/util/net/ssl_options.h"
#include "mongo/util/net/ssl_parameters_gen.h"
#include "mongo/util/str.h"
#include "mongo/util/synchronized_value.h"
#include "mongo/util/text.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kNetwork


namespace mongo {

SSLManagerCoordinator* theSSLManagerCoordinator;

namespace {

// This function returns true if the character is supposed to be escaped according to the rules
// in RFC4514. The exception to the RFC the space character ' ' and the '#', because we've not
// required users to escape spaces or sharps in DNs in the past.
inline bool isEscaped(char ch) {
    switch (ch) {
        case '"':
        case '+':
        case ',':
        case ';':
        case '<':
        case '>':
        case '\\':
            return true;
        default:
            return false;
    }
}

// These characters may appear escaped in a string or not, but must not appear as the first
// character.
inline bool isMayBeEscaped(char ch) {
    switch (ch) {
        case ' ':
        case '#':
        case '=':
            return true;
        default:
            return false;
    }
}

/*
 * This class parses out the components of a DN according to RFC4514.
 *
 * It takes in a StringData to the DN to be parsed, the buffer containing the StringData
 * must remain in scope for the duration that it is being parsed.
 */
class RFC4514Parser {
public:
    explicit RFC4514Parser(StringData sd) : _str(sd), _it(_str.begin()) {}

    std::string extractAttributeName();

    enum ValueTerminator {
        NewRDN,      // The value ended in ','
        MultiValue,  // The value ended in '+'
        Done         // The value ended with the end of the string
    };

    // Returns a decoded string representing one value in an RDN, and the way the value was
    // terminated.
    std::pair<std::string, ValueTerminator> extractValue();

    bool done() const {
        return _it == _str.end();
    }

    void skipSpaces() {
        while (!done() && _cur() == ' ') {
            _advance();
        }
    }

private:
    char _cur() const {
        uassert(51036, "Overflowed string while parsing DN string", !done());
        return *_it;
    }

    char _advance() {
        invariant(!done());
        ++_it;
        return done() ? '\0' : _cur();
    }

    StringData _str;
    StringData::const_iterator _it;
};

// Parses an attribute name according to the rules for the "descr" type defined in
// https://tools.ietf.org/html/rfc4512
std::string RFC4514Parser::extractAttributeName() {
    StringBuilder sb;

    auto ch = _cur();
    std::function<bool(char ch)> characterCheck;
    // If the first character is a digit, then this is an OID and can only contain
    // numbers and '.'
    if (ctype::isDigit(ch)) {
        characterCheck = [](char ch) { return ctype::isDigit(ch) || ch == '.'; };
        // If the first character is an alpha, then this is a short name and can only
        // contain alpha/digit/hyphen characters.
    } else if (ctype::isAlpha(ch)) {
        characterCheck = [](char ch) { return ctype::isAlnum(ch) || ch == '-'; };
        // Otherwise this is an invalid attribute name
    } else {
        uasserted(ErrorCodes::BadValue,
                  str::stream() << "DN attribute names must begin with either a digit or an alpha"
                                << " not \'" << ch << "\'");
    }

    for (; ch != '=' && !done(); ch = _advance()) {
        if (ch == ' ') {
            continue;
        }
        uassert(ErrorCodes::BadValue,
                str::stream() << "DN attribute name contains an invalid character \'" << ch << "\'",
                characterCheck(ch));
        sb << ch;
    }

    if (!done()) {
        _advance();
    }

    return sb.str();
}

std::pair<std::string, RFC4514Parser::ValueTerminator> RFC4514Parser::extractValue() {
    StringBuilder sb;

    // The RFC states the spaces at the beginning and end of the value must be escaped, which
    // means we should skip any leading unescaped spaces.
    skipSpaces();

    // Every time we see an escaped space ("\ "), we increment this counter. Every time we see
    // anything non-space character we reset this counter to zero. That way we'll know the number
    // of consecutive escaped spaces at the end of the string there are.
    int trailingSpaces = 0;

    char ch = _cur();
    uassert(ErrorCodes::BadValue, "Raw DER sequences are not supported in DN strings", ch != '#');
    for (; ch != ',' && ch != '+' && !done(); ch = _advance()) {
        if (ch == '\\') {
            ch = _advance();
            if (isEscaped(ch)) {
                sb << ch;
                trailingSpaces = 0;
            } else if (ctype::isXdigit(ch)) {
                const std::array<char, 2> hexValStr = {ch, _advance()};

                uassert(ErrorCodes::BadValue,
                        str::stream() << "Escaped hex value contains invalid character \'"
                                      << hexValStr[1] << "\'",
                        ctype::isXdigit(hexValStr[1]));
                const char hexVal = hexblob::decodePair(StringData(hexValStr.data(), 2));
                sb << hexVal;
                if (hexVal != ' ') {
                    trailingSpaces = 0;
                } else {
                    trailingSpaces++;
                }
            } else if (isMayBeEscaped(ch)) {
                // It is legal to escape whitespace, but we don't count it as an "escaped"
                // character because we don't require it to be escaped within the value, that is
                // "C=New York" is legal, and so is "C=New\ York"
                //
                // The exception is that leading and trailing whitespace must be escaped or else
                // it will be trimmed.
                sb << ch;
                if (ch == ' ') {
                    trailingSpaces++;
                } else {
                    trailingSpaces = 0;
                }
            } else {
                uasserted(ErrorCodes::BadValue,
                          str::stream() << "Invalid escaped character \'" << ch << "\'");
            }
        } else if (isEscaped(ch)) {
            uasserted(ErrorCodes::BadValue,
                      str::stream()
                          << "Found unescaped character that should be escaped: \'" << ch << "\'");
        } else {
            if (ch != ' ') {
                trailingSpaces = 0;
            }
            sb << ch;
        }
    }

    std::string val = sb.str();
    // It's legal to have trailing spaces as long as they are escaped, so if we have some trailing
    // escaped spaces, trim the size of the string to the last non-space character + the number of
    // escaped trailing spaces.
    if (trailingSpaces > 0) {
        auto lastNonSpace = val.find_last_not_of(' ');
        lastNonSpace += trailingSpaces + 1;
        val.erase(lastNonSpace);
    }

    // Consume the + or , character
    if (!done()) {
        _advance();
    }

    switch (ch) {
        case '+':
            return {std::move(val), MultiValue};
        case ',':
            return {std::move(val), NewRDN};
        default:
            invariant(done());
            return {std::move(val), Done};
    }
}

const auto getTLSVersionCounts = ServiceContext::declareDecoration<TLSVersionCounts>();


void canonicalizeClusterDN(std::vector<std::string>* dn) {
    // remove all RDNs we don't care about
    for (size_t i = 0; i < dn->size(); i++) {
        std::string& comp = dn->at(i);
        boost::algorithm::trim(comp);
        if (!str::startsWith(comp.c_str(), "DC=") &&  //
            !str::startsWith(comp.c_str(), "O=") &&   //
            !str::startsWith(comp.c_str(), "OU=")) {
            dn->erase(dn->begin() + i);
            i--;
        }
    }
    std::stable_sort(dn->begin(), dn->end());
}

constexpr StringData kOID_DC = "0.9.2342.19200300.100.1.25"_sd;
constexpr StringData kOID_O = "2.5.4.10"_sd;
constexpr StringData kOID_OU = "2.5.4.11"_sd;

std::vector<SSLX509Name::Entry> canonicalizeClusterDN(
    const std::vector<std::vector<SSLX509Name::Entry>>& entries) {
    std::vector<SSLX509Name::Entry> ret;

    for (const auto& rdn : entries) {
        for (const auto& entry : rdn) {
            if ((entry.oid != kOID_DC) && (entry.oid != kOID_O) && (entry.oid != kOID_OU)) {
                continue;
            }
            ret.push_back(entry);
        }
    }
    std::stable_sort(ret.begin(), ret.end());
    return ret;
}

struct DNValue {
    explicit DNValue(SSLX509Name dn)
        : fullDN(std::move(dn)), canonicalized(canonicalizeClusterDN(fullDN.entries())) {}

    SSLX509Name fullDN;
    std::vector<SSLX509Name::Entry> canonicalized;
};
synchronized_value<boost::optional<DNValue>> clusterMemberOverride;
boost::optional<std::vector<SSLX509Name::Entry>> getClusterMemberDNOverrideParameter() {
    auto guarded_value = clusterMemberOverride.synchronize();
    auto& value = *guarded_value;
    if (!value) {
        return boost::none;
    }
    return value->canonicalized;
}
}  // namespace

SSLManagerCoordinator* SSLManagerCoordinator::get() {
    return theSSLManagerCoordinator;
}

std::shared_ptr<SSLManagerInterface> SSLManagerCoordinator::createTransientSSLManager(
    const TransientSSLParams& transientSSLParams) const {
    return SSLManagerInterface::create(
        sslGlobalParams, transientSSLParams, false /* isSSLServer */);
}

std::shared_ptr<SSLManagerInterface> SSLManagerCoordinator::getSSLManager() {
    return *_manager;
}

void logCert(const CertInformationToLog& cert, StringData certType, const int logNum) {
    auto attrs = cert.getDynamicAttributes();
    attrs.add("type", certType);
    LOGV2(logNum, "Certificate information", attrs);
}

void logCRL(const CRLInformationToLog& crl, const int logNum) {
    LOGV2(logNum,
          "CRL information",
          "thumbprint"_attr = hexblob::encode(crl.thumbprint.data(), crl.thumbprint.size()),
          "notValidBefore"_attr = crl.validityNotBefore.toString(),
          "notValidAfter"_attr = crl.validityNotAfter.toString());
}

void logSSLInfo(const SSLInformationToLog& info,
                const int logNumPEM,
                const int logNumCluster,
                const int logNumCrl) {
    if (!(sslGlobalParams.sslPEMKeyFile.empty())) {
        logCert(info.server, "Server", logNumPEM);
    }
    if (info.cluster.has_value()) {
        logCert(info.cluster.value(), "Cluster", logNumCluster);
    }
    if (info.crl.has_value()) {
        logCRL(info.crl.value(), logNumCrl);
    }
}

void SSLManagerCoordinator::rotate() {
    stdx::lock_guard lockGuard(_lock);

    std::shared_ptr<SSLManagerInterface> manager =
        SSLManagerInterface::create(sslGlobalParams, isSSLServer);

    auto svcCtx = getGlobalServiceContext();
    const auto clusterAuthMode = ClusterAuthMode::get(svcCtx);
    if (clusterAuthMode.sendsX509()) {
        auth::setInternalUserAuthParams(auth::createInternalX509AuthDocument(
            StringData(manager->getSSLConfiguration().clientSubjectName.toString())));
    }

    auto tl = svcCtx->getTransportLayer();
    invariant(tl != nullptr);
    uassertStatusOK(tl->rotateCertificates(manager, false));

    std::shared_ptr<SSLManagerInterface> originalManager = *_manager;
    _manager = manager;

    LOGV2(4913400, "Successfully rotated X509 certificates.");
    logSSLInfo(_manager->get()->getSSLInformationToLog());

    originalManager->stopJobs();
}

SSLManagerCoordinator::SSLManagerCoordinator()
    : _manager(SSLManagerInterface::create(sslGlobalParams, isSSLServer)) {
    logSSLInfo(_manager->get()->getSSLInformationToLog());
}

void ClusterMemberDNOverride::append(OperationContext* opCtx,
                                     BSONObjBuilder* b,
                                     StringData name,
                                     const boost::optional<TenantId>&) {
    auto value = clusterMemberOverride.get();
    if (value) {
        b->append(name, value->fullDN.toString());
    }
}

Status ClusterMemberDNOverride::setFromString(StringData str, const boost::optional<TenantId>&) {
    if (str.empty()) {
        *clusterMemberOverride = boost::none;
        return Status::OK();
    }

    auto swDN = parseDN(str);
    if (!swDN.isOK()) {
        return swDN.getStatus();
    }
    auto dn = std::move(swDN.getValue());
    auto status = dn.normalizeStrings();
    if (!status.isOK()) {
        return status;
    }

    DNValue val(std::move(dn));
    if (val.canonicalized.empty()) {
        return {ErrorCodes::BadValue,
                "Cluster member DN's must contain at least one O, OU, or DC component"};
    }

    *clusterMemberOverride = {std::move(val)};
    return Status::OK();
}

StatusWith<SSLX509Name> parseDN(StringData sd) try {
    uassert(ErrorCodes::BadValue, "DN strings must be valid UTF-8 strings", isValidUTF8(sd));
    RFC4514Parser parser(sd);

    std::vector<std::vector<SSLX509Name::Entry>> entries;
    auto curRDN = entries.emplace(entries.end());
    while (!parser.done()) {
        // Allow spaces to separate RDNs for readability, e.g. "CN=foo, OU=bar, DC=bizz"
        parser.skipSpaces();
        auto attributeName = parser.extractAttributeName();
        auto oid = x509ShortNameToOid(attributeName);
        uassert(ErrorCodes::BadValue, str::stream() << "DN contained an unknown OID " << oid, oid);
        std::string value;
        char terminator;
        std::tie(value, terminator) = parser.extractValue();
        curRDN->emplace_back(std::move(*oid), kASN1UTF8String, std::move(value));
        if (terminator == RFC4514Parser::NewRDN) {
            curRDN = entries.emplace(entries.end());
        }
    }

    uassert(ErrorCodes::BadValue,
            "Cannot parse empty DN",
            entries.size() > 1 || !entries.front().empty());

    return SSLX509Name(std::move(entries));
} catch (const DBException& e) {
    return e.toStatus();
}
#if MONGO_CONFIG_SSL_PROVIDER == MONGO_CONFIG_SSL_PROVIDER_OPENSSL
// OpenSSL has a more complete library of OID to SN mappings.
std::string x509OidToShortName(StringData name) {
    const auto nid = OBJ_txt2nid(name.rawData());
    if (nid == 0) {
        return name.toString();
    }

    const auto* sn = OBJ_nid2sn(nid);
    if (!sn) {
        return name.toString();
    }

    return sn;
}

using UniqueASN1Object =
    std::unique_ptr<ASN1_OBJECT, OpenSSLDeleter<decltype(ASN1_OBJECT_free), ASN1_OBJECT_free>>;

boost::optional<std::string> x509ShortNameToOid(StringData name) {
    // Converts the OID to an ASN1_OBJECT
    UniqueASN1Object obj(OBJ_txt2obj(name.rawData(), 0));
    if (!obj) {
        return boost::none;
    }

    // OBJ_obj2txt doesn't let you pass in a NULL buffer and a negative size to discover how
    // big the buffer should be, but the man page gives 80 as a good guess for buffer size.
    constexpr auto kDefaultBufferSize = 80;
    std::vector<char> buffer(kDefaultBufferSize);
    size_t realSize = OBJ_obj2txt(buffer.data(), buffer.size(), obj.get(), 1);

    // Resize the buffer down or up to the real size.
    buffer.resize(realSize);

    // If the real size is greater than the default buffer size we picked, then just call
    // OBJ_obj2txt again now that the buffer is correctly sized.
    if (realSize > kDefaultBufferSize) {
        OBJ_obj2txt(buffer.data(), buffer.size(), obj.get(), 1);
    }

    return std::string(buffer.data(), buffer.size());
}
#else
// On Apple/Windows we have to provide our own mapping.
// Generate the 2.5.4.* portions of this list from OpenSSL sources with:
// grep -E '^X509 ' "$OPENSSL/crypto/objects/objects.txt" | tr -d '\t' |
//   sed -e 's/^X509 *\([0-9]\+\) *\(: *\)\+\([[:alnum:]]\+\).*/{"2.5.4.\1", "\3"},/g'
static const std::initializer_list<std::pair<StringData, StringData>> kX509OidToShortNameMappings =
    {
        {"0.9.2342.19200300.100.1.1"_sd, "UID"_sd},
        {"0.9.2342.19200300.100.1.25"_sd, "DC"_sd},
        {"1.2.840.113549.1.9.1"_sd, "emailAddress"_sd},
        {"2.5.29.17"_sd, "subjectAltName"_sd},

        // X509 OIDs Generated from objects.txt
        {"2.5.4.3"_sd, "CN"_sd},
        {"2.5.4.4"_sd, "SN"_sd},
        {"2.5.4.5"_sd, "serialNumber"_sd},
        {"2.5.4.6"_sd, "C"_sd},
        {"2.5.4.7"_sd, "L"_sd},
        {"2.5.4.8"_sd, "ST"_sd},
        {"2.5.4.9"_sd, "street"_sd},
        {"2.5.4.10"_sd, "O"_sd},
        {"2.5.4.11"_sd, "OU"_sd},
        {"2.5.4.12"_sd, "title"_sd},
        {"2.5.4.13"_sd, "description"_sd},
        {"2.5.4.14"_sd, "searchGuide"_sd},
        {"2.5.4.15"_sd, "businessCategory"_sd},
        {"2.5.4.16"_sd, "postalAddress"_sd},
        {"2.5.4.17"_sd, "postalCode"_sd},
        {"2.5.4.18"_sd, "postOfficeBox"_sd},
        {"2.5.4.19"_sd, "physicalDeliveryOfficeName"_sd},
        {"2.5.4.20"_sd, "telephoneNumber"_sd},
        {"2.5.4.21"_sd, "telexNumber"_sd},
        {"2.5.4.22"_sd, "teletexTerminalIdentifier"_sd},
        {"2.5.4.23"_sd, "facsimileTelephoneNumber"_sd},
        {"2.5.4.24"_sd, "x121Address"_sd},
        {"2.5.4.25"_sd, "internationaliSDNNumber"_sd},
        {"2.5.4.26"_sd, "registeredAddress"_sd},
        {"2.5.4.27"_sd, "destinationIndicator"_sd},
        {"2.5.4.28"_sd, "preferredDeliveryMethod"_sd},
        {"2.5.4.29"_sd, "presentationAddress"_sd},
        {"2.5.4.30"_sd, "supportedApplicationContext"_sd},
        {"2.5.4.31"_sd, "member"_sd},
        {"2.5.4.32"_sd, "owner"_sd},
        {"2.5.4.33"_sd, "roleOccupant"_sd},
        {"2.5.4.34"_sd, "seeAlso"_sd},
        {"2.5.4.35"_sd, "userPassword"_sd},
        {"2.5.4.36"_sd, "userCertificate"_sd},
        {"2.5.4.37"_sd, "cACertificate"_sd},
        {"2.5.4.38"_sd, "authorityRevocationList"_sd},
        {"2.5.4.39"_sd, "certificateRevocationList"_sd},
        {"2.5.4.40"_sd, "crossCertificatePair"_sd},
        {"2.5.4.41"_sd, "name"_sd},
        {"2.5.4.42"_sd, "GN"_sd},
        {"2.5.4.43"_sd, "initials"_sd},
        {"2.5.4.44"_sd, "generationQualifier"_sd},
        {"2.5.4.45"_sd, "x500UniqueIdentifier"_sd},
        {"2.5.4.46"_sd, "dnQualifier"_sd},
        {"2.5.4.47"_sd, "enhancedSearchGuide"_sd},
        {"2.5.4.48"_sd, "protocolInformation"_sd},
        {"2.5.4.49"_sd, "distinguishedName"_sd},
        {"2.5.4.50"_sd, "uniqueMember"_sd},
        {"2.5.4.51"_sd, "houseIdentifier"_sd},
        {"2.5.4.52"_sd, "supportedAlgorithms"_sd},
        {"2.5.4.53"_sd, "deltaRevocationList"_sd},
        {"2.5.4.54"_sd, "dmdName"_sd},
        {"2.5.4.65"_sd, "pseudonym"_sd},
        {"2.5.4.72"_sd, "role"_sd},
};

std::string x509OidToShortName(StringData oid) {
    auto it = std::find_if(
        kX509OidToShortNameMappings.begin(),
        kX509OidToShortNameMappings.end(),
        [&](const std::pair<StringData, StringData>& entry) { return entry.first == oid; });

    if (it == kX509OidToShortNameMappings.end()) {
        return oid.toString();
    }
    return it->second.toString();
}

boost::optional<std::string> x509ShortNameToOid(StringData name) {
    auto it = std::find_if(
        kX509OidToShortNameMappings.begin(),
        kX509OidToShortNameMappings.end(),
        [&](const std::pair<StringData, StringData>& entry) { return entry.second == name; });

    if (it == kX509OidToShortNameMappings.end()) {
        // If the name is a known oid in our mapping list then just return it.
        if (std::find_if(kX509OidToShortNameMappings.begin(),
                         kX509OidToShortNameMappings.end(),
                         [&](const auto& entry) { return entry.first == name; }) !=
            kX509OidToShortNameMappings.end()) {
            return name.toString();
        }
        return boost::none;
    }
    return it->first.toString();
}
#endif

TLSVersionCounts& TLSVersionCounts::get(ServiceContext* serviceContext) {
    return getTLSVersionCounts(serviceContext);
}

MONGO_INITIALIZER_WITH_PREREQUISITES(SSLManagerLogger, ("SSLManager"))
(InitializerContext*) {
    if (!isSSLServer || (sslGlobalParams.sslMode.load() != SSLParams::SSLMode_disabled)) {
        const auto& config = SSLManagerCoordinator::get()->getSSLManager()->getSSLConfiguration();
        if (!config.clientSubjectName.empty()) {
            LOGV2_DEBUG(23214,
                        1,
                        "Client Certificate Name: {name}",
                        "Client certificate name",
                        "name"_attr = config.clientSubjectName);
        }
        if (!config.serverSubjectName().empty()) {
            LOGV2_DEBUG(23215,
                        1,
                        "Server Certificate Name: {name}",
                        "Server certificate name",
                        "name"_attr = config.serverSubjectName());
            LOGV2_DEBUG(23216,
                        1,
                        "Server Certificate Expiration: {expiration}",
                        "Server certificate expiration",
                        "expiration"_attr = config.serverCertificateExpirationDate);
        }
    }
}

Status SSLX509Name::normalizeStrings() {
    for (auto& rdn : _entries) {
        for (auto& entry : rdn) {
            switch (entry.type) {
                // For each type of valid DirectoryString, do the string prep algorithm.
                case kASN1UTF8String:
                case kASN1PrintableString:
                case kASN1TeletexString:
                case kASN1UniversalString:
                case kASN1BMPString:
                case kASN1IA5String:
                case kASN1OctetString: {
                    // Technically https://tools.ietf.org/html/rfc5280#section-4.1.2.4 requires
                    // that DN component values must be at least 1 code point long, but we've
                    // supported empty components before (see SERVER-39107) so we special-case
                    // normalizing empty values to an empty UTF-8 string
                    if (entry.value.empty()) {
                        entry.type = kASN1UTF8String;
                        break;
                    }

                    auto res = icuX509DNPrep(entry.value);
                    if (!res.isOK()) {
                        return res.getStatus();
                    }
                    entry.value = std::move(res.getValue());
                    entry.type = kASN1UTF8String;
                    break;
                }
                default:
                    LOGV2_DEBUG(23217,
                                1,
                                "Certificate subject name contains unknown string type: "
                                "{entryType} (string value is \"{entryValue}\")",
                                "Certificate subject name contains unknown string type",
                                "entryType"_attr = entry.type,
                                "entryValue"_attr = entry.value);
                    break;
            }
        }
    }

    return Status::OK();
}

StatusWith<std::string> SSLX509Name::getOID(StringData oid) const {
    for (const auto& rdn : _entries) {
        for (const auto& entry : rdn) {
            if (entry.oid == oid) {
                return entry.value;
            }
        }
    }
    return {ErrorCodes::KeyNotFound, "OID does not exist"};
}

StringBuilder& operator<<(StringBuilder& os, const SSLX509Name& name) {
    std::string comma;
    for (const auto& rdn : name._entries) {
        std::string plus;
        os << comma;
        for (const auto& entry : rdn) {
            os << plus << x509OidToShortName(entry.oid) << "=" << escapeRfc2253(entry.value);
            plus = "+";
        }
        comma = ",";
    }
    return os;
}

std::string SSLX509Name::toString() const {
    StringBuilder os;
    os << *this;
    return os.str();
}

Status SSLConfiguration::setServerSubjectName(SSLX509Name name) {
    auto status = name.normalizeStrings();
    if (!status.isOK()) {
        return status;
    }
    _serverSubjectName = std::move(name);
    _canonicalServerSubjectName = canonicalizeClusterDN(_serverSubjectName.entries());
    return Status::OK();
}

/**
 * The behavior of isClusterMember() is subtly different when passed
 * an SSLX509Name versus a StringData.
 *
 * The SSLX509Name version (immediately below) compares distinguished
 * names in their normalized, unescaped forms and provides a more reliable match.
 *
 * The StringData version attempts to canonicalize the stringified subject name
 * according to RFC4514 and compare that to the normalized/unescaped version of
 * the server's distinguished name.
 */
bool SSLConfiguration::isClusterMember(SSLX509Name subject) const {
    if (!subject.normalizeStrings().isOK()) {
        return false;
    }

    auto client = canonicalizeClusterDN(subject.entries());
    if (client.empty()) {
        return false;
    }

    if (client == _canonicalServerSubjectName) {
        return true;
    }

    auto altClusterDN = getClusterMemberDNOverrideParameter();
    return (altClusterDN && (client == *altClusterDN));
}

bool SSLConfiguration::isClusterMember(StringData subjectName) const {
    auto swClient = parseDN(subjectName);
    if (!swClient.isOK()) {
        LOGV2_WARNING(23219,
                      "Unable to parse client subject name: {error}",
                      "Unable to parse client subject name",
                      "error"_attr = swClient.getStatus());
        return false;
    }
    auto& client = swClient.getValue();
    auto status = client.normalizeStrings();
    if (!status.isOK()) {
        LOGV2_WARNING(23220,
                      "Unable to normalize client subject name: {error}",
                      "Unable to normalize client subject name",
                      "error"_attr = status);
        return false;
    }

    auto canonicalClient = canonicalizeClusterDN(client.entries());

    return !canonicalClient.empty() && (canonicalClient == _canonicalServerSubjectName);
}

void SSLConfiguration::getServerStatusBSON(BSONObjBuilder* security) const {
    security->append("SSLServerSubjectName", _serverSubjectName.toString());
    security->appendBool("SSLServerHasCertificateAuthority", hasCA);
    security->appendDate("SSLServerCertificateExpirationDate", serverCertificateExpirationDate);
}

SSLManagerInterface::~SSLManagerInterface() {}

SSLConnectionInterface::~SSLConnectionInterface() {}

namespace {

/**
 * Enum of supported Abstract Syntax Notation One (ASN.1) Distinguished Encoding Rules (DER) types.
 *
 * This is a subset of all DER types.
 */
enum class DERType : char {
    // Primitive, not supported by the parser
    // Only exists when BER indefinite form is used which is not valid DER.
    EndOfContent = 0,

    // Primitive
    INTEGER = 2,

    // Primitive
    UTF8String = 12,

    // Sequence or Sequence Of, Constructed
    SEQUENCE = 16,

    // Set or Set Of, Constructed
    SET = 17,
};

/**
 * Distinguished Encoding Rules (DER) are a strict subset of Basic Encoding Rules (BER).
 *
 * For more details, see X.690 from ITU-T.
 *
 * It is a Tag + Length + Value format. The tag is generally 1 byte, the length is 1 or more
 * and then followed by the value.
 */
class DERToken {
public:
    DERToken() {}
    DERToken(DERType type, size_t length, const char* const data)
        : _type(type), _length(length), _data(data) {}

    /**
     * Get the ASN.1 type of the current token.
     */
    DERType getType() const {
        return _type;
    }

    /**
     * Get a ConstDataRange for the value of this SET or SET OF.
     */
    ConstDataRange getSetRange() {
        invariant(_type == DERType::SET);
        return ConstDataRange(_data, _data + _length);
    }

    /**
     * Get a ConstDataRange for the value of this SEQUENCE or SEQUENCE OF.
     */
    ConstDataRange getSequenceRange() {
        invariant(_type == DERType::SEQUENCE);
        return ConstDataRange(_data, _data + _length);
    }

    /**
     * Get a std::string for the value of this Utf8String.
     */
    std::string readUtf8String() {
        invariant(_type == DERType::UTF8String);
        return std::string(_data, _length);
    }

    /**
     * Get a vector representation for the value of this DER INTEGER
     */
    DERInteger readInt() {
        invariant(_type == DERType::INTEGER);
        DERInteger out(_length);
        std::copy(_data, _data + _length, out.begin());
        return out;
    }

    /**
     * Parse a buffer of bytes and return the number of bytes we read for this token.
     *
     * Returns a DERToken which consists of the (tag, length, value) tuple.
     */
    static StatusWith<DERToken> parse(ConstDataRange cdr, size_t* outLength);

private:
    DERType _type{DERType::EndOfContent};
    size_t _length{0};
    const char* _data{nullptr};
};

}  // namespace

template <>
struct DataType::Handler<DERToken> {
    static Status load(DERToken* t,
                       const char* ptr,
                       size_t length,
                       size_t* advanced,
                       std::ptrdiff_t debug_offset) {
        size_t outLength;

        auto swPair = DERToken::parse(ConstDataRange(ptr, length), &outLength);

        if (!swPair.isOK()) {
            return swPair.getStatus();
        }

        if (t) {
            *t = std::move(swPair.getValue());
        }

        if (advanced) {
            *advanced = outLength;
        }

        return Status::OK();
    }

    static DERToken defaultConstruct() {
        return DERToken();
    }
};

namespace {

StatusWith<std::string> readDERString(ConstDataRangeCursor& cdc) {
    auto swString = cdc.readAndAdvanceNoThrow<DERToken>();
    if (!swString.isOK()) {
        return swString.getStatus();
    }

    auto derString = swString.getValue();

    if (derString.getType() != DERType::UTF8String) {
        return Status(ErrorCodes::InvalidSSLConfiguration,
                      str::stream()
                          << "Unexpected DER Tag, Got " << static_cast<char>(derString.getType())
                          << ", Expected UTF8String");
    }

    return derString.readUtf8String();
}

StatusWith<DERInteger> readDERInt(ConstDataRangeCursor& cdc) {
    auto swInt = cdc.readAndAdvanceNoThrow<DERToken>();
    if (!swInt.isOK()) {
        return swInt.getStatus();
    }

    auto derInt = swInt.getValue();

    if (derInt.getType() != DERType::INTEGER) {
        return Status(ErrorCodes::InvalidSSLConfiguration,
                      str::stream() << "Unexpected DER Tag, Got "
                                    << static_cast<char>(derInt.getType()) << ", Expected INTEGER");
    }

    return derInt.readInt();
}


StatusWith<DERToken> DERToken::parse(ConstDataRange cdr, size_t* outLength) {
    const size_t kTagLength = 1;
    const size_t kTagLengthAndInitialLengthByteLength = kTagLength + 1;

    ConstDataRangeCursor cdrc(cdr);

    auto swTagByte = cdrc.readAndAdvanceNoThrow<char>();
    if (!swTagByte.getStatus().isOK()) {
        return swTagByte.getStatus();
    }

    const char tagByte = swTagByte.getValue();

    // Get the tag number from the first 5 bits
    const char tag = tagByte & 0x1f;

    // Check the 6th bit
    const bool constructed = tagByte & 0x20;
    const bool primitive = !constructed;

    // Check bits 7 and 8 for the tag class, we only want Universal (i.e. 0)
    const char tagClass = tagByte & 0xC0;
    if (tagClass != 0) {
        return Status(ErrorCodes::InvalidSSLConfiguration, "Unsupported tag class");
    }

    // Validate the 6th bit is correct, and it is a known type
    switch (static_cast<DERType>(tag)) {
        case DERType::UTF8String:
            if (!primitive) {
                return Status(ErrorCodes::InvalidSSLConfiguration, "Unknown DER tag");
            }
            break;
        case DERType::INTEGER:
            if (!primitive) {
                return Status(ErrorCodes::InvalidSSLConfiguration, "Unknown DER tag");
            }
            break;
        case DERType::SEQUENCE:
        case DERType::SET:
            if (!constructed) {
                return Status(ErrorCodes::InvalidSSLConfiguration, "Unknown DER tag");
            }
            break;
        default:
            return Status(ErrorCodes::InvalidSSLConfiguration, "Unknown DER tag");
    }

    // Do we have at least 1 byte for the length
    if (cdrc.length() < kTagLengthAndInitialLengthByteLength) {
        return Status(ErrorCodes::InvalidSSLConfiguration, "Invalid DER length");
    }

    // Read length
    // Depending on the high bit, either read 1 byte or N bytes
    auto swInitialLengthByte = cdrc.readAndAdvanceNoThrow<char>();
    if (!swInitialLengthByte.getStatus().isOK()) {
        return swInitialLengthByte.getStatus();
    }

    const char initialLengthByte = swInitialLengthByte.getValue();


    uint64_t derLength = 0;

    // How many bytes does it take to encode the length?
    size_t encodedLengthBytesCount = 1;

    if (initialLengthByte & 0x80) {
        // Length is > 127 bytes, i.e. Long form of length
        const size_t lengthBytesCount = 0x7f & initialLengthByte;

        // If length is encoded in more then 8 bytes, we disallow it
        if (lengthBytesCount > 8) {
            return Status(ErrorCodes::InvalidSSLConfiguration, "Invalid DER length");
        }

        // Ensure we have enough data for the length bytes
        const char* lengthLongFormPtr = cdrc.data();

        Status statusLength = cdrc.advanceNoThrow(lengthBytesCount);
        if (!statusLength.isOK()) {
            return statusLength;
        }

        encodedLengthBytesCount = 1 + lengthBytesCount;

        std::array<char, 8> lengthBuffer;
        lengthBuffer.fill(0);

        // Copy the length into the end of the buffer
        memcpy(lengthBuffer.data() + (8 - lengthBytesCount), lengthLongFormPtr, lengthBytesCount);

        // We now have 0x00..NN in the buffer and it can be properly decoded as BigEndian
        derLength = ConstDataView(lengthBuffer.data()).read<BigEndian<uint64_t>>();
    } else {
        // Length is <= 127 bytes, i.e. short form of length
        derLength = initialLengthByte;
    }

    // This is the total length of the TLV and all data
    // This will not overflow since encodedLengthBytesCount <= 9
    const uint64_t tagAndLengthByteCount = kTagLength + encodedLengthBytesCount;

    // This may overflow since derLength is from user data so check our arithmetic carefully.
    if (overflow::add(tagAndLengthByteCount, derLength, outLength) || *outLength > cdr.length()) {
        return Status(ErrorCodes::InvalidSSLConfiguration, "Invalid DER length");
    }

    return DERToken(static_cast<DERType>(tag), derLength, cdr.data() + tagAndLengthByteCount);
}
}  // namespace

StatusWith<stdx::unordered_set<RoleName>> parsePeerRoles(ConstDataRange cdrExtension) {
    stdx::unordered_set<RoleName> roles;

    ConstDataRangeCursor cdcExtension(cdrExtension);

    /**
     * MongoDBAuthorizationGrants ::= SET OF MongoDBAuthorizationGrant
     *
     * MongoDBAuthorizationGrant ::= CHOICE {
     *  MongoDBRole,
     *  ...!UTF8String:"Unrecognized entity in MongoDBAuthorizationGrant"
     * }
     */
    auto swSet = cdcExtension.readAndAdvanceNoThrow<DERToken>();
    if (!swSet.isOK()) {
        return swSet.getStatus();
    }

    if (swSet.getValue().getType() != DERType::SET) {
        return Status(ErrorCodes::InvalidSSLConfiguration,
                      str::stream()
                          << "Unexpected DER Tag, Got "
                          << static_cast<char>(swSet.getValue().getType()) << ", Expected SET");
    }

    ConstDataRangeCursor cdcSet(swSet.getValue().getSetRange());

    while (!cdcSet.empty()) {
        /**
         * MongoDBRole ::= SEQUENCE {
         *  role     UTF8String,
         *  database UTF8String
         * }
         */
        auto swSequence = cdcSet.readAndAdvanceNoThrow<DERToken>();
        if (!swSequence.isOK()) {
            return swSequence.getStatus();
        }

        auto sequenceStart = swSequence.getValue();

        if (sequenceStart.getType() != DERType::SEQUENCE) {
            return Status(ErrorCodes::InvalidSSLConfiguration,
                          str::stream() << "Unexpected DER Tag, Got "
                                        << static_cast<char>(sequenceStart.getType())
                                        << ", Expected SEQUENCE");
        }

        ConstDataRangeCursor cdcSequence(sequenceStart.getSequenceRange());

        auto swRole = readDERString(cdcSequence);
        if (!swRole.isOK()) {
            return swRole.getStatus();
        }

        auto swDatabase = readDERString(cdcSequence);
        if (!swDatabase.isOK()) {
            return swDatabase.getStatus();
        }

        roles.emplace(swRole.getValue(), swDatabase.getValue());
    }

    return roles;
}

StatusWith<std::vector<DERInteger>> parseTLSFeature(ConstDataRange cdrExtension) {
    std::vector<DERInteger> features;
    ConstDataRangeCursor cdcExtension(cdrExtension);

    /**
     * FEATURES ::= SEQUENCE OF INTEGER
     */
    auto swSeq = cdcExtension.readAndAdvanceNoThrow<DERToken>();
    if (!swSeq.isOK()) {
        return swSeq.getStatus();
    }

    if (swSeq.getValue().getType() != DERType::SEQUENCE) {
        return Status(ErrorCodes::InvalidSSLConfiguration,
                      str::stream() << "Unexpected DER Tag, Got "
                                    << static_cast<char>(swSeq.getValue().getType())
                                    << ", Expected SEQUENCE");
    }

    ConstDataRangeCursor cdcSeq(swSeq.getValue().getSequenceRange());

    while (!cdcSeq.empty()) {
        auto swDERInt = readDERInt(cdcSeq);
        if (!swDERInt.isOK()) {
            return swDERInt.getStatus();
        }

        features.emplace_back(swDERInt.getValue());
    }

    return std::move(features);
}

std::string removeFQDNRoot(std::string name) {
    if (name.back() == '.') {
        name.pop_back();
    }
    return name;
};

namespace {

// Characters that need to be escaped in RFC 2253
const std::array<char, 7> rfc2253EscapeChars = {',', '+', '"', '\\', '<', '>', ';'};

}  // namespace

// See section "2.4 Converting an AttributeValue from ASN.1 to a String" in RFC 2243
std::string escapeRfc2253(StringData str) {
    std::string ret;

    if (str.size() > 0) {
        size_t pos = 0;

        // a space or "#" character occurring at the beginning of the string
        if (str[0] == ' ') {
            ret = "\\ ";
            pos = 1;
        } else if (str[0] == '#') {
            ret = "\\#";
            pos = 1;
        }

        while (pos < str.size()) {
            if (static_cast<signed char>(str[pos]) < 0) {
                ret += '\\';
                ret += unsignedHex(str[pos]);
            } else {
                if (std::find(rfc2253EscapeChars.cbegin(), rfc2253EscapeChars.cend(), str[pos]) !=
                    rfc2253EscapeChars.cend()) {
                    ret += '\\';
                }

                ret += str[pos];
            }
            ++pos;
        }

        // a space character occurring at the end of the string
        if (ret.size() > 2 && ret[ret.size() - 1] == ' ') {
            ret[ret.size() - 1] = '\\';
            ret += ' ';
        }
    }

    return ret;
}

void recordTLSVersion(TLSVersion version, const HostAndPort& hostForLogging) {
    StringData versionString;
    auto& counts = mongo::TLSVersionCounts::get(getGlobalServiceContext());
    switch (version) {
        case TLSVersion::kTLS10:
            counts.tls10.addAndFetch(1);
            if (std::find(sslGlobalParams.tlsLogVersions.cbegin(),
                          sslGlobalParams.tlsLogVersions.cend(),
                          SSLParams::Protocols::TLS1_0) != sslGlobalParams.tlsLogVersions.cend()) {
                versionString = "1.0"_sd;
            }
            break;
        case TLSVersion::kTLS11:
            counts.tls11.addAndFetch(1);
            if (std::find(sslGlobalParams.tlsLogVersions.cbegin(),
                          sslGlobalParams.tlsLogVersions.cend(),
                          SSLParams::Protocols::TLS1_1) != sslGlobalParams.tlsLogVersions.cend()) {
                versionString = "1.1"_sd;
            }
            break;
        case TLSVersion::kTLS12:
            counts.tls12.addAndFetch(1);
            if (std::find(sslGlobalParams.tlsLogVersions.cbegin(),
                          sslGlobalParams.tlsLogVersions.cend(),
                          SSLParams::Protocols::TLS1_2) != sslGlobalParams.tlsLogVersions.cend()) {
                versionString = "1.2"_sd;
            }
            break;
        case TLSVersion::kTLS13:
            counts.tls13.addAndFetch(1);
            if (std::find(sslGlobalParams.tlsLogVersions.cbegin(),
                          sslGlobalParams.tlsLogVersions.cend(),
                          SSLParams::Protocols::TLS1_3) != sslGlobalParams.tlsLogVersions.cend()) {
                versionString = "1.3"_sd;
            }
            break;
        default:
            counts.tlsUnknown.addAndFetch(1);
            if (!sslGlobalParams.tlsLogVersions.empty()) {
                versionString = "unknown"_sd;
            }
            break;
    }

    if (!versionString.empty()) {
        LOGV2(23218,
              "Accepted connection with TLS Version {tlsVersion} from connection {remoteHost}",
              "Accepted connection with TLS",
              "tlsVersion"_attr = versionString,
              "remoteHost"_attr = hostForLogging);
    }
}

// TODO SERVER-11601 Use NFC Unicode canonicalization
bool hostNameMatchForX509Certificates(std::string nameToMatch, std::string certHostName) {
    nameToMatch = removeFQDNRoot(std::move(nameToMatch));
    certHostName = removeFQDNRoot(std::move(certHostName));

    if (certHostName.size() < 2) {
        return false;
    }

    // match wildcard DNS names
    if (certHostName[0] == '*' && certHostName[1] == '.') {
        // allow name.example.com if the cert is *.example.com, '*' does not match '.'
        const char* subName = strchr(nameToMatch.c_str(), '.');
        return subName && !str::caseInsensitiveCompare(certHostName.c_str() + 1, subName);
    } else {
        return !str::caseInsensitiveCompare(nameToMatch.c_str(), certHostName.c_str());
    }
}

void tlsEmitWarningExpiringClientCertificate(const SSLX509Name& peer) {
    LOGV2_WARNING(23221,
                  "Peer certificate '{peerSubjectName}' expires soon",
                  "Peer certificate expires soon",
                  "peerSubjectName"_attr = peer);
}

void tlsEmitWarningExpiringClientCertificate(const SSLX509Name& peer, Days days) {
    LOGV2_WARNING(23222,
                  "Peer certificate '{peerSubjectName}' expires in {days}",
                  "Peer certificate expiration information",
                  "peerSubjectName"_attr = peer,
                  "days"_attr = days);
}

}  // namespace mongo
