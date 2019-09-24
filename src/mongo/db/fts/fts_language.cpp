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

#include "mongo/db/fts/fts_language.h"

#include <algorithm>
#include <cctype>
#include <fmt/format.h>
#include <map>
#include <memory>
#include <string>
#include <type_traits>
#include <utility>

#include "mongo/base/error_codes.h"
#include "mongo/base/string_data.h"
#include "mongo/db/fts/fts_basic_phrase_matcher.h"
#include "mongo/db/fts/fts_basic_tokenizer.h"
#include "mongo/db/fts/fts_unicode_phrase_matcher.h"
#include "mongo/db/fts/fts_unicode_tokenizer.h"
#include "mongo/util/assert_util.h"

namespace mongo::fts {

namespace {

using namespace fmt::literals;

/**
 * Case-insensitive StringData comparator.
 * Returns true if a < b.
 */
struct LanguageStringCompare {
    bool operator()(StringData a, StringData b) const {
        return std::lexicographical_compare(
            a.begin(), a.end(), b.begin(), b.end(), [](unsigned char a, unsigned char b) {
                return std::tolower(a) < std::tolower(b);
            });
    }
};

// FTS Language map. These languages are available with TEXT_INDEX_VERSION_2 and above.
//
// Parameters:
// - C++ unique identifier suffix
// - lower case string name
// - language alias
//
struct {
    StringData name;   // - lower case string name
    StringData alias;  // - language alias (if nonempty)
} static constexpr kLanguagesV2V3[] = {
    {"none"_sd, {}},
    {"danish"_sd, "da"_sd},
    {"dutch"_sd, "nl"_sd},
    {"english"_sd, "en"_sd},
    {"finnish"_sd, "fi"_sd},
    {"french"_sd, "fr"_sd},
    {"german"_sd, "de"_sd},
    {"hungarian"_sd, "hu"_sd},
    {"italian"_sd, "it"_sd},
    {"norwegian"_sd, "nb"_sd},
    {"portuguese"_sd, "pt"_sd},
    {"romanian"_sd, "ro"_sd},
    {"russian"_sd, "ru"_sd},
    {"spanish"_sd, "es"_sd},
    {"swedish"_sd, "sv"_sd},
    {"turkish"_sd, "tr"_sd},
};

//
// Register all Snowball language modules for TEXT_INDEX_VERSION_1.  Note that only the full
// names are recognized by the StopWords class (as such, the language string "dan" in
// TEXT_INDEX_VERSION_1 will generate the Danish stemmer and the empty stopword list).
//

struct {
    StringData name;
} static constexpr kLanguagesV1[] = {
    {"none"_sd},       {"da"_sd},      {"dan"_sd},       {"danish"_sd},   {"de"_sd},
    {"deu"_sd},        {"dut"_sd},     {"dutch"_sd},     {"en"_sd},       {"eng"_sd},
    {"english"_sd},    {"es"_sd},      {"esl"_sd},       {"fi"_sd},       {"fin"_sd},
    {"finnish"_sd},    {"fr"_sd},      {"fra"_sd},       {"fre"_sd},      {"french"_sd},
    {"ger"_sd},        {"german"_sd},  {"hu"_sd},        {"hun"_sd},      {"hungarian"_sd},
    {"it"_sd},         {"ita"_sd},     {"italian"_sd},   {"nl"_sd},       {"nld"_sd},
    {"no"_sd},         {"nor"_sd},     {"norwegian"_sd}, {"por"_sd},      {"porter"_sd},
    {"portuguese"_sd}, {"pt"_sd},      {"ro"_sd},        {"romanian"_sd}, {"ron"_sd},
    {"ru"_sd},         {"rum"_sd},     {"rus"_sd},       {"russian"_sd},  {"spa"_sd},
    {"spanish"_sd},    {"sv"_sd},      {"swe"_sd},       {"swedish"_sd},  {"tr"_sd},
    {"tur"_sd},        {"turkish"_sd},
};

template <TextIndexVersion ver>
class LanguageRegistry {
public:
    // For V3 and above, use UnicodeFTSLanguage.
    using LanguageType =
        std::conditional_t<(ver >= TEXT_INDEX_VERSION_3), UnicodeFTSLanguage, BasicFTSLanguage>;

    // For V2 and above, language names are case-insensitive.
    using KeyCompare =
        std::conditional_t<(ver >= TEXT_INDEX_VERSION_2), LanguageStringCompare, std::less<>>;

    void add(StringData name, StringData alias = {}) {
        auto p = std::make_shared<const LanguageType>(std::string{name});
        _map[name.toString()] = p;
        if (!alias.empty()) {
            _map[alias.toString()] = p;
        }
    }

    const LanguageType& make(StringData langName) const {
        std::string nameStr{langName};
        auto it = _map.find(nameStr);
        if (it == _map.end()) {
            if constexpr (ver == TEXT_INDEX_VERSION_1) {
                // v1 treats unrecognized language strings as "none".
                return *_map.at("none");
            } else {
                // v2 and above reject unrecognized language strings.
                uasserted(ErrorCodes::BadValue,
                          R"(unsupported language: "{}" for text index version {})"_format(langName,
                                                                                           ver));
            }
        }
        return *it->second;
    }

private:
    std::map<std::string, std::shared_ptr<const LanguageType>, KeyCompare> _map;
};

// template <TextIndexVersion ver>
// LanguageRegistry<ver> languageRegistry;

template <TextIndexVersion ver>
const LanguageRegistry<ver>& getLanguageRegistry() {
    static const auto instance = [] {
        auto registry = new LanguageRegistry<ver>;
        if constexpr (ver == TEXT_INDEX_VERSION_1) {
            for (auto&& spec : kLanguagesV1) {
                registry->add(spec.name);
            }
        } else if constexpr (ver == TEXT_INDEX_VERSION_2 || ver == TEXT_INDEX_VERSION_3) {
            for (auto&& spec : kLanguagesV2V3) {
                registry->add(spec.name, spec.alias);
            }
        }
        return registry;
    }();
    return *instance;
}

}  // namespace

const FTSLanguage& FTSLanguage::make(StringData langName, TextIndexVersion textIndexVersion) {
    switch (textIndexVersion) {
        case TEXT_INDEX_VERSION_1:
            return getLanguageRegistry<TEXT_INDEX_VERSION_1>().make(langName);
        case TEXT_INDEX_VERSION_2:
            return getLanguageRegistry<TEXT_INDEX_VERSION_2>().make(langName);
        case TEXT_INDEX_VERSION_3:
            return getLanguageRegistry<TEXT_INDEX_VERSION_3>().make(langName);
        case TEXT_INDEX_VERSION_INVALID:
            break;
    }
    uasserted(ErrorCodes::BadValue, "invalid TextIndexVersion");
}

std::unique_ptr<FTSTokenizer> BasicFTSLanguage::createTokenizer() const {
    return std::make_unique<BasicFTSTokenizer>(this);
}

std::unique_ptr<FTSTokenizer> UnicodeFTSLanguage::createTokenizer() const {
    return std::make_unique<UnicodeFTSTokenizer>(this);
}

}  // namespace mongo::fts
