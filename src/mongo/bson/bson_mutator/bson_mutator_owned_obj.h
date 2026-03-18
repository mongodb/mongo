/**
 *    Copyright (C) 2025-present MongoDB, Inc.
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

#pragma once

#include "mongo/bson/bsontypes_util.h"

#include <string>

namespace mongo::bson_mutator {

/**
 * copies of BSONElements which own their data members. For use with mutation engine.
 */

struct BSONRegExOwned : public BSONRegEx {
    BSONRegExOwned(std::string pat, std::string f) : patternOwned(pat), flagsOwned(f) {
        pattern = StringData(patternOwned);
        flags = StringData(flagsOwned);
    };

    BSONRegExOwned(const BSONRegExOwned&) = delete;
    BSONRegExOwned& operator=(const BSONRegExOwned&) = delete;
    BSONRegExOwned(BSONRegExOwned&& obj) noexcept
        : patternOwned(std::exchange(obj.patternOwned, {})),
          flagsOwned(std::exchange(obj.flagsOwned, {})) {
        pattern = StringData(patternOwned);
        flags = StringData(flagsOwned);
    }

    BSONRegExOwned& operator=(BSONRegExOwned&& obj) noexcept {
        if (this != &obj) {
            patternOwned = std::exchange(obj.patternOwned, {});
            pattern = StringData(patternOwned);
            flagsOwned = std::exchange(obj.flagsOwned, {});
            flags = StringData(flagsOwned);
        }
        return *this;
    }

    std::string patternOwned;
    std::string flagsOwned;
};

struct BSONCodeOwned : public BSONCode {
    explicit BSONCodeOwned(std::string str) : codeOwned(std::exchange(str, {})) {
        code = StringData(codeOwned);
    }

    BSONCodeOwned(const BSONCodeOwned&) = delete;
    BSONCodeOwned& operator=(const BSONCodeOwned&) = delete;
    BSONCodeOwned(BSONCodeOwned&& obj) noexcept : codeOwned(std::exchange(obj.codeOwned, {})) {
        code = StringData(codeOwned);
    }

    BSONCodeOwned& operator=(BSONCodeOwned&& obj) noexcept {
        if (this != &obj) {
            codeOwned = std::exchange(obj.codeOwned, {});
            code = StringData(codeOwned);
        }
        return *this;
    }

    std::string codeOwned;
};

struct BSONSymbolOwned : public BSONSymbol {
    explicit BSONSymbolOwned(std::string sym) : symbolOwned(std::exchange(sym, {})) {
        symbol = StringData(symbolOwned);
    };

    BSONSymbolOwned(const BSONSymbolOwned&) = delete;
    BSONSymbolOwned& operator=(const BSONSymbolOwned&) = delete;
    BSONSymbolOwned(BSONSymbolOwned&& obj) noexcept
        : symbolOwned(std::exchange(obj.symbolOwned, {})) {
        symbol = StringData(symbolOwned);
    }

    BSONSymbolOwned& operator=(BSONSymbolOwned&& obj) noexcept {
        if (this != &obj) {
            symbolOwned = std::exchange(obj.symbolOwned, {});
            symbol = StringData(symbolOwned);
        }
        return *this;
    }

    std::string symbolOwned;
};

struct BSONCodeWScopeOwned : public BSONCodeWScope {
    BSONCodeWScopeOwned(std::string str, const BSONObj& obj) : codeOwned(std::exchange(str, {})) {
        code = StringData(codeOwned);
        scope = obj.getOwned();
    };

    BSONCodeWScopeOwned(const BSONCodeWScopeOwned&) = delete;
    BSONCodeWScopeOwned& operator=(const BSONCodeWScopeOwned&) = delete;
    BSONCodeWScopeOwned(BSONCodeWScopeOwned&& obj) noexcept
        : codeOwned(std::exchange(obj.codeOwned, {})) {
        code = StringData(obj.codeOwned);
        scope = obj.scope.getOwned();
    }

    BSONCodeWScopeOwned& operator=(BSONCodeWScopeOwned&& obj) noexcept {
        if (this != &obj) {
            codeOwned = std::exchange(obj.codeOwned, {});
            code = StringData(obj.codeOwned);
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
        ns = StringData(nsOwned);
        oid = o;
    };

    BSONDBRefOwned(const BSONDBRefOwned&) = delete;
    BSONDBRefOwned& operator=(const BSONDBRefOwned&) = delete;
    BSONDBRefOwned(BSONDBRefOwned&& obj) noexcept : nsOwned(std::exchange(obj.nsOwned, {})) {
        ns = StringData(nsOwned);
        oid = obj.oid;
    }

    BSONDBRefOwned& operator=(BSONDBRefOwned&& obj) noexcept {
        if (this != &obj) {
            nsOwned = std::exchange(obj.nsOwned, {});
            ns = StringData(nsOwned);
            oid = obj.oid;
        }
        return *this;
    }

    std::string nsOwned;
};

}  // namespace mongo::bson_mutator
