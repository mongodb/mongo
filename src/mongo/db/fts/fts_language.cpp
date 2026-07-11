// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/fts/fts_language.h"

#include "mongo/base/error_codes.h"
#include "mongo/db/fts/fts_basic_tokenizer.h"
#include "mongo/db/fts/fts_tokenizer.h"
#include "mongo/db/fts/fts_unicode_tokenizer.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/ctype.h"

#include <algorithm>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <string_view>
#include <type_traits>

#include <fmt/format.h>

namespace mongo::fts {

namespace {
using namespace std::literals::string_view_literals;

/**
 * Case-insensitive std::string_view comparator.
 * Returns true if a < b.
 */
struct LanguageStringCompare {
    bool operator()(std::string_view a, std::string_view b) const {
        return std::lexicographical_compare(
            a.begin(), a.end(), b.begin(), b.end(), [](char a, char b) {
                return ctype::toLower(a) < ctype::toLower(b);
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
    std::string_view name;   // - lower case string name
    std::string_view alias;  // - language alias (if nonempty)
} static constexpr kLanguagesV2V3[] = {
    {"none"sv, {}},
    {"danish"sv, "da"sv},
    {"dutch"sv, "nl"sv},
    {"english"sv, "en"sv},
    {"finnish"sv, "fi"sv},
    {"french"sv, "fr"sv},
    {"german"sv, "de"sv},
    {"hungarian"sv, "hu"sv},
    {"italian"sv, "it"sv},
    {"norwegian"sv, "nb"sv},
    {"portuguese"sv, "pt"sv},
    {"romanian"sv, "ro"sv},
    {"russian"sv, "ru"sv},
    {"spanish"sv, "es"sv},
    {"swedish"sv, "sv"sv},
    {"turkish"sv, "tr"sv},
};

//
// Register all Snowball language modules for TEXT_INDEX_VERSION_1.  Note that only the full
// names are recognized by the StopWords class (as such, the language string "dan" in
// TEXT_INDEX_VERSION_1 will generate the Danish stemmer and the empty stopword list).
//

struct {
    std::string_view name;
} static constexpr kLanguagesV1[] = {
    {"none"sv},      {"da"sv},      {"dan"sv},       {"danish"sv},  {"de"sv},      {"deu"sv},
    {"dut"sv},       {"dutch"sv},   {"en"sv},        {"eng"sv},     {"english"sv}, {"es"sv},
    {"esl"sv},       {"fi"sv},      {"fin"sv},       {"finnish"sv}, {"fr"sv},      {"fra"sv},
    {"fre"sv},       {"french"sv},  {"ger"sv},       {"german"sv},  {"hu"sv},      {"hun"sv},
    {"hungarian"sv}, {"it"sv},      {"ita"sv},       {"italian"sv}, {"nl"sv},      {"nld"sv},
    {"no"sv},        {"nor"sv},     {"norwegian"sv}, {"por"sv},     {"porter"sv},  {"portuguese"sv},
    {"pt"sv},        {"ro"sv},      {"romanian"sv},  {"ron"sv},     {"ru"sv},      {"rum"sv},
    {"rus"sv},       {"russian"sv}, {"spa"sv},       {"spanish"sv}, {"sv"sv},      {"swe"sv},
    {"swedish"sv},   {"tr"sv},      {"tur"sv},       {"turkish"sv},
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

    void add(std::string_view name, std::string_view alias = {}) {
        auto p = std::make_shared<const LanguageType>(std::string{name});
        _map[std::string{name}] = p;
        if (!alias.empty()) {
            _map[std::string{alias}] = p;
        }
    }

    const LanguageType& make(std::string_view langName) const {
        std::string nameStr{langName};
        auto it = _map.find(nameStr);
        if (it == _map.end()) {
            if constexpr (ver == TEXT_INDEX_VERSION_1) {
                // v1 treats unrecognized language strings as "none".
                return *_map.at("none");
            } else {
                // v2 and above reject unrecognized language strings.
                uasserted(ErrorCodes::BadValue,
                          fmt::format(R"(unsupported language: "{}" for text index version {})",
                                      langName,
                                      fmt::underlying(ver)));
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

const FTSLanguage& FTSLanguage::make(std::string_view langName, TextIndexVersion textIndexVersion) {
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
