
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

#include "mongo/util/icu.h"

#include <memory>
#include <unicode/localpointer.h>
#include <unicode/putil.h>
#include <unicode/uiter.h>
#include <unicode/unistr.h>
#include <unicode/usprep.h>
#include <unicode/ustring.h>
#include <unicode/utypes.h>
#include <vector>

#include "mongo/util/assert_util.h"
#include "mongo/util/mongoutils/str.h"

namespace mongo {
namespace {

/**
 * Convenience wrapper for a UChar[] string.
 * Instantiate with UString::fromUTF8() and reseriealize with ustr.toUTF8()
 */
class UString {
public:
    UString() = delete;
    explicit UString(size_t size) {
        _str.resize(size);
    }

    const UChar* uc_str() const {
        return _str.data();
    }
    UChar* data() {
        return _str.data();
    }
    size_t capacity() const {
        return _str.capacity();
    }
    size_t size() const {
        return _str.size();
    }
    void resize(size_t len) {
        _str.resize(len);
    }

    static UString fromUTF8(StringData str) {
        UErrorCode error = U_ZERO_ERROR;
        int32_t len = 0;
        u_strFromUTF8(nullptr, 0, &len, str.rawData(), str.size(), &error);
        uassert(ErrorCodes::BadValue, "Non UTF-8 data encountered", error != U_INVALID_CHAR_FOUND);
        uassert(50687,
                str::stream() << "Error preflighting UTF-8 conversion: " << u_errorName(error),
                error == U_BUFFER_OVERFLOW_ERROR);

        error = U_ZERO_ERROR;
        UString ret(len);
        u_strFromUTF8(ret.data(), ret.capacity(), &len, str.rawData(), str.size(), &error);
        uassert(50688,
                str::stream() << "Error converting UTF-8 string: " << u_errorName(error),
                U_SUCCESS(error));
        ret.resize(len);
        return ret;
    }

    std::string toUTF8() const {
        UErrorCode error = U_ZERO_ERROR;
        int32_t len = 0;
        u_strToUTF8(nullptr, 0, &len, _str.data(), _str.size(), &error);
        uassert(50689,
                str::stream() << "Error preflighting UTF-8 conversion: " << u_errorName(error),
                error == U_BUFFER_OVERFLOW_ERROR);

        error = U_ZERO_ERROR;
        std::string ret;
        ret.resize(len);
        u_strToUTF8(&ret[0], ret.capacity(), &len, _str.data(), _str.size(), &error);
        uassert(50690,
                str::stream() << "Error converting string to UTF-8: " << u_errorName(error),
                U_SUCCESS(error));
        ret.resize(len);
        return ret;
    }

private:
    std::vector<UChar> _str;
};

/**
 * Convenience wrapper for ICU unicode string prep API.
 */
class USPrep {
public:
    USPrep() = delete;
    USPrep(UStringPrepProfileType type) {
        UErrorCode error = U_ZERO_ERROR;
        _profile.reset(usprep_openByType(type, &error));
        uassert(50691,
                str::stream() << "Unable to open unicode string prep profile: "
                              << u_errorName(error),
                U_SUCCESS(error));
    }

    UString prepare(const UString& src, int32_t options = USPREP_DEFAULT) {
        UErrorCode error = U_ZERO_ERROR;
        auto len = usprep_prepare(
            _profile.get(), src.uc_str(), src.size(), nullptr, 0, options, nullptr, &error);
        uassert(ErrorCodes::BadValue,
                "Unable to normalize input string",
                error != U_INVALID_CHAR_FOUND);
        uassert(50692,
                str::stream() << "Error preflighting normalization: " << u_errorName(error),
                error == U_BUFFER_OVERFLOW_ERROR);

        error = U_ZERO_ERROR;
        UString ret(len);
        len = usprep_prepare(_profile.get(),
                             src.uc_str(),
                             src.size(),
                             ret.data(),
                             ret.capacity(),
                             options,
                             nullptr,
                             &error);
        uassert(50693,
                str::stream() << "Failed normalizing string: " << u_errorName(error),
                U_SUCCESS(error));
        ret.resize(len);
        return ret;
    }

private:
    class USPrepDeleter {
    public:
        void operator()(UStringPrepProfile* profile) {
            usprep_close(profile);
        }
    };

    std::unique_ptr<UStringPrepProfile, USPrepDeleter> _profile;
};

}  // namespace

StatusWith<std::string> icuSaslPrep(StringData str, UStringPrepOptions options) try {
    const auto opts = (options == kUStringPrepDefault) ? USPREP_DEFAULT : USPREP_ALLOW_UNASSIGNED;
    return USPrep(USPREP_RFC4013_SASLPREP).prepare(UString::fromUTF8(str), opts).toUTF8();
} catch (const DBException& e) {
    return e.toStatus();
}

StatusWith<std::string> icuX509DNPrep(StringData str) try {
    return USPrep(USPREP_RFC4518_LDAP).prepare(UString::fromUTF8(str), USPREP_DEFAULT).toUTF8();
} catch (const DBException& e) {
    return e.toStatus();
}

}  // namespace mongo
