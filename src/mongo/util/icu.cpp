// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/util/icu.h"

#include "mongo/base/error_codes.h"
#include "mongo/base/init.h"  // IWYU pragma: keep
#include "mongo/base/initializer.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/str.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string_view>
#include <vector>

#include <unicode/uchar.h>  // IWYU pragma: keep
#include <unicode/umachine.h>
#include <unicode/usprep.h>
#include <unicode/ustring.h>  // IWYU pragma: keep
#include <unicode/utf8.h>     // IWYU pragma: keep
#include <unicode/utypes.h>

namespace mongo {
using namespace std::literals::string_view_literals;
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

    static UString fromUTF8(std::string_view str) {
        if (str.empty()) {
            return UString(0);
        }
        UErrorCode error = U_ZERO_ERROR;
        int32_t len = 0;
        u_strFromUTF8(nullptr, 0, &len, str.data(), str.size(), &error);
        uassert(ErrorCodes::BadValue, "Non UTF-8 data encountered", error != U_INVALID_CHAR_FOUND);
        uassert(50687,
                str::stream() << "Error preflighting UTF-8 conversion: " << u_errorName(error),
                error == U_BUFFER_OVERFLOW_ERROR);

        error = U_ZERO_ERROR;
        UString ret(len);
        u_strFromUTF8(ret.data(), ret.capacity(), &len, str.data(), str.size(), &error);
        uassert(50688,
                str::stream() << "Error converting UTF-8 string: " << u_errorName(error),
                U_SUCCESS(error));
        ret.resize(len);
        return ret;
    }

    UString foldCase() const {
        if (_str.empty()) {
            return UString(0);
        }
        UErrorCode error = U_ZERO_ERROR;
        int32_t len =
            u_strFoldCase(nullptr, 0, _str.data(), _str.size(), U_FOLD_CASE_DEFAULT, &error);
        uassert(9553000,
                str::stream() << "Error preflighting Unicode case fold: " << u_errorName(error),
                error == U_BUFFER_OVERFLOW_ERROR);

        error = U_ZERO_ERROR;
        UString ret(len);
        u_strFoldCase(
            ret.data(), ret.capacity(), _str.data(), _str.size(), U_FOLD_CASE_DEFAULT, &error);
        uassert(9553001,
                str::stream() << "Error applying Unicode case fold: " << u_errorName(error),
                U_SUCCESS(error));
        ret.resize(len);
        return ret;
    }

    std::string toUTF8() const {
        if (_str.empty()) {
            return {};
        }
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

StatusWith<std::string> icuSaslPrep(std::string_view str, UStringPrepOptions options) try {
    const auto opts = (options == kUStringPrepDefault) ? USPREP_DEFAULT : USPREP_ALLOW_UNASSIGNED;
    return USPrep(USPREP_RFC4013_SASLPREP).prepare(UString::fromUTF8(str), opts).toUTF8();
} catch (const DBException& e) {
    return e.toStatus();
}

StatusWith<std::string> icuX509DNPrep(std::string_view str) try {
    return USPrep(USPREP_RFC4518_LDAP).prepare(UString::fromUTF8(str), USPREP_DEFAULT).toUTF8();
} catch (const DBException& e) {
    return e.toStatus();
}

/**
 * ICU has a subtle undefined behavior race condition in the USPrep cache code. While unlikely to
 * cause a problem, we can mitigate by causing the caches to be initialized at startup time.
 */
MONGO_INITIALIZER_GENERAL(LoadIcuPrep, ("LoadICUData"), ("default"))(InitializerContext*) {
    // Force ICU to load its caches by calling each function.
    invariant(icuSaslPrep("a"sv).getStatus());
    invariant(icuX509DNPrep("a"sv).getStatus());
}

std::string icuCaseFold(std::string_view str) {
    return UString::fromUTF8(str).foldCase().toUTF8();
}

}  // namespace mongo
