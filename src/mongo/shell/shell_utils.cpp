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


#include <algorithm>
#include <boost/cstdint.hpp>
#include <boost/filesystem/operations.hpp>
#include <boost/filesystem/path.hpp>
#include <boost/move/utility_core.hpp>
#include <boost/none.hpp>
#include <boost/numeric/conversion/converter_policies.hpp>
#include <boost/optional/optional.hpp>
#include <cerrno>
#include <climits>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <fmt/format.h>
#include <functional>
#include <iostream>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <utility>
#include <vector>

#ifndef _WIN32
#include <pwd.h>
#endif

#include "mongo/base/data_range.h"
#include "mongo/base/error_codes.h"
#include "mongo/base/shim.h"
#include "mongo/base/status.h"
#include "mongo/base/status_with.h"
#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobj_comparator.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/bsontypes.h"
#include "mongo/client/client_api_version_parameters_gen.h"
#include "mongo/client/connection_string.h"
#include "mongo/client/dbclient_base.h"
#include "mongo/client/mongo_uri.h"
#include "mongo/client/replica_set_monitor.h"
#include "mongo/config.h"  // IWYU pragma: keep
#include "mongo/crypto/hash_block.h"
#include "mongo/crypto/sha256_block.h"
#include "mongo/db/auth/validated_tenancy_scope_factory.h"
#include "mongo/db/database_name.h"
#include "mongo/db/hasher.h"
#include "mongo/db/query/collation/collator_factory_icu.h"
#include "mongo/platform/decimal128.h"
#include "mongo/platform/random.h"
#include "mongo/scripting/engine.h"
#include "mongo/shell/bench.h"
#include "mongo/shell/shell_options.h"
#include "mongo/shell/shell_utils.h"
#include "mongo/shell/shell_utils_extended.h"
#include "mongo/shell/shell_utils_launcher.h"
#include "mongo/stdx/mutex.h"
#include "mongo/unittest/golden_test_base.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/buildinfo.h"
#include "mongo/util/ctype.h"
#include "mongo/util/fail_point.h"
#include "mongo/util/processinfo.h"
#include "mongo/util/represent_as.h"
#include "mongo/util/str.h"
#include "mongo/util/text.h"  // IWYU pragma: keep

#if defined(MONGO_CONFIG_HAVE_HEADER_UNISTD_H)
#include <unistd.h>
#endif

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kDefault


namespace mongo::shell_utils {
using namespace fmt::literals;
namespace {
boost::filesystem::path getUserDir() {
#ifdef _WIN32
    auto wenvp = _wgetenv(L"USERPROFILE");
    if (wenvp)
        return toUtf8String(wenvp);

    return "./";
#else
    const auto homeDir = getenv("HOME");
    if (homeDir)
        return homeDir;

    // The storage for these variables has to live until the value is captured into a std::string at
    // the end of this function.  This is because getpwuid_r(3) doesn't use static storage, but
    // storage provided by the caller.  As a fallback, reserve enough space to store 8 paths, on the
    // theory that the pwent buffer probably needs about that many paths, to fully describe a user
    // -- shell paths, home directory paths, etc.

    const long pwentBufferSize = std::max<long>(sysconf(_SC_GETPW_R_SIZE_MAX), PATH_MAX * 8);

    struct passwd pwent;
    struct passwd* res;

    std::vector<char> buffer(pwentBufferSize);

    do {
        if (!getpwuid_r(getuid(), &pwent, &buffer[0], buffer.size(), &res))
            break;

        if (errno != EINTR)
            uasserted(mongo::ErrorCodes::InternalError,
                      "Unable to get home directory for the current user.");
    } while (errno == EINTR);

    return pwent.pw_dir;
#endif
}

}  // namespace
}  // namespace mongo::shell_utils

boost::filesystem::path mongo::shell_utils::getHistoryFilePath() {
    static const auto& historyFile = *new boost::filesystem::path(getUserDir() / ".dbshell");

    return historyFile;
}


namespace mongo {
namespace JSFiles {
extern const JSFile servers;
extern const JSFile servers_misc;
extern const JSFile data_consistency_checker;
extern const JSFile bridge;
extern const JSFile feature_compatibility_version;
}  // namespace JSFiles

namespace {

std::unique_ptr<DBClientBase> benchRunConfigCreateConnectionImplProvider(
    const BenchRunConfig& config) {
    const ConnectionString connectionString = uassertStatusOK(ConnectionString::parse(config.host));
    auto swConn{connectionString.connect("BenchRun")};
    uassert(16158, swConn.getStatus().reason(), swConn.isOK());
    return std::move(swConn.getValue());
}

auto benchRunConfigCreateConnectionImplRegistration = MONGO_WEAK_FUNCTION_REGISTRATION(
    BenchRunConfig::createConnectionImpl, benchRunConfigCreateConnectionImplProvider);

// helper functions for isBalanced
bool isUseCmd(std::string code) {
    size_t first_space = code.find(' ');
    if (first_space)
        code = code.substr(0, first_space);
    return code == "use";
}

/**
 * Skip over a quoted string, including quotes escaped with backslash
 *
 * @param code      String
 * @param start     Starting position within string, always > 0
 * @param quote     Quote character (single or double quote)
 * @return          Position of ending quote, or code.size() if no quote found
 */
size_t skipOverString(const std::string& code, size_t start, char quote) {
    size_t pos = start;
    while (pos < code.size()) {
        pos = code.find(quote, pos);
        if (pos == std::string::npos) {
            return code.size();
        }
        // We want to break if the quote we found is not escaped, but we need to make sure
        // that the escaping backslash is not itself escaped.  Comparisons of start and pos
        // are to keep us from reading beyond the beginning of the quoted string.
        //
        if (start == pos || code[pos - 1] != '\\' ||   // previous char was backslash
            start == pos - 1 || code[pos - 2] == '\\'  // char before backslash was not another
        ) {
            break;  // The quote we found was not preceded by an unescaped backslash; it is real
        }
        ++pos;  // The quote we found was escaped with backslash, so it doesn't count
    }
    return pos;
}

bool isOpSymbol(char c) {
    static std::string OpSymbols = "~!%^&*-+=|:,<>/?.";

    for (size_t i = 0; i < OpSymbols.size(); i++)
        if (OpSymbols[i] == c)
            return true;
    return false;
}

}  // namespace

namespace shell_utils {


bool isBalanced(const std::string& code) {
    if (isUseCmd(code))
        return true;  // don't balance "use <dbname>" in case dbname contains special chars
    int curlyBrackets = 0;
    int squareBrackets = 0;
    int parens = 0;
    bool danglingOp = false;

    for (size_t i = 0; i < code.size(); i++) {
        switch (code[i]) {
            case '/':
                if (i + 1 < code.size() && code[i + 1] == '/') {
                    while (i < code.size() && code[i] != '\n')
                        i++;
                }
                continue;
            case '{':
                curlyBrackets++;
                break;
            case '}':
                if (curlyBrackets <= 0)
                    return true;
                curlyBrackets--;
                break;
            case '[':
                squareBrackets++;
                break;
            case ']':
                if (squareBrackets <= 0)
                    return true;
                squareBrackets--;
                break;
            case '(':
                parens++;
                break;
            case ')':
                if (parens <= 0)
                    return true;
                parens--;
                break;
            case '"':
            case '\'':
                i = skipOverString(code, i + 1, code[i]);
                if (i >= code.size()) {
                    return true;  // Do not let unterminated strings enter multi-line mode
                }
                break;
            case '\\':
                if (i + 1 < code.size() && code[i + 1] == '/')
                    i++;
                break;
            case '+':
            case '-':
                if (i + 1 < code.size() && code[i + 1] == code[i]) {
                    i++;
                    continue;  // postfix op (++/--) can't be a dangling op
                }
                break;
        }
        if (i >= code.size()) {
            danglingOp = false;
            break;
        }
        if ("~!%^&*-+=|:,<>/?."_sd.find(code[i]) != std::string::npos)
            danglingOp = true;
        else if (!ctype::isSpace(code[i]))
            danglingOp = false;
    }

    return curlyBrackets == 0 && squareBrackets == 0 && parens == 0 && !danglingOp;
}


std::string dbConnect;

static const char* argv0 = nullptr;
EnterpriseShellCallback* enterpriseCallback = nullptr;

void RecordMyLocation(const char* _argv0) {
    argv0 = _argv0;
}

// helpers

BSONObj makeUndefined() {
    BSONObjBuilder b;
    b.appendUndefined("");
    return b.obj();
}
const BSONObj undefinedReturn = makeUndefined();

BSONElement singleArg(const BSONObj& args) {
    uassert(12597, "need to specify 1 argument", args.nFields() == 1);
    return args.firstElement();
}

// real methods

BSONObj JSGetMemInfo(const BSONObj& args, void* data) {
    ProcessInfo pi;
    uassert(10258, "processinfo not supported", pi.supported());

    BSONObjBuilder e;
    e.append("virtual", pi.getVirtualMemorySize());
    e.append("resident", pi.getResidentSize());

    BSONObjBuilder b;
    b.append("ret", e.obj());

    return b.obj();
}

thread_local auto _prng = PseudoRandom(0);

BSONObj JSSrand(const BSONObj& a, void* data) {
    boost::optional<int64_t> prngSeed = boost::none;
    boost::optional<int64_t> asDouble = boost::none;

    // Grab the least significant bits of either the supplied argument or a random number from
    // SecureRandom.
    if (a.nFields() == 1 && a.firstElement().isNumber()) {
        asDouble = representAs<double>(a.firstElement().safeNumberLong());
        prngSeed = asDouble ? representAs<int64_t>(*asDouble) : boost::none;
        uassert(6290200, "Cannot represent seed as 64 bit integral or double value", prngSeed);
    } else {
        // Use secure random number generator to get the seed value that can be safely
        // represented as double.
        auto asInt64 = SecureRandom().nextInt64SafeDoubleRepresentable();
        asDouble = representAs<double>(asInt64);
        invariant(asDouble);
        prngSeed = representAs<int64_t>(*asDouble);
    }

    // The seed is representable as both an int64_t and a double, so that the value we return (as a
    // double) can be fed back in to JSSrand() to initialize the prng (as an int64_t).
    _prng = PseudoRandom(*prngSeed);
    return BSON("" << *asDouble);
}

BSONObj JSRand(const BSONObj& a, void* data) {
    uassert(12519, "rand accepts no arguments", a.nFields() == 0);
    return BSON("" << _prng.nextCanonicalDouble());
}

BSONObj isWindows(const BSONObj& a, void* data) {
    uassert(13006, "isWindows accepts no arguments", a.nFields() == 0);
#ifdef _WIN32
    return BSON("" << true);
#else
    return BSON("" << false);
#endif
}

BSONObj getBuildInfo(const BSONObj& a, void* data) {
    uassert(16822, "getBuildInfo accepts no arguments", a.nFields() == 0);
    BSONObjBuilder b;
    ::mongo::getBuildInfo().serialize(&b);
    return BSON("" << b.done());
}

BSONObj _setShellFailPoint(const BSONObj& a, void* data) {
    if (a.nFields() != 1) {
        uasserted(ErrorCodes::BadValue,
                  str::stream() << "_setShellFailPoint takes exactly 1 argument, but was given "
                                << a.nFields());
    }

    if (!a.firstElement().isABSONObj()) {
        uasserted(ErrorCodes::BadValue,
                  str::stream() << "_setShellFailPoint given a non-object as an argument.");
    }

    auto cmdObj = a.firstElement().Obj();
    setGlobalFailPoint(cmdObj.firstElement().str(), cmdObj);

    return BSON("" << true);
}

BSONObj computeSHA256Block(const BSONObj& a, void* data) {
    std::vector<ConstDataRange> blocks;

    auto ele = a[0];

    BSONObjBuilder bob;
    switch (ele.type()) {
        case BinData: {
            int len;
            const char* ptr = ele.binData(len);
            SHA256Block::computeHash({ConstDataRange(ptr, len)}).appendAsBinData(bob, ""_sd);

            break;
        }
        case String: {
            auto str = ele.valueStringData();
            SHA256Block::computeHash({ConstDataRange(str.rawData(), str.size())})
                .appendAsBinData(bob, ""_sd);
            break;
        }
        default:
            uasserted(ErrorCodes::BadValue, "Can only computeSHA256Block of strings and bindata");
    }

    return bob.obj();
}

/**
 * This function computes a hash value for a document.
 * Specifically, this is the same hash function that is used to form a hashed index,
 * and thus used to generate shard keys for a collection.
 *
 * e.g.
 * > // For a given collection prepared like so:
 * > use mydb
 * > db.mycollection.createIndex({ x: "hashed" })
 * > sh.shardCollection("mydb.mycollection", { x: "hashed" })
 * > // And a sample object like so:
 * > var obj = { x: "Whatever key", y: 2, z: 10.0 }
 * > // The hashed value of the shard key can be acquired by passing in the shard key value:
 * > convertShardKeyToHashed("Whatever key")
 */
BSONObj convertShardKeyToHashed(const BSONObj& a, void* data) {
    uassert(10151, "convertShardKeyToHashed accepts 1 argument", a.nFields() == 1);
    const auto& objEl = a.firstElement();

    auto key = BSONElementHasher::hash64(objEl, BSONElementHasher::DEFAULT_HASH_SEED);
    return BSON("" << key);
}

/**
 * Generate a security token suitable for passing in an OpMsg payload token field.
 *
 * @param user object - { user: 'name', db: 'dbname', tenant: OID }
 * @param secret string - Secret to use for test signing
 * @return string - Compact serialized JWS on an OIDC token.
 */
BSONObj _createSecurityToken(const BSONObj& args, void* data) {
    std::vector<BSONElement> argv;
    args.elems(argv);
    uassert(6161500,
            "_createSecurityToken requires two arguments, an object and a non-empty string",
            (argv.size() == 2) && (argv[0].type() == Object) && (argv[1].type() == String) &&
                !argv[1].valueStringData().empty());

    auto token = auth::ValidatedTenancyScopeFactory::create(
        UserName::parseFromBSON(argv[0]),
        argv[1].valueStringData(),
        auth::ValidatedTenancyScope::TenantProtocol::kDefault,
        auth::ValidatedTenancyScopeFactory::TokenForTestingTag{});
    return BSON("" << token.getOriginalToken());
}

/**
 * Generate an unsigned security token which contains a tenant component.
 * @param object - { tenant: OID, expectPrefix: bool }
 * @return string - Unsigned compact serialized JWS on an OIDC token.
 */
BSONObj _createTenantToken(const BSONObj& args, void* data) {
    uassert(8039400,
            "_createTenantToken requires one argument, and it must be an object",
            args.nFields() == 1 && args.firstElement().isABSONObj());
    const auto obj = args.firstElement().Obj();
    uassert(8154401,
            "_createTenantToken requires field `tenant` of type ObjectId",
            obj.hasField("tenant"_sd) && obj["tenant"_sd].type() == jstOID);
    const auto tenant = TenantId::parseFromBSON(obj["tenant"_sd]);
    const auto expectPrefix = obj["expectPrefix"].booleanSafe();
    const auto token = auth::ValidatedTenancyScopeFactory::create(
        tenant,
        (expectPrefix ? auth::ValidatedTenancyScope::TenantProtocol::kAtlasProxy
                      : auth::ValidatedTenancyScope::TenantProtocol::kDefault),
        auth::ValidatedTenancyScopeFactory::TenantForTestingTag{});
    return BSON("" << token.getOriginalToken());
}

BSONObj replMonitorStats(const BSONObj& a, void* data) {
    uassert(17134,
            "replMonitorStats requires a single string argument (the ReplSet name)",
            a.nFields() == 1 && a.firstElement().type() == String);

    auto name = a.firstElement().valueStringDataSafe();
    auto rsm = ReplicaSetMonitor::get(name.toString());
    if (!rsm) {
        return BSON(""
                    << "no ReplSetMonitor exists by that name");
    }

    BSONObjBuilder result;
    rsm->appendInfo(result);
    // Stats are like {replSetName: {hosts: [{ ... }, { ... }]}}.
    return result.obj()[name].Obj().getOwned();
}

BSONObj shouldRetryWrites(const BSONObj&, void* data) {
    return BSON("" << shellGlobalParams.shouldRetryWrites);
}

BSONObj shouldUseImplicitSessions(const BSONObj&, void* data) {
    return BSON("" << shellGlobalParams.shouldUseImplicitSessions);
}

BSONObj apiParameters(const BSONObj&, void* data) {
    return BSON("" << BSON("apiVersion" << shellGlobalParams.apiVersion << "apiStrict"
                                        << shellGlobalParams.apiStrict << "apiDeprecationErrors"
                                        << shellGlobalParams.apiDeprecationErrors));
}

BSONObj interpreterVersion(const BSONObj& a, void* data) {
    uassert(16453, "interpreterVersion accepts no arguments", a.nFields() == 0);
    return BSON("" << getGlobalScriptEngine()->getInterpreterVersionString());
}

BSONObj fileExistsJS(const BSONObj& a, void*) {
    uassert(40678,
            "fileExists expects one string argument",
            a.nFields() == 1 && a.firstElement().type() == String);
    return BSON("" << fileExists(a.firstElement().str()));
}

BSONObj isInteractive(const BSONObj& a, void*) {
    return BSON("" << shellGlobalParams.runShell);
}

BSONObj numberDecimalsEqual(const BSONObj& input, void*) {
    uassert(5760500, "numberDecimalsEqual expects two arguments", input.nFields() == 2);

    BSONObjIterator i(input);
    auto first = i.next();
    auto second = i.next();
    uassert(5760501,
            "Both the arguments of numberDecimalsEqual should be of type 'NumberDecimal'",
            first.type() == BSONType::NumberDecimal && second.type() == BSONType::NumberDecimal);

    return BSON("" << first.numberDecimal().isEqual(second.numberDecimal()));
}

BSONObj numberDecimalsAlmostEqual(const BSONObj& input, void*) {
    if (input.nFields() != 3) {
        uassert(9193200,
                "numberDecimalsAlmostEqual expects three arguments, two NumberDecimal inputs and an"
                "integer for how many decimal places to check.",
                input.nFields() == 3);
    }

    BSONObjIterator i(input);
    auto first = i.next();
    auto second = i.next();
    auto third = i.next();

    // Type-check arguments before performing any calculations.
    if (!(first.type() == BSONType::NumberDecimal && second.type() == BSONType::NumberDecimal &&
          third.isNumber())) {
        return BSON("" << false);
    }

    auto a = first.numberDecimal();
    auto b = second.numberDecimal();

    // 10.0 is used frequently in the rest of the function, so save it to a variable.
    auto ten = Decimal128(10);
    auto exponent = a.toAbs().logarithm(ten).round();

    // Early exit for zero, infinity and NaN cases.
    if ((a.isZero() && b.isZero()) || (a.isNaN() && b.isNaN()) ||
        (a.isInfinite() && b.isInfinite() && (a.isNegative() == b.isNegative()))) {
        return BSON("" << true /* isErrorAcceptable */);
    } else if (!a.isZero() && !b.isZero()) {
        // Return early if arguments are not the same order of magnitude.
        if (exponent != b.toAbs().logarithm(ten).round()) {
            return BSON("" << false);
        }

        // Put the whole number behind the decimal point.
        if (!exponent.isZero()) {
            a = a.divide(ten.power(exponent));
            b = b.divide(ten.power(exponent));
        }
    }

    auto places = third.numberDecimal();
    auto isErrorAcceptable = a.subtract(b)
                                 .toAbs()
                                 .multiply(ten.power(places, Decimal128::kRoundTowardZero))
                                 .round(Decimal128::kRoundTowardZero) == Decimal128(0);

    return BSON("" << isErrorAcceptable);
}


class GoldenTestContextShell : public unittest::GoldenTestContextBase {
public:
    explicit GoldenTestContextShell(const unittest::GoldenTestConfig* config,
                                    boost::filesystem::path testPath,
                                    bool validateOnClose)
        : GoldenTestContextBase(config, testPath, validateOnClose, [this](auto const&... args) {
              return onError(args...);
          }) {}

    // Disable move/copy because onError captures 'this' address.
    GoldenTestContextShell(GoldenTestContextShell&&) = delete;


protected:
    void onError(const std::string& message,
                 const std::string& actualStr,
                 const boost::optional<std::string>& expectedStr) {
        throw GoldenTestContextShellFailure{
            message, getActualOutputPath().string(), getExpectedOutputPath().string()};
    }
};

std::string GoldenTestContextShellFailure::toString() const {
    return "Test output verification failed: {}\n"
           "Actual output file: {}, "
           "expected output file: {}"
           ""_format(message, actualOutputFile, expectedOutputFile);
}

void GoldenTestContextShellFailure::diff() const {
    auto cmd = unittest::GoldenTestEnvironment::getInstance()->diffCmd(expectedOutputFile,
                                                                       actualOutputFile);
    int status = std::system(cmd.c_str());
    // Ignore return code: 'diff' returns non-zero when files differ, which we expect.
    (void)status;
}

unittest::GoldenTestConfig goldenTestConfig{"jstests/expected_output"};
boost::optional<GoldenTestContextShell> goldenTestContext;

void closeGoldenTestContext() {
    if (goldenTestContext) {
        goldenTestContext->verifyOutput();
        goldenTestContext = boost::none;
    }
}

BSONObj _openGoldenData(const BSONObj& input, void*) {
    uassert(6741513,
            str::stream() << "_openGoldenData expects 2 arguments: 'testPath' and 'config'.",
            input.nFields() == 2);

    BSONObjIterator i(input);
    auto testPathArg = i.next();
    auto configArg = i.next();
    invariant(i.next().eoo());

    uassert(6741512,
            "_openGoldenData 'testPath' must be a string",
            testPathArg.type() == BSONType::String);
    auto testPath = testPathArg.valueStringData();

    uassert(6741511,
            "_openGoldenData 'config' must be an object",
            configArg.type() == BSONType::Object);
    auto config = configArg.Obj();
    goldenTestConfig = unittest::GoldenTestConfig::parseFromBson(config);

    goldenTestContext.emplace(&goldenTestConfig, testPath.toString(), true /*validateOnClose*/);

    return {};
}
BSONObj _writeGoldenData(const BSONObj& input, void*) {
    uassert(6741510,
            str::stream() << "_writeGoldenData expects 1 argument: 'content'. got: " << input,
            input.nFields() == 1);

    BSONObjIterator i(input);
    auto contentArg = i.next();
    invariant(i.next().eoo());

    uassert(6741509,
            "_writeGoldenData 'content' must be a string",
            contentArg.type() == BSONType::String);
    auto content = contentArg.valueStringData();

    uassert(6741508, "_writeGoldenData() requires _openGoldenData() first", goldenTestContext);
    auto& os = goldenTestContext->outStream();
    os << content;

    return {};
}
BSONObj _closeGoldenData(const BSONObj& input, void*) {
    uassert(6741507,
            str::stream() << "_closeGoldenData expects 0 arguments. got: " << input,
            input.nFields() == 0);

    closeGoldenTestContext();

    return {};
}

/**
 * This function is a light-weight BSON builder to support building an arbitrary BSON in shell.
 * This function is particularly useful for testing invalid BSON object which is impossible to be
 * constructed from JS shell environment.
 *
 * The field names and values in the `args` are in the order like: name1, value1, name2, value...
 *
 * args:
 *   "0": string; field name for the first field
 *   "1": any; value for the first field
 *   "2": string; field name for the second field
 *   "3": any; value for the second field
 *   "4": ...
 *
 * e.g.
 * > let bsonObj = _buildBsonObj("_id", 1, "a", 2, "foo", "bar");
 * > printjson(bsonObj)
 * { "_id" : 1, "a" : 2, "foo" : "bar" }
 */
BSONObj _buildBsonObj(const BSONObj& args, void*) {
    ::mongo::BSONObjBuilder builder(64);
    int fieldNum = 0;   // next field name in numeric form
    BSONElement name;   // next pipe relative path
    BSONElement value;  // next pipe relative path

    do {
        name = args.getField(std::to_string(fieldNum++));
        value = args.getField(std::to_string(fieldNum++));
        if (name.type() == BSONType::EOO) {
            break;
        }

        uassert(9197700,
                str::stream() << "BSON field name must not contain null terminators.",
                std::string::npos == name.str().find('\0'));
        uassert(7587900,
                str::stream() << "BSON field name must be a string: " << name,
                name.type() == BSONType::String);
        uassert(7587901,
                str::stream() << "Missing BSON field value: " << value,
                value.type() != BSONType::EOO);
        builder << name.str() << value;
    } while (name.type() != BSONType::EOO);
    return BSON("" << builder.obj());
}

/*
 * The following code has been updated to remove unnecessary content and better comply
 * with MongoDB coding standards.  The original source code can be found at:
 * FNV 1a 64 bit: http://www.isthe.com/chongo/src/fnv/hash_64a.c
 */
#define FNV1A_64_INIT ((uint64_t)0xcbf29ce484222325ULL)
static inline uint64_t fnv_64a_buf(const void* buf, size_t len, uint64_t hval) {
    const unsigned char* bp = (const unsigned char*)buf; /* start of buffer */
    const unsigned char* be = bp + len;                  /* beyond end of buffer */
    while (bp < be) {
        hval ^= (uint64_t)*bp++;
        hval += (hval << 1) + (hval << 4) + (hval << 5) + (hval << 7) + (hval << 8) + (hval << 40);
    }

    return (hval);
}

BSONObj _fnvHashToHexString(const BSONObj& args, void*) {
    uassert(8423397,
            "_fnvHashToHexString expects one string argument",
            args.nFields() == 1 && args.firstElement().type() == String);

    auto input = args.firstElement().str();
    auto hashed = fnv_64a_buf(input.c_str(), input.size(), FNV1A_64_INIT);
    return BSON("" << fmt::format("{0:x}", hashed));
}

// Comparison function for sorting BSON elements in an array.
bool cmpBSONObjs(const BSONObj& lhs, const BSONObj& rhs) {
    // Use the woCompare method with no bits set, so field names are ignored. This is helpful for
    // comparing elements in an array without considering their initial ordering/id.
    return lhs.woCompare(rhs, BSONObj(), false, nullptr) < 0;
}

void sortBSONObjectInternallyHelper(const BSONObj& input,
                                    BSONObjBuilder& bob,
                                    NormalizationOptsSet opts);

// Helper for `sortBSONObjectInternally`, handles a BSONElement for different recursion cases.
void sortBSONElementInternally(const BSONElement& el,
                               BSONObjBuilder& bob,
                               NormalizationOptsSet opts) {
    if (el.type() == BSONType::Array) {
        std::vector<BSONElement> arr = el.Array();

        if (isSet(opts, NormalizationOpts::kSortArrays)) {
            // Sort each individual BSONElement in the array internally.
            std::vector<BSONObj> sortedObjs;
            sortedObjs.reserve(arr.size());

            for (const auto& child : arr) {
                BSONObjBuilder tmp;
                sortBSONElementInternally(child, tmp, opts);
                sortedObjs.emplace_back(tmp.obj());
            }

            // Sort the top-level elements in the array among each other. The elements have already
            // been sorted individually.
            std::sort(sortedObjs.begin(), sortedObjs.end(), cmpBSONObjs);

            // Append the elements back to the top-level BSONObjBuilder.
            BSONArrayBuilder sub(bob.subarrayStart(el.fieldNameStringData()));
            for (const auto& child : sortedObjs) {
                sub.append(child.firstElement());
            }
            sub.doneFast();
        } else {
            BSONObjBuilder sub(bob.subarrayStart(el.fieldNameStringData()));
            for (const auto& child : arr) {
                sortBSONElementInternally(child, sub, opts);
            }
            sub.doneFast();
        }
    } else if (el.type() == BSONType::Object) {
        BSONObjBuilder sub(bob.subobjStart(el.fieldNameStringData()));
        sortBSONObjectInternallyHelper(el.Obj(), sub, opts);
        sub.doneFast();
    } else {
        bob.append(el);
    }
}

void sortBSONObjectInternallyHelper(const BSONObj& input,
                                    BSONObjBuilder& bob,
                                    NormalizationOptsSet opts) {
    BSONObjIteratorSorted it(input);
    while (it.more()) {
        sortBSONElementInternally(it.next(), bob, opts);
    }
}

/**
 * Returns a new BSON with the same field/value pairings, but is recursively sorted by the fields.
 * By default, arrays are not sorted unless NormalizationOptsSet has the kSortArrays bit set.
 */
BSONObj sortBSONObjectInternally(const BSONObj& input,
                                 NormalizationOptsSet opts = NormalizationOpts::kSortBSON) {
    BSONObjBuilder bob(input.objsize());
    sortBSONObjectInternallyHelper(input, bob, opts);
    return bob.obj();
}

void sortQueryResults(std::vector<BSONObj>& input) {
    std::sort(input.begin(), input.end(), [&](const BSONObj& lhs, const BSONObj& rhs) {
        return SimpleBSONObjComparator::kInstance.evaluate(lhs < rhs);
    });
}

void normalizeNumericElementsHelper(const BSONObj& input, BSONObjBuilder& bob);

void normalizeNumericElements(const BSONElement& el, BSONObjBuilder& bob) {
    switch (el.type()) {
        case NumberInt:
        case NumberLong:
        case NumberDouble:
        case NumberDecimal: {
            bob.append(el.fieldName(), el.numberDecimal().normalize());
            break;
        }
        case Array: {
            BSONObjBuilder sub(bob.subarrayStart(el.fieldNameStringData()));
            for (const auto& child : el.Array()) {
                normalizeNumericElements(child, sub);
            }
            sub.doneFast();
            break;
        }
        case Object: {
            BSONObjBuilder sub(bob.subobjStart(el.fieldNameStringData()));
            normalizeNumericElementsHelper(el.Obj(), sub);
            sub.doneFast();
            break;
        }
        default:
            bob.append(el);
            break;
    }
}

void normalizeNumericElementsHelper(const BSONObj& input, BSONObjBuilder& bob) {
    BSONObjIterator it(input);
    while (it.more()) {
        normalizeNumericElements(it.next(), bob);
    }
}

/**
 * Returns a new BSONObj with the same field/value pairings, but with numeric types converted into
 * Decimal128 and normalized to maximum precision. For example, NumberInt(1), NumberLong(1), 1.0,
 * and NumberDecimal('1.0000') would be normalized into the same number.
 */
BSONObj normalizeNumerics(const BSONObj& input) {
    BSONObjBuilder bob(input.objsize());
    normalizeNumericElementsHelper(input, bob);
    return bob.obj();
}

void roundFloatingPointNumericElementsHelper(const BSONObj& input, BSONObjBuilder& bob);

void roundFloatingPointNumericElements(const BSONElement& el, BSONObjBuilder& bob) {
    switch (el.type()) {
        case NumberDouble: {
            // Take advantage of Decimal128's ability to round to 15 digits at construction time.
            bob.append(el.fieldName(),
                       Decimal128(el.numberDouble(), Decimal128::kRoundTo15Digits).toDouble());
            break;
        }
        case Array: {
            BSONObjBuilder sub(bob.subarrayStart(el.fieldNameStringData()));
            for (const auto& child : el.Array()) {
                roundFloatingPointNumericElements(child, sub);
            }
            sub.doneFast();
            break;
        }
        case Object: {
            BSONObjBuilder sub(bob.subobjStart(el.fieldNameStringData()));
            roundFloatingPointNumericElementsHelper(el.Obj(), sub);
            sub.doneFast();
            break;
        }
        default:
            bob.append(el);
            break;
    }
}

void roundFloatingPointNumericElementsHelper(const BSONObj& input, BSONObjBuilder& bob) {
    BSONObjIterator it(input);
    while (it.more()) {
        roundFloatingPointNumericElements(it.next(), bob);
    }
}

/**
 * Returns a new BSONObj with the same field/value pairings, but with floating-point types rounded
 * to 15 digits of precision. For example, 1.000000000000001 and NumberDecimal('1') would
 * be normalized into the same number.
 */
BSONObj roundFloatingPointNumerics(const BSONObj& input) {
    BSONObjBuilder bob(input.objsize());
    roundFloatingPointNumericElementsHelper(input, bob);
    return bob.obj();
}

void removeNullAndUndefinedElementsHelper(const BSONObj& input, BSONObjBuilder& bob);

template <typename Builder>
void removeNullAndUndefinedElements(const BSONElement& el, Builder& bob) {
    switch (el.type()) {
        case Undefined:
        case jstNULL:
            // Don't append the element if it's null or undefined.
            break;
        case Array: {
            auto appendArrayElements = [&](auto& sub) {
                for (const auto& child : el.Array()) {
                    removeNullAndUndefinedElements(child, sub);
                }
                sub.doneFast();
            };

            if constexpr (std::is_same_v<Builder, BSONObjBuilder>) {
                BSONArrayBuilder sub(bob.subarrayStart(el.fieldNameStringData()));
                appendArrayElements(sub);
            } else if constexpr (std::is_same_v<Builder, BSONArrayBuilder>) {
                BSONArrayBuilder sub(bob.subarrayStart());
                appendArrayElements(sub);
            }
            break;
        }
        case Object: {
            if constexpr (std::is_same_v<Builder, BSONObjBuilder>) {
                BSONObjBuilder sub(bob.subobjStart(el.fieldNameStringData()));
                removeNullAndUndefinedElementsHelper(el.Obj(), sub);
                sub.doneFast();
            } else if constexpr (std::is_same_v<Builder, BSONArrayBuilder>) {
                BSONObjBuilder sub(bob.subobjStart());
                removeNullAndUndefinedElementsHelper(el.Obj(), sub);
                sub.doneFast();
            }
            break;
        }
        default:
            bob.append(el);
            break;
    }
}

void removeNullAndUndefinedElementsHelper(const BSONObj& input, BSONObjBuilder& bob) {
    BSONObjIterator it(input);
    while (it.more()) {
        removeNullAndUndefinedElements(it.next(), bob);
    }
}

/**
 * Returns a new BSONObj with null and undefined elements removed. This will make result sets with
 * missing elements match those with null or defined field/array values. For instance, {a: null},
 * {a: undefined}, and {} will be considered equal.
 */
BSONObj removeNullAndUndefined(const BSONObj& input) {
    BSONObjBuilder bob(input.objsize());
    removeNullAndUndefinedElementsHelper(input, bob);
    return bob.obj();
}

BSONObj normalizeBSONObj(const BSONObj& input, NormalizationOptsSet opts) {
    BSONObj result = input;
    if (isSet(opts, NormalizationOpts::kConflateNullAndMissing)) {
        result = removeNullAndUndefined(result);
    }
    if (isSet(opts, NormalizationOpts::kRoundFloatingPointNumerics)) {
        result = roundFloatingPointNumerics(result);
    }
    if (isSet(opts, NormalizationOpts::kNormalizeNumerics)) {
        result = normalizeNumerics(result);
    }
    if (isSet(opts, NormalizationOpts::kSortBSON)) {
        result = sortBSONObjectInternally(result, opts);
    }
    return result;
}

/*
 * Takes two arrays of documents, and returns whether they contain the same set of BSON Objects. The
 * BSON do not need to be in the same order for this to return true. Has no special logic for
 * handling double/NumberDecimal closeness.
 */
BSONObj _resultSetsEqualUnordered(const BSONObj& input, void*) {
    BSONObjIterator i(input);
    uassert(9422901, "_resultSetsEqualUnordered expects two arguments", i.more());
    auto first = i.next();
    uassert(9422902, "_resultSetsEqualUnordered expects two arguments", i.more());
    auto second = i.next();
    uassert(9193201,
            str::stream() << "_resultSetsEqualUnordered expects two arrays of containing objects "
                             "as input received "
                          << first.type() << " and " << second.type(),
            first.type() == BSONType::Array && second.type() == BSONType::Array);

    auto firstAsBson = first.Array();
    auto secondAsBson = second.Array();

    for (const auto& el : firstAsBson) {
        uassert(9193202,
                str::stream() << "_resultSetsEqualUnordered expects all elements of input arrays "
                                 "to be objects, received "
                              << el.type(),
                el.type() == BSONType::Object);
    }
    for (const auto& el : secondAsBson) {
        uassert(9193203,
                str::stream() << "_resultSetsEqualUnordered expects all elements of input arrays "
                                 "to be objects, received "
                              << el.type(),
                el.type() == BSONType::Object);
    }

    if (firstAsBson.size() != secondAsBson.size()) {
        return BSON("" << false);
    }

    // Optimistically assume they're already in the same order.
    if (first.binaryEqualValues(second)) {
        return BSON("" << true);
    }

    std::vector<BSONObj> firstSorted;
    std::vector<BSONObj> secondSorted;
    for (size_t i = 0; i < firstAsBson.size(); i++) {
        firstSorted.push_back(sortBSONObjectInternally(firstAsBson[i].Obj()));
        secondSorted.push_back(sortBSONObjectInternally(secondAsBson[i].Obj()));
    }

    sortQueryResults(firstSorted);
    sortQueryResults(secondSorted);

    for (size_t i = 0; i < firstSorted.size(); i++) {
        if (!firstSorted[i].binaryEqual(secondSorted[i])) {
            return BSON("" << false);
        }
    }

    return BSON("" << true);
}

/*
 * Takes two strings and a valid collation document and returns the comparison result (a number < 0
 * if 'left' is less than 'right', a number > 0 if 'left' is greater than 'right', and 0 if 'left'
 * and 'right' are equal) with respect to the collation
 * Refer to https://www.mongodb.com/docs/manual/reference/collation and
 * https://unicode-org.github.io/icu/userguide/collation for the expected behaviour when collation
 * is specified
 */
BSONObj _compareStringsWithCollation(const BSONObj& input, void*) {
    BSONObjIterator i(input);

    uassert(9367800, "Expected left argument", i.more());
    auto left = i.next();
    uassert(9367801, "Left argument should be a string", left.type() == BSONType::String);

    uassert(9367802, "Expected right argument", i.more());
    auto right = i.next();
    uassert(9367803, "Right argument should be string", right.type() == BSONType::String);

    uassert(9367804, "Expected collation argument", i.more());
    auto collatorSpec = i.next();
    uassert(9367805, "Expected a collation object", collatorSpec.type() == BSONType::Object);

    CollatorFactoryICU collationFactory;
    auto collator = uassertStatusOK(collationFactory.makeFromBSON(collatorSpec.Obj()));

    if (collator == nullptr) {
        // 'makeFromBSON' will return a nullptr if the collator spec contains '{locale:
        // "simple"}'. In this case we can go with a simple binary comparison.
        return BSON("" << left.valueStringData().compare(right.valueStringData()));
    }

    int cmp = collator->compare(left.valueStringData(), right.valueStringData());
    return BSON("" << cmp);
}

void installShellUtils(Scope& scope) {
    scope.injectNative("getMemInfo", JSGetMemInfo);
    scope.injectNative("_createSecurityToken", _createSecurityToken);
    scope.injectNative("_createTenantToken", _createTenantToken);
    scope.injectNative("_replMonitorStats", replMonitorStats);
    scope.injectNative("_srand", JSSrand);
    scope.injectNative("_rand", JSRand);
    scope.injectNative("_isWindows", isWindows);
    scope.injectNative("_setShellFailPoint", _setShellFailPoint);
    scope.injectNative("interpreterVersion", interpreterVersion);
    scope.injectNative("getBuildInfo", getBuildInfo);
    scope.injectNative("computeSHA256Block", computeSHA256Block);
    scope.injectNative("convertShardKeyToHashed", convertShardKeyToHashed);
    scope.injectNative("fileExists", fileExistsJS);
    scope.injectNative("isInteractive", isInteractive);
    scope.injectNative("numberDecimalsEqual", numberDecimalsEqual);
    scope.injectNative("numberDecimalsAlmostEqual", numberDecimalsAlmostEqual);
    scope.injectNative("_openGoldenData", _openGoldenData);
    scope.injectNative("_writeGoldenData", _writeGoldenData);
    scope.injectNative("_closeGoldenData", _closeGoldenData);
    scope.injectNative("_buildBsonObj", _buildBsonObj);
    scope.injectNative("_fnvHashToHexString", _fnvHashToHexString);
    scope.injectNative("_resultSetsEqualUnordered", _resultSetsEqualUnordered);
    scope.injectNative("_compareStringsWithCollation", _compareStringsWithCollation);

    installShellUtilsLauncher(scope);
    installShellUtilsExtended(scope);
}

void setEnterpriseShellCallback(EnterpriseShellCallback* callback) {
    enterpriseCallback = callback;
}

void initializeEnterpriseScope(Scope& scope) {
    if (enterpriseCallback != nullptr) {
        enterpriseCallback(scope);
    }
}

void initScope(Scope& scope) {
    // Need to define this method before JSFiles::utils is executed.
    scope.injectNative("_shouldRetryWrites", shouldRetryWrites);
    scope.injectNative("_shouldUseImplicitSessions", shouldUseImplicitSessions);
    scope.injectNative("_apiParameters", apiParameters);
    scope.externalSetup();
    mongo::shell_utils::installShellUtils(scope);
    scope.execSetup(JSFiles::servers);
    scope.execSetup(JSFiles::servers_misc);
    scope.execSetup(JSFiles::data_consistency_checker);
    scope.execSetup(JSFiles::bridge);
    scope.execSetup(JSFiles::feature_compatibility_version);

    initializeEnterpriseScope(scope);

    scope.injectNative("benchRun", BenchRunner::benchRunSync);  // alias
    scope.injectNative("benchRunSync", BenchRunner::benchRunSync);
    scope.injectNative("benchRunOnce", BenchRunner::benchRunOnce);
    scope.injectNative("benchStart", BenchRunner::benchStart);
    scope.injectNative("benchFinish", BenchRunner::benchFinish);

    if (!dbConnect.empty()) {
        uassert(12513, "connect failed", scope.exec(dbConnect, "(connect)", false, true, false));
    }
}

Prompter::Prompter(const std::string& prompt) : _prompt(prompt), _confirmed() {}

bool Prompter::confirm() {
    if (_confirmed) {
        return true;
    }

    // The printf and scanf functions provide thread safe i/o.

    printf("\n%s (y/n): ", _prompt.c_str());

    char yn = '\0';
    int nScanMatches = scanf("%c", &yn);
    bool matchedY = (nScanMatches == 1 && (yn == 'y' || yn == 'Y'));

    return _confirmed = matchedY;
}

ConnectionRegistry::ConnectionRegistry() = default;

void ConnectionRegistry::registerConnection(DBClientBase& client, StringData uri) {
    BSONObj info;
    BSONObj command;
    // If apiStrict is set override it, whatsmyuri is not in the Stable API.
    if (client.getApiParameters().getStrict()) {
        command = BSON("whatsmyuri" << 1 << "apiStrict" << false);
    } else {
        command = BSON("whatsmyuri" << 1);
    }

    if (client.runCommand(DatabaseName::kAdmin, command, info)) {
        stdx::lock_guard<stdx::mutex> lk(_mutex);
        _connectionUris[uri.toString()].insert(info["you"].str());
    }
}

void ConnectionRegistry::killOperationsOnAllConnections(bool withPrompt) const {
    Prompter prompter("do you want to kill the current op(s) on the server?");
    stdx::lock_guard<stdx::mutex> lk(_mutex);
    for (auto& connection : _connectionUris) {
        std::string errmsg;

        auto uri = uassertStatusOK(MongoURI::parse(connection.first));
        std::unique_ptr<DBClientBase> conn(uri.connect("MongoDB Shell", errmsg));
        if (!conn) {
            continue;
        }

        const std::set<std::string>& uris = connection.second;

        BSONObj currentOpRes;
        conn->runCommand(DatabaseName::kAdmin, BSON("currentOp" << 1), currentOpRes);
        if (!currentOpRes["inprog"].isABSONObj()) {
            // We don't have permissions (or the call didn't succeed) - go to the next connection.
            continue;
        }
        auto inprog = currentOpRes["inprog"].embeddedObject();
        for (const auto& op : inprog) {
            // For sharded clusters, `client_s` is used instead and `client` is not present.
            std::string client;
            if (auto elem = op["client"]) {
                // mongod currentOp client
                if (elem.type() != String) {
                    std::cout << "Ignoring operation " << op["opid"].toString(false)
                              << "; expected 'client' field in currentOp response to have type "
                                 "string, but found "
                              << typeName(elem.type()) << std::endl;
                    continue;
                }
                client = elem.str();
            } else if (auto elem = op["client_s"]) {
                // mongos currentOp client
                if (elem.type() != String) {
                    std::cout << "Ignoring operation " << op["opid"].toString(false)
                              << "; expected 'client_s' field in currentOp response to have type "
                                 "string, but found "
                              << typeName(elem.type()) << std::endl;
                    continue;
                }
                client = elem.str();
            } else {
                // Internal operation, like TTL index.
                continue;
            }
            if (uris.count(client)) {
                if (!withPrompt || prompter.confirm()) {
                    BSONObj info;
                    conn->runCommand(
                        DatabaseName::kAdmin, BSON("killOp" << 1 << "op" << op["opid"]), info);
                } else {
                    return;
                }
            }
        }
    }
}

ConnectionRegistry connectionRegistry;

void onConnect(DBClientBase& c, StringData uri) {
    if (shellGlobalParams.nokillop.load()) {
        return;
    }

    connectionRegistry.registerConnection(c, uri);
}

bool fileExists(const std::string& file) {
    try {
#ifdef _WIN32
        boost::filesystem::path p(toWideString(file.c_str()));
#else
        boost::filesystem::path p(file);
#endif
        return boost::filesystem::exists(p);
    } catch (...) {
        return false;
    }
}

}  // namespace shell_utils
}  // namespace mongo
