/**
 *    Copyright (C) 2024-present MongoDB, Inc.
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

#include "mongo/db/storage/ident.h"

#include "mongo/bson/util/builder.h"
#include "mongo/db/database_name.h"
#include "mongo/util/database_name_util.h"
#include "mongo/util/serialization_context.h"
#include "mongo/util/uuid.h"

namespace mongo {

namespace {
constexpr auto kCollectionIdentStem = "collection"_sd;
constexpr auto kIndexIdentStem = "index"_sd;
constexpr auto kInternalIdentStem = "internal"_sd;

// Does not escape letters, digits, '.', or '_'.
// Otherwise escapes to a '.' followed by a zero-filled 2- or 3-digit decimal number.
// Note that this escape table does not produce a 1:1 mapping to and from dbname, and
// collisions are possible.
// For example:
//     "db.123", "db\0143", and "db\073" all escape to "db.123".
//       {'d','b','1','2','3'} => "d" + "b" + "." + "1" + "2" + "3" => "db.123"
//       {'d','b','\x0c','3'}  => "d" + "b" + ".12" + "3"           => "db.123"
//       {'d','b','\x3b'}      => "d" + "b" + ".123"                => "db.123"
constexpr std::array<StringData, 256> escapeTable = {
    ".00"_sd,  ".01"_sd,  ".02"_sd,  ".03"_sd,  ".04"_sd,  ".05"_sd,  ".06"_sd,  ".07"_sd,
    ".08"_sd,  ".09"_sd,  ".10"_sd,  ".11"_sd,  ".12"_sd,  ".13"_sd,  ".14"_sd,  ".15"_sd,
    ".16"_sd,  ".17"_sd,  ".18"_sd,  ".19"_sd,  ".20"_sd,  ".21"_sd,  ".22"_sd,  ".23"_sd,
    ".24"_sd,  ".25"_sd,  ".26"_sd,  ".27"_sd,  ".28"_sd,  ".29"_sd,  ".30"_sd,  ".31"_sd,
    ".32"_sd,  ".33"_sd,  ".34"_sd,  ".35"_sd,  ".36"_sd,  ".37"_sd,  ".38"_sd,  ".39"_sd,
    ".40"_sd,  ".41"_sd,  ".42"_sd,  ".43"_sd,  ".44"_sd,  ".45"_sd,  "."_sd,    ".47"_sd,
    "0"_sd,    "1"_sd,    "2"_sd,    "3"_sd,    "4"_sd,    "5"_sd,    "6"_sd,    "7"_sd,
    "8"_sd,    "9"_sd,    ".58"_sd,  ".59"_sd,  ".60"_sd,  ".61"_sd,  ".62"_sd,  ".63"_sd,
    ".64"_sd,  "A"_sd,    "B"_sd,    "C"_sd,    "D"_sd,    "E"_sd,    "F"_sd,    "G"_sd,
    "H"_sd,    "I"_sd,    "J"_sd,    "K"_sd,    "L"_sd,    "M"_sd,    "N"_sd,    "O"_sd,
    "P"_sd,    "Q"_sd,    "R"_sd,    "S"_sd,    "T"_sd,    "U"_sd,    "V"_sd,    "W"_sd,
    "X"_sd,    "Y"_sd,    "Z"_sd,    ".91"_sd,  ".92"_sd,  ".93"_sd,  ".94"_sd,  "_"_sd,
    ".96"_sd,  "a"_sd,    "b"_sd,    "c"_sd,    "d"_sd,    "e"_sd,    "f"_sd,    "g"_sd,
    "h"_sd,    "i"_sd,    "j"_sd,    "k"_sd,    "l"_sd,    "m"_sd,    "n"_sd,    "o"_sd,
    "p"_sd,    "q"_sd,    "r"_sd,    "s"_sd,    "t"_sd,    "u"_sd,    "v"_sd,    "w"_sd,
    "x"_sd,    "y"_sd,    "z"_sd,    ".123"_sd, ".124"_sd, ".125"_sd, ".126"_sd, ".127"_sd,
    ".128"_sd, ".129"_sd, ".130"_sd, ".131"_sd, ".132"_sd, ".133"_sd, ".134"_sd, ".135"_sd,
    ".136"_sd, ".137"_sd, ".138"_sd, ".139"_sd, ".140"_sd, ".141"_sd, ".142"_sd, ".143"_sd,
    ".144"_sd, ".145"_sd, ".146"_sd, ".147"_sd, ".148"_sd, ".149"_sd, ".150"_sd, ".151"_sd,
    ".152"_sd, ".153"_sd, ".154"_sd, ".155"_sd, ".156"_sd, ".157"_sd, ".158"_sd, ".159"_sd,
    ".160"_sd, ".161"_sd, ".162"_sd, ".163"_sd, ".164"_sd, ".165"_sd, ".166"_sd, ".167"_sd,
    ".168"_sd, ".169"_sd, ".170"_sd, ".171"_sd, ".172"_sd, ".173"_sd, ".174"_sd, ".175"_sd,
    ".176"_sd, ".177"_sd, ".178"_sd, ".179"_sd, ".180"_sd, ".181"_sd, ".182"_sd, ".183"_sd,
    ".184"_sd, ".185"_sd, ".186"_sd, ".187"_sd, ".188"_sd, ".189"_sd, ".190"_sd, ".191"_sd,
    ".192"_sd, ".193"_sd, ".194"_sd, ".195"_sd, ".196"_sd, ".197"_sd, ".198"_sd, ".199"_sd,
    ".200"_sd, ".201"_sd, ".202"_sd, ".203"_sd, ".204"_sd, ".205"_sd, ".206"_sd, ".207"_sd,
    ".208"_sd, ".209"_sd, ".210"_sd, ".211"_sd, ".212"_sd, ".213"_sd, ".214"_sd, ".215"_sd,
    ".216"_sd, ".217"_sd, ".218"_sd, ".219"_sd, ".220"_sd, ".221"_sd, ".222"_sd, ".223"_sd,
    ".224"_sd, ".225"_sd, ".226"_sd, ".227"_sd, ".228"_sd, ".229"_sd, ".230"_sd, ".231"_sd,
    ".232"_sd, ".233"_sd, ".234"_sd, ".235"_sd, ".236"_sd, ".237"_sd, ".238"_sd, ".239"_sd,
    ".240"_sd, ".241"_sd, ".242"_sd, ".243"_sd, ".244"_sd, ".245"_sd, ".246"_sd, ".247"_sd,
    ".248"_sd, ".249"_sd, ".250"_sd, ".251"_sd, ".252"_sd, ".253"_sd, ".254"_sd, ".255"_sd};

std::string generateNewIdent(const DatabaseName& dbName,
                             StringData identType,
                             bool directoryPerDB,
                             bool directoryForIndexes) {
    StringBuilder buf;
    if (directoryPerDB) {
        buf << ident::createDBNamePathComponent(dbName) << '/';
    }
    buf << identType;
    buf << (directoryForIndexes ? '/' : '-');
    // The suffix of an ident serves as a unique identifier.
    //
    // (v8.2+) Suffix new idents with a unique UUID.
    //
    // Idents created before v8.2 are suffixed with a <counter> + <random number> combination.
    // Future versions of the server must support both formats.
    buf << UUID::gen();
    return buf.str();
}

enum class IdentType { collection, index, internal };

boost::optional<IdentType> getIdentType(StringData str) {
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
    StringData uniqueTag;
};

bool validateTag(StringData uniqueTag) {
    // The tag is pretty free-form, but must not contain any characters which would be special when
    // interpreted as a path
    return !uniqueTag.empty() && uniqueTag.find_first_of("./\\:") == uniqueTag.npos;
}

boost::optional<ParsedIdent> validateIdent(boost::optional<StringData> dbName,
                                           StringData identType,
                                           StringData uniqueTag) {
    // Ident type must be one of a fixed set of values
    auto parsedIdentType = getIdentType(identType);
    if (!parsedIdentType)
        return boost::none;
    if (!validateTag(uniqueTag))
        return boost::none;

    // If the dbName is present it must be non-empty, must not change when escaped with
    // createDBNamePathComponent(), and must not be "." or ".."
    if (dbName) {
        if (dbName->empty() || dbName == "."_sd || dbName == ".."_sd)
            return boost::none;
        for (char c : *dbName) {
            if (escapeTable[c].size() != 1)
                return boost::none;
        }
    }

    return ParsedIdent{*parsedIdentType, uniqueTag};
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
boost::optional<ParsedIdent> parseIdent(StringData str) {
    struct Parts {
        char delim;
        StringData head;
        StringData tail;
    };
    auto split = [](StringData in, StringData delims) -> boost::optional<Parts> {
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
    if (auto identType = getIdentType(tok2->head)) {
        // Format 3: "$dbName/$identType-$uniqueTag"
        if (!validateTag(tok2->tail))
            return boost::none;
        return ParsedIdent{*identType, tok2->tail};
    }
    if (auto identType = getIdentType(tok1->head)) {
        // Format 2: "$identType/$uniqueTag"
        if (!validateTag(tok1->tail))
            return boost::none;
        return ParsedIdent{*identType, tok1->tail};
    }
    return boost::none;
}
}  // namespace

namespace ident {

std::string generateNewCollectionIdent(const DatabaseName& dbName,
                                       bool directoryPerDB,
                                       bool directoryForIndexes) {
    return generateNewIdent(dbName, kCollectionIdentStem, directoryPerDB, directoryForIndexes);
}

std::string generateNewIndexIdent(const DatabaseName& dbName,
                                  bool directoryPerDB,
                                  bool directoryForIndexes) {
    return generateNewIdent(dbName, kIndexIdentStem, directoryPerDB, directoryForIndexes);
}

std::string generateNewInternalIdent(StringData identStem) {
    return fmt::format("{}-{}{}", kInternalIdentStem, identStem, UUID::gen().toString());
}

std::string generateNewInternalIndexBuildIdent(StringData identStem, StringData indexIdent) {
    return fmt::format("{}-{}-{}", kInternalIdentStem, identStem, indexIdent);
}

bool isCollectionOrIndexIdent(StringData ident) {
    auto parsed = parseIdent(ident);
    return parsed &&
        (parsed->identType == IdentType::collection || parsed->identType == IdentType::index);
}

bool isInternalIdent(StringData ident, StringData identStem) {
    auto parsed = parseIdent(ident);
    return parsed && parsed->identType == IdentType::internal &&
        parsed->uniqueTag.starts_with(identStem);
}

bool isCollectionIdent(StringData ident) {
    // Internal idents prefixed "internal-" should not be considered collections, because
    // they are not eligible for orphan recovery through repair.
    auto parsed = parseIdent(ident);
    return parsed && parsed->identType == IdentType::collection;
}

bool isValidIdent(StringData ident) {
    // These internal idents do not follow the normal scheme
    if (ident == kSizeStorer || ident == kMbdCatalog)
        return true;
    return parseIdent(ident).has_value();
}

std::string createDBNamePathComponent(const DatabaseName& dbName) {
    std::string escaped;
    const auto db = DatabaseNameUtil::serialize(dbName, SerializationContext::stateCatalog());
    escaped.reserve(db.size());
    for (unsigned char c : db) {
        StringData ce = escapeTable[c];
        escaped.append(ce.begin(), ce.end());
    }
    return escaped;
}

}  // namespace ident
}  // namespace mongo
