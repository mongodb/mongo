/*    Copyright 2009 10gen Inc.
 *
 *    This program is free software: you can redistribute it and/or  modify
 *    it under the terms of the GNU Affero General Public License, version 3,
 *    as published by the Free Software Foundation.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    GNU Affero General Public License for more details.
 *
 *    You should have received a copy of the GNU Affero General Public License
 *    along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the GNU Affero General Public License in all respects
 *    for all of the code used other than as permitted herein. If you modify
 *    file(s) with this exception, you may extend this exception to your
 *    version of the file(s), but you are not obligated to do so. If you do not
 *    wish to do so, delete this exception statement from your version. If you
 *    delete this exception statement from all source files in the program,
 *    then also delete it in the license file.
 */


#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kNetwork

#include "mongo/platform/basic.h"

#include "mongo/util/net/ssl_manager.h"

#include <boost/algorithm/string.hpp>
#include <string>
#include <vector>

#include "mongo/base/init.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/config.h"
#include "mongo/db/server_parameters.h"
#include "mongo/platform/overflow_arithmetic.h"
#include "mongo/transport/session.h"
#include "mongo/util/hex.h"
#include "mongo/util/log.h"
#include "mongo/util/mongoutils/str.h"
#include "mongo/util/net/ssl_options.h"
#include "mongo/util/net/ssl_types.h"
#include "mongo/util/text.h"

namespace mongo {

namespace {

const transport::Session::Decoration<SSLPeerInfo> peerInfoForSession =
    transport::Session::declareDecoration<SSLPeerInfo>();

/**
 * Configurable via --setParameter disableNonSSLConnectionLogging=true. If false (default)
 * if the sslMode is set to preferSSL, we will log connections that are not using SSL.
 * If true, such log messages will be suppressed.
 */
ExportedServerParameter<bool, ServerParameterType::kStartupOnly>
    disableNonSSLConnectionLoggingParameter(ServerParameterSet::getGlobal(),
                                            "disableNonSSLConnectionLogging",
                                            &sslGlobalParams.disableNonSSLConnectionLogging);

ExportedServerParameter<std::string, ServerParameterType::kStartupOnly>
    setDiffieHellmanParameterPEMFile(ServerParameterSet::getGlobal(),
                                     "opensslDiffieHellmanParameters",
                                     &sslGlobalParams.sslPEMTempDHParam);
}  // namespace


SSLPeerInfo& SSLPeerInfo::forSession(const transport::SessionHandle& session) {
    return peerInfoForSession(session.get());
}

SSLParams sslGlobalParams;

const SSLParams& getSSLGlobalParams() {
    return sslGlobalParams;
}

class OpenSSLCipherConfigParameter
    : public ExportedServerParameter<std::string, ServerParameterType::kStartupOnly> {
public:
    OpenSSLCipherConfigParameter()
        : ExportedServerParameter<std::string, ServerParameterType::kStartupOnly>(
              ServerParameterSet::getGlobal(),
              "opensslCipherConfig",
              &sslGlobalParams.sslCipherConfig) {}
    Status validate(const std::string& potentialNewValue) final {
        if (!sslGlobalParams.sslCipherConfig.empty()) {
            return Status(
                ErrorCodes::BadValue,
                "opensslCipherConfig setParameter is incompatible with net.ssl.sslCipherConfig");
        }
        // Note that there is very little validation that we can do here.
        // OpenSSL exposes no API to validate a cipher config string. The only way to figure out
        // what a string maps to is to make an SSL_CTX object, set the string on it, then parse the
        // resulting STACK_OF object. If provided an invalid entry in the string, it will silently
        // ignore it. Because an entry in the string may map to multiple ciphers, or remove ciphers
        // from the final set produced by the full string, we can't tell if any entry failed
        // to parse.
        return Status::OK();
    }
} openSSLCipherConfig;

#ifdef MONGO_CONFIG_SSL

namespace {
#if MONGO_CONFIG_SSL_PROVIDER == SSL_PROVIDER_OPENSSL
// OpenSSL has a more complete library of OID to SN mappings.
std::string x509OidToShortName(const std::string& name) {
    const auto nid = OBJ_txt2nid(name.c_str());
    if (nid == 0) {
        return name;
    }

    const auto* sn = OBJ_nid2sn(nid);
    if (!sn) {
        return name;
    }

    return sn;
}
#else
// On Apple/Windows we have to provide our own mapping.
std::string x509OidToShortName(const std::string& name) {
    static const std::map<std::string, std::string> kX509OidToShortNameMappings = {
        {"0.9.2342.19200300.100.1.1", "UID"},
        {"0.9.2342.19200300.100.1.25", "DC"},
        {"1.2.840.113549.1.9.1", "emailAddress"},
        {"2.5.4.3", "CN"},
        {"2.5.4.6", "C"},
        {"2.5.4.7", "L"},
        {"2.5.4.8", "ST"},
        {"2.5.4.9", "STREET"},
        {"2.5.4.10", "O"},
        {"2.5.4.11", "OU"},
    };

    auto it = kX509OidToShortNameMappings.find(name);
    if (it == kX509OidToShortNameMappings.end()) {
        return name;
    }
    return it->second;
}
#endif
}  // namespace

MONGO_INITIALIZER_WITH_PREREQUISITES(SSLManagerLogger, ("SSLManager", "GlobalLogManager"))
(InitializerContext*) {
    if (!isSSLServer || (sslGlobalParams.sslMode.load() != SSLParams::SSLMode_disabled)) {
        const auto& config = getSSLManager()->getSSLConfiguration();
        if (!config.clientSubjectName.empty()) {
            LOG(1) << "Client Certificate Name: " << config.clientSubjectName;
        }
        if (!config.serverSubjectName.empty()) {
            LOG(1) << "Server Certificate Name: " << config.serverSubjectName;
            LOG(1) << "Server Certificate Expiration: " << config.serverCertificateExpirationDate;
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

namespace {
void canonicalizeClusterDN(std::vector<std::string>* dn) {
    // remove all RDNs we don't care about
    for (size_t i = 0; i < dn->size(); i++) {
        std::string& comp = dn->at(i);
        boost::algorithm::trim(comp);
        if (!mongoutils::str::startsWith(comp.c_str(), "DC=") &&
            !mongoutils::str::startsWith(comp.c_str(), "O=") &&
            !mongoutils::str::startsWith(comp.c_str(), "OU=")) {
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
}  // namespace

/**
 * The behavior of isClusterMember() is subtly different when passed
 * an SSLX509Name versus a StringData.
 *
 * The SSLX509Name version (immediately below) compares distinguished
 * names in their raw, unescaped forms and provides a more reliable match.
 *
 * The StringData version attempts to do a simplified string compare
 * with the serialized version of the server subject name.
 *
 * Because escaping is not checked in the StringData version,
 * some not-strictly matching RDNs will appear to share O/OU/DC with the
 * server subject name.  Therefore, that variant should be called with care.
 */
bool SSLConfiguration::isClusterMember(const SSLX509Name& subject) const {
    auto client = canonicalizeClusterDN(subject._entries);
    auto server = canonicalizeClusterDN(serverSubjectName._entries);

    return !client.empty() && (client == server);
}

bool SSLConfiguration::isClusterMember(StringData subjectName) const {
    std::vector<std::string> clientRDN = StringSplitter::split(subjectName.toString(), ",");
    std::vector<std::string> serverRDN = StringSplitter::split(serverSubjectName.toString(), ",");

    canonicalizeClusterDN(&clientRDN);
    canonicalizeClusterDN(&serverRDN);

    return !clientRDN.empty() && (clientRDN == serverRDN);
}

BSONObj SSLConfiguration::getServerStatusBSON() const {
    BSONObjBuilder security;
    security.append("SSLServerSubjectName", serverSubjectName.toString());
    security.appendBool("SSLServerHasCertificateAuthority", hasCA);
    security.appendDate("SSLServerCertificateExpirationDate", serverCertificateExpirationDate);
    return security.obj();
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
    auto swString = cdc.readAndAdvance<DERToken>();
    if (!swString.isOK()) {
        return swString.getStatus();
    }

    auto derString = swString.getValue();

    if (derString.getType() != DERType::UTF8String) {
        return Status(ErrorCodes::InvalidSSLConfiguration,
                      str::stream() << "Unexpected DER Tag, Got "
                                    << static_cast<char>(derString.getType())
                                    << ", Expected UTF8String");
    }

    return derString.readUtf8String();
}


StatusWith<DERToken> DERToken::parse(ConstDataRange cdr, size_t* outLength) {
    const size_t kTagLength = 1;
    const size_t kTagLengthAndInitialLengthByteLength = kTagLength + 1;

    ConstDataRangeCursor cdrc(cdr);

    auto swTagByte = cdrc.readAndAdvance<char>();
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
    auto swInitialLengthByte = cdrc.readAndAdvance<char>();
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

        Status statusLength = cdrc.advance(lengthBytesCount);
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
    if (mongoUnsignedAddOverflow64(tagAndLengthByteCount, derLength, outLength) ||
        *outLength > cdr.length()) {
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
    auto swSet = cdcExtension.readAndAdvance<DERToken>();
    if (!swSet.isOK()) {
        return swSet.getStatus();
    }

    if (swSet.getValue().getType() != DERType::SET) {
        return Status(ErrorCodes::InvalidSSLConfiguration,
                      str::stream() << "Unexpected DER Tag, Got "
                                    << static_cast<char>(swSet.getValue().getType())
                                    << ", Expected SET");
    }

    ConstDataRangeCursor cdcSet(swSet.getValue().getSetRange());

    while (!cdcSet.empty()) {
        /**
         * MongoDBRole ::= SEQUENCE {
         *  role     UTF8String,
         *  database UTF8String
         * }
         */
        auto swSequence = cdcSet.readAndAdvance<DERToken>();
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
                ret += integerToHex(str[pos]);
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

#endif

}  // namespace mongo

#ifdef MONGO_CONFIG_SSL
// TODO SERVER-11601 Use NFC Unicode canonicalization
bool mongo::hostNameMatchForX509Certificates(std::string nameToMatch, std::string certHostName) {
    nameToMatch = removeFQDNRoot(std::move(nameToMatch));
    certHostName = removeFQDNRoot(std::move(certHostName));

    if (certHostName.size() < 2) {
        return false;
    }

    // match wildcard DNS names
    if (certHostName[0] == '*' && certHostName[1] == '.') {
        // allow name.example.com if the cert is *.example.com, '*' does not match '.'
        const char* subName = strchr(nameToMatch.c_str(), '.');
        return subName && !strcasecmp(certHostName.c_str() + 1, subName);
    } else {
        return !strcasecmp(nameToMatch.c_str(), certHostName.c_str());
    }
}
#endif
