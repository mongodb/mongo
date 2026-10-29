// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/storage/ident.h"

#include "mongo/bson/util/builder.h"
#include "mongo/db/database_name.h"
#include "mongo/db/database_name_util.h"
#include "mongo/db/storage/storage_engine.h"
#include "mongo/util/serialization_context.h"
#include "mongo/util/str.h"
#include "mongo/util/uuid.h"

#include <string_view>

namespace mongo {
using namespace std::literals::string_view_literals;

namespace {
constexpr auto kCollectionIdentStem = "collection"sv;
constexpr auto kIndexIdentStem = "index"sv;
constexpr auto kInternalIdentStem = "internal"sv;

// Does not escape letters, digits, '.', or '_'.
// Otherwise escapes to a '.' followed by a zero-filled 2- or 3-digit decimal number.
// Note that this escape table does not produce a 1:1 mapping to and from dbname, and
// collisions are possible.
// For example:
//     "db.123", "db\0143", and "db\073" all escape to "db.123".
//       {'d','b','1','2','3'} => "d" + "b" + "." + "1" + "2" + "3" => "db.123"
//       {'d','b','\x0c','3'}  => "d" + "b" + ".12" + "3"           => "db.123"
//       {'d','b','\x3b'}      => "d" + "b" + ".123"                => "db.123"
constexpr std::array<std::string_view, 256> escapeTable = {
    ".00"sv,  ".01"sv,  ".02"sv,  ".03"sv,  ".04"sv,  ".05"sv,  ".06"sv,  ".07"sv,  ".08"sv,
    ".09"sv,  ".10"sv,  ".11"sv,  ".12"sv,  ".13"sv,  ".14"sv,  ".15"sv,  ".16"sv,  ".17"sv,
    ".18"sv,  ".19"sv,  ".20"sv,  ".21"sv,  ".22"sv,  ".23"sv,  ".24"sv,  ".25"sv,  ".26"sv,
    ".27"sv,  ".28"sv,  ".29"sv,  ".30"sv,  ".31"sv,  ".32"sv,  ".33"sv,  ".34"sv,  ".35"sv,
    ".36"sv,  ".37"sv,  ".38"sv,  ".39"sv,  ".40"sv,  ".41"sv,  ".42"sv,  ".43"sv,  ".44"sv,
    ".45"sv,  "."sv,    ".47"sv,  "0"sv,    "1"sv,    "2"sv,    "3"sv,    "4"sv,    "5"sv,
    "6"sv,    "7"sv,    "8"sv,    "9"sv,    ".58"sv,  ".59"sv,  ".60"sv,  ".61"sv,  ".62"sv,
    ".63"sv,  ".64"sv,  "A"sv,    "B"sv,    "C"sv,    "D"sv,    "E"sv,    "F"sv,    "G"sv,
    "H"sv,    "I"sv,    "J"sv,    "K"sv,    "L"sv,    "M"sv,    "N"sv,    "O"sv,    "P"sv,
    "Q"sv,    "R"sv,    "S"sv,    "T"sv,    "U"sv,    "V"sv,    "W"sv,    "X"sv,    "Y"sv,
    "Z"sv,    ".91"sv,  ".92"sv,  ".93"sv,  ".94"sv,  "_"sv,    ".96"sv,  "a"sv,    "b"sv,
    "c"sv,    "d"sv,    "e"sv,    "f"sv,    "g"sv,    "h"sv,    "i"sv,    "j"sv,    "k"sv,
    "l"sv,    "m"sv,    "n"sv,    "o"sv,    "p"sv,    "q"sv,    "r"sv,    "s"sv,    "t"sv,
    "u"sv,    "v"sv,    "w"sv,    "x"sv,    "y"sv,    "z"sv,    ".123"sv, ".124"sv, ".125"sv,
    ".126"sv, ".127"sv, ".128"sv, ".129"sv, ".130"sv, ".131"sv, ".132"sv, ".133"sv, ".134"sv,
    ".135"sv, ".136"sv, ".137"sv, ".138"sv, ".139"sv, ".140"sv, ".141"sv, ".142"sv, ".143"sv,
    ".144"sv, ".145"sv, ".146"sv, ".147"sv, ".148"sv, ".149"sv, ".150"sv, ".151"sv, ".152"sv,
    ".153"sv, ".154"sv, ".155"sv, ".156"sv, ".157"sv, ".158"sv, ".159"sv, ".160"sv, ".161"sv,
    ".162"sv, ".163"sv, ".164"sv, ".165"sv, ".166"sv, ".167"sv, ".168"sv, ".169"sv, ".170"sv,
    ".171"sv, ".172"sv, ".173"sv, ".174"sv, ".175"sv, ".176"sv, ".177"sv, ".178"sv, ".179"sv,
    ".180"sv, ".181"sv, ".182"sv, ".183"sv, ".184"sv, ".185"sv, ".186"sv, ".187"sv, ".188"sv,
    ".189"sv, ".190"sv, ".191"sv, ".192"sv, ".193"sv, ".194"sv, ".195"sv, ".196"sv, ".197"sv,
    ".198"sv, ".199"sv, ".200"sv, ".201"sv, ".202"sv, ".203"sv, ".204"sv, ".205"sv, ".206"sv,
    ".207"sv, ".208"sv, ".209"sv, ".210"sv, ".211"sv, ".212"sv, ".213"sv, ".214"sv, ".215"sv,
    ".216"sv, ".217"sv, ".218"sv, ".219"sv, ".220"sv, ".221"sv, ".222"sv, ".223"sv, ".224"sv,
    ".225"sv, ".226"sv, ".227"sv, ".228"sv, ".229"sv, ".230"sv, ".231"sv, ".232"sv, ".233"sv,
    ".234"sv, ".235"sv, ".236"sv, ".237"sv, ".238"sv, ".239"sv, ".240"sv, ".241"sv, ".242"sv,
    ".243"sv, ".244"sv, ".245"sv, ".246"sv, ".247"sv, ".248"sv, ".249"sv, ".250"sv, ".251"sv,
    ".252"sv, ".253"sv, ".254"sv, ".255"sv};

StringBuilder buildIdentPrefix(const DatabaseName& dbName,
                               std::string_view identType,
                               bool directoryPerDB,
                               bool directoryForIndexes) {
    StringBuilder buf;
    if (directoryPerDB) {
        buf << ident::createDBNamePathComponent(dbName) << '/';
    }
    buf << identType;
    buf << (directoryForIndexes ? '/' : '-');
    return buf;
}

std::string generateNewIdent(const DatabaseName& dbName,
                             std::string_view identType,
                             const boost::optional<std::string_view>& optIdentUniqueTag,
                             bool directoryPerDB,
                             bool directoryForIndexes) {
    auto buf = buildIdentPrefix(dbName, identType, directoryPerDB, directoryForIndexes);
    // The suffix of an ident serves as a unique identifier.
    //
    // (v8.2+) Suffix new idents with a unique UUID.
    //
    // Idents created before v8.2 are suffixed with a <counter> + <random number> combination.
    // Future versions of the server must support both formats.
    if (optIdentUniqueTag) {
        buf << *optIdentUniqueTag;
    } else {
        buf << UUID::gen();
    }
    return buf.str();
}

enum class IdentType { collection, index, internal };

boost::optional<IdentType> getIdentType(std::string_view str) {
    if (str == kCollectionIdentStem)
        return IdentType::collection;
    if (str == kIndexIdentStem)
        return IdentType::index;
    if (str == kInternalIdentStem)
        return IdentType::internal;
    return boost::none;
}
struct ParsedIdent {
    IdentType identType;
    std::string_view uniqueTag;
    boost::optional<std::string_view> dbName;
};

boost::optional<ParsedIdent> validateIdent(boost::optional<std::string_view> dbName,
                                           std::string_view identType,
                                           std::string_view uniqueTag) {
    // Ident type must be one of a fixed set of values
    auto parsedIdentType = getIdentType(identType);
    if (!parsedIdentType)
        return boost::none;
    if (!ident::validateTag(uniqueTag))
        return boost::none;

    // If the dbName is present it must be non-empty, must not change when escaped with
    // createDBNamePathComponent(), and must not be "." or ".."
    if (dbName) {
        if (dbName->empty() || dbName == "."sv || dbName == ".."sv)
            return boost::none;
        for (char c : *dbName) {
            if (escapeTable[c].size() != 1)
                return boost::none;
        }
    }

    return ParsedIdent{*parsedIdentType, uniqueTag, dbName};
}

// Valid idents can be one of the following formats:
//
// 1. "$identType-$uniqueTag" (default)
// 2. "$identType/$uniqueTag" (directoryForIndexes but not directoryPerDB)
// 3. "$dbName/$identType-$uniqueTag" (directoryPerDB but not directoryForIndexes)
// 4. "$dbName/$identType/$uniqueTag" (both directoryForIndexes and directoryPerDB)
//
// $identType must be one of "collection", "index", or "internal".
// $dbName is a string escaped by createDBNamePathComponent().
// $uniqueTag is fairly free-form, but must not start with an ident type or contain any characters
// which would be special when interpreted as a path.
boost::optional<ParsedIdent> parseIdent(std::string_view str) {
    struct Parts {
        char delim;
        std::string_view head;
        std::string_view tail;
    };
    auto split = [](std::string_view in, std::string_view delims) -> boost::optional<Parts> {
        auto pos = in.find_first_of(delims);
        if (pos == in.npos)
            return {};
        return Parts{in[pos], in.substr(0, pos), in.substr(pos + 1)};
    };

    auto tok1 = split(str, "/-");
    if (!tok1) {
        // All formats have at least an ident type followed by a separator
        return boost::none;
    }

    if (tok1->delim == '-') {
        // Format 1: "$identType-$uniqueTag"
        return validateIdent({}, tok1->head, tok1->tail);
    }
    invariant(tok1->delim == '/');

    auto tok2 = split(tok1->tail, "/-");
    if (!tok2) {
        // Format 2: "$identType/$uniqueTag"
        return validateIdent({}, tok1->head, tok1->tail);
    }
    if (tok2->delim == '/') {
        // This is the only format with two slashes
        // Format 4: "$dbName/$identType/$uniqueTag"
        return validateIdent(tok1->head, tok2->head, tok2->tail);
    }
    invariant(tok2->delim == '-');

    // "index/collection-something" is ambiguous: it could be either be a collection in a db named
    // index with directoryPerDB=true, or an index with directoryForIndexes=true and
    // directoryPerDB=false. We assume here that the unique tag won't start with a known ident type,
    // and thus it's a collection in the db named "index".
    if (auto parsed = validateIdent(tok1->head, tok2->head, tok2->tail)) {
        // Format 3: "$dbName/$identType-$uniqueTag"
        return parsed;
    }
    if (auto identType = getIdentType(tok1->head)) {
        // Format 2: "$identType/$uniqueTag"
        if (!ident::validateTag(tok1->tail))
            return boost::none;
        return ParsedIdent{*identType, tok1->tail, boost::none};
    }
    return boost::none;
}
}  // namespace

namespace ident {

std::string generateNewCollectionIdent(const DatabaseName& dbName,
                                       bool directoryPerDB,
                                       bool directoryForIndexes,
                                       const boost::optional<std::string_view>& optIdentUniqueTag) {
    return generateNewIdent(
        dbName, kCollectionIdentStem, optIdentUniqueTag, directoryPerDB, directoryForIndexes);
}

std::string generateNewIndexIdent(const DatabaseName& dbName,
                                  bool directoryPerDB,
                                  bool directoryForIndexes,
                                  const boost::optional<std::string_view>& optIdentUniqueTag) {
    return generateNewIdent(
        dbName, kIndexIdentStem, optIdentUniqueTag, directoryPerDB, directoryForIndexes);
}

std::string generateNewInternalIdent(std::string_view identStem) {
    return fmt::format("{}-{}{}", kInternalIdentStem, identStem, UUID::gen().toString());
}

std::string generateNewInternalIndexBuildIdent(std::string_view identStem,
                                               std::string_view indexIdent) {
    auto parsed = parseIdent(indexIdent);
    massert(11570700,
            str::stream() << "Invalid ident supplied to generateNewInternalIndexBuildIdent: "
                          << indexIdent,
            parsed);
    massert(11570701,
            str::stream() << "Non-index ident supplied to generateNewInternalIndexBuildIdent: "
                          << indexIdent,
            parsed->identType == IdentType::index);
    StringBuilder buf;
    if (parsed->dbName) {
        buf << *parsed->dbName << '/';
    }
    buf << kInternalIdentStem << '-' << identStem << '-' << parsed->uniqueTag;
    return buf.str();
}

std::string generateNewIndexBuildIdent(const UUID& buildUUID) {
    return fmt::format("{}-{}-{}", kInternalIdentStem, kIndexBuildIdentStem, buildUUID.toString());
}

std::string_view getCollectionIdentUniqueTag(std::string_view ident,
                                             const DatabaseName& dbName,
                                             bool directoryPerDB,
                                             bool directoryForIndexes) {
    auto identPrefix =
        buildIdentPrefix(dbName, kCollectionIdentStem, directoryPerDB, directoryForIndexes);
    invariant(ident.starts_with(identPrefix.stringData()));
    return ident.substr(identPrefix.len());
}

std::string_view getIndexIdentUniqueTag(std::string_view ident,
                                        const DatabaseName& dbName,
                                        bool directoryPerDB,
                                        bool directoryForIndexes) {
    auto identPrefix =
        buildIdentPrefix(dbName, kIndexIdentStem, directoryPerDB, directoryForIndexes);
    invariant(ident.starts_with(identPrefix.stringData()));
    return ident.substr(identPrefix.len());
}

bool isCollectionOrIndexIdent(std::string_view ident) {
    auto parsed = parseIdent(ident);
    return parsed &&
        (parsed->identType == IdentType::collection || parsed->identType == IdentType::index);
}

bool isInternalIdent(std::string_view ident, std::string_view identStem) {
    auto parsed = parseIdent(ident);
    return parsed && parsed->identType == IdentType::internal &&
        parsed->uniqueTag.starts_with(identStem);
}

bool isReplicatedFastCountIdent(std::string_view ident) {
    return ident == kFastCountMetadataStore || ident == kFastCountMetadataStoreTimestamps;
}

bool isCollectionIdent(std::string_view ident) {
    // Internal idents prefixed "internal-" should not be considered collections, because
    // they are not eligible for orphan recovery through repair.
    auto parsed = parseIdent(ident);
    return parsed && parsed->identType == IdentType::collection;
}

bool isIndexIdent(std::string_view ident) {
    auto parsed = parseIdent(ident);
    return parsed && parsed->identType == IdentType::index;
}

bool validateTag(std::string_view uniqueTag) {
    return !uniqueTag.empty() && uniqueTag.find_first_of("./\\:") == uniqueTag.npos;
}

bool isValidIdent(std::string_view ident) {
    // These internal idents do not follow the normal scheme
    if (ident == kSizeStorer || ident == kMdbCatalog)
        return true;
    return parseIdent(ident).has_value();
}

std::string_view getDirectory(std::string_view ident) {
    uassert(11558900,
            str::stream() << "Invalid ident supplied to getDirectory: " << ident,
            isValidIdent(ident));
    auto pos = ident.rfind('/');
    if (pos == std::string_view::npos) {
        return ""sv;
    }
    return ident.substr(0, pos);
}

std::string createDBNamePathComponent(const DatabaseName& dbName) {
    std::string escaped;
    const auto db = DatabaseNameUtil::serialize(dbName, SerializationContext::stateCatalog());
    escaped.reserve(db.size());
    for (unsigned char c : db) {
        std::string_view ce = escapeTable[c];
        escaped.append(ce.begin(), ce.end());
    }
    return escaped;
}

}  // namespace ident
}  // namespace mongo
