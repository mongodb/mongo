// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/bson/bsontypes_util.h"

#include <string>
#include <string_view>

namespace mongo::bson_mutator {

/**
 * copies of BSONElements which own their data members. For use with mutation engine.
 */

struct BSONRegExOwned : public BSONRegEx {
    BSONRegExOwned(std::string pat, std::string f) : patternOwned(pat), flagsOwned(f) {
        pattern = std::string_view(patternOwned);
        flags = std::string_view(flagsOwned);
    };

    BSONRegExOwned(const BSONRegExOwned&) = delete;
    BSONRegExOwned& operator=(const BSONRegExOwned&) = delete;
    BSONRegExOwned(BSONRegExOwned&& obj) noexcept
        : patternOwned(std::exchange(obj.patternOwned, {})),
          flagsOwned(std::exchange(obj.flagsOwned, {})) {
        pattern = std::string_view(patternOwned);
        flags = std::string_view(flagsOwned);
    }

    BSONRegExOwned& operator=(BSONRegExOwned&& obj) noexcept {
        if (this != &obj) {
            patternOwned = std::exchange(obj.patternOwned, {});
            pattern = std::string_view(patternOwned);
            flagsOwned = std::exchange(obj.flagsOwned, {});
            flags = std::string_view(flagsOwned);
        }
        return *this;
    }

    std::string patternOwned;
    std::string flagsOwned;
};

struct BSONCodeOwned : public BSONCode {
    explicit BSONCodeOwned(std::string str) : codeOwned(std::exchange(str, {})) {
        code = std::string_view(codeOwned);
    }

    BSONCodeOwned(const BSONCodeOwned&) = delete;
    BSONCodeOwned& operator=(const BSONCodeOwned&) = delete;
    BSONCodeOwned(BSONCodeOwned&& obj) noexcept : codeOwned(std::exchange(obj.codeOwned, {})) {
        code = std::string_view(codeOwned);
    }

    BSONCodeOwned& operator=(BSONCodeOwned&& obj) noexcept {
        if (this != &obj) {
            codeOwned = std::exchange(obj.codeOwned, {});
            code = std::string_view(codeOwned);
        }
        return *this;
    }

    std::string codeOwned;
};

struct BSONSymbolOwned : public BSONSymbol {
    explicit BSONSymbolOwned(std::string sym) : symbolOwned(std::exchange(sym, {})) {
        symbol = std::string_view(symbolOwned);
    };

    BSONSymbolOwned(const BSONSymbolOwned&) = delete;
    BSONSymbolOwned& operator=(const BSONSymbolOwned&) = delete;
    BSONSymbolOwned(BSONSymbolOwned&& obj) noexcept
        : symbolOwned(std::exchange(obj.symbolOwned, {})) {
        symbol = std::string_view(symbolOwned);
    }

    BSONSymbolOwned& operator=(BSONSymbolOwned&& obj) noexcept {
        if (this != &obj) {
            symbolOwned = std::exchange(obj.symbolOwned, {});
            symbol = std::string_view(symbolOwned);
        }
        return *this;
    }

    std::string symbolOwned;
};

struct BSONCodeWScopeOwned : public BSONCodeWScope {
    BSONCodeWScopeOwned(std::string str, const BSONObj& obj) : codeOwned(std::exchange(str, {})) {
        code = std::string_view(codeOwned);
        scope = obj.getOwned();
    };

    BSONCodeWScopeOwned(const BSONCodeWScopeOwned&) = delete;
    BSONCodeWScopeOwned& operator=(const BSONCodeWScopeOwned&) = delete;
    BSONCodeWScopeOwned(BSONCodeWScopeOwned&& obj) noexcept
        : codeOwned(std::exchange(obj.codeOwned, {})) {
        code = std::string_view(obj.codeOwned);
        scope = obj.scope.getOwned();
    }

    BSONCodeWScopeOwned& operator=(BSONCodeWScopeOwned&& obj) noexcept {
        if (this != &obj) {
            codeOwned = std::exchange(obj.codeOwned, {});
            code = std::string_view(obj.codeOwned);
            scope = obj.scope.getOwned();
        }
        return *this;
    }

    std::string codeOwned;
};

struct BSONBinDataOwned : public BSONBinData {
    BSONBinDataOwned(std::vector<uint8_t> vec, BinDataType subtype)
        : dataOwned(std::exchange(vec, {})) {
        data = dataOwned.data();
        length = dataOwned.size();
        type = subtype;
    }

    explicit BSONBinDataOwned(const BSONBinData& d) noexcept {
        dataOwned = std::vector<uint8_t>(reinterpret_cast<const uint8_t*>(d.data),
                                         reinterpret_cast<const uint8_t*>(d.data) + d.length);
        data = dataOwned.data();
        length = dataOwned.size();
        type = d.type;
    }


    BSONBinDataOwned(const BSONBinDataOwned&) = delete;
    BSONBinDataOwned& operator=(const BSONBinDataOwned&) = delete;
    BSONBinDataOwned(BSONBinDataOwned&& obj) noexcept
        : dataOwned(std::exchange(obj.dataOwned, {})) {
        data = dataOwned.data();
        length = dataOwned.size();
        type = obj.type;
    }

    BSONBinDataOwned& operator=(BSONBinDataOwned&& obj) noexcept {
        if (this != &obj) {
            dataOwned = std::exchange(obj.dataOwned, {});
            data = dataOwned.data();
            length = dataOwned.size();
            type = obj.type;
        }
        return *this;
    }

    std::vector<uint8_t> dataOwned;
};

struct BSONDBRefOwned : public BSONDBRef {
    BSONDBRefOwned(std::string nameSpace, OID o) : nsOwned(std::exchange(nameSpace, {})) {
        ns = std::string_view(nsOwned);
        oid = o;
    };

    BSONDBRefOwned(const BSONDBRefOwned&) = delete;
    BSONDBRefOwned& operator=(const BSONDBRefOwned&) = delete;
    BSONDBRefOwned(BSONDBRefOwned&& obj) noexcept : nsOwned(std::exchange(obj.nsOwned, {})) {
        ns = std::string_view(nsOwned);
        oid = obj.oid;
    }

    BSONDBRefOwned& operator=(BSONDBRefOwned&& obj) noexcept {
        if (this != &obj) {
            nsOwned = std::exchange(obj.nsOwned, {});
            ns = std::string_view(nsOwned);
            oid = obj.oid;
        }
        return *this;
    }

    std::string nsOwned;
};

}  // namespace mongo::bson_mutator
