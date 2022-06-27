/**
 *    Copyright (C) 2022-present MongoDB, Inc.
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

#include "mongo/util/pcre.h"

#include <fmt/format.h>

#define PCRE2_CODE_UNIT_WIDTH 8  // Select 8-bit PCRE2 library.
#include <pcre2.h>

#include "mongo/base/error_codes.h"
#include "mongo/logv2/log.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/errno_util.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kDefault

namespace mongo::pcre {
namespace {

using namespace fmt::literals;
using namespace std::string_literals;

std::string pcre2ErrorMessage(int e) {
    char buf[120];
    int len = pcre2_get_error_message(e, reinterpret_cast<PCRE2_UCHAR*>(buf), sizeof(buf));
    if (len < 0) {
        return "Failed to get PCRE2 error message for code {}: {}"_format(e, [metaError = len] {
            switch (metaError) {
                case PCRE2_ERROR_NOMEMORY:
                    return "NOMEMORY"s;
                case PCRE2_ERROR_BADDATA:
                    return "BADDATA"s;
                default:
                    return "code={}"_format(metaError);
            }
        }());
    }
    return std::string(buf, len);
}

#define X_(name) std::pair{Errc::name, PCRE2_##name},
constexpr std::array errTable{MONGO_PCRE_ERROR_EXPAND_TABLE_(X_)};
#undef X_

Errc toErrc(int e) {
    if (e == 0)
        return Errc::OK;
    auto it =
        std::find_if(errTable.begin(), errTable.end(), [&](auto&& p) { return e == p.second; });
    iassert(ErrorCodes::BadValue, "Unknown pcre2 error {}"_format(e), it != errTable.end());
    return it->first;
}

int fromErrc(Errc e) {
    if (e == Errc::OK)
        return 0;
    auto it =
        std::find_if(errTable.begin(), errTable.end(), [&](auto&& p) { return e == p.first; });
    iassert(ErrorCodes::BadValue,
            "Unknown pcre::Errc {}"_format(static_cast<int>(e)),
            it != errTable.end());
    return it->second;
}

}  // namespace

inline namespace options {
#define X_(name) const CompileOptions name{PCRE2_##name};
MONGO_PCRE_OPTION_EXPAND_TABLE_COMPILE_(X_)
#undef X_

#define X_(name) const MatchOptions name{PCRE2_##name};
MONGO_PCRE_OPTION_EXPAND_TABLE_MATCH_(X_)
#undef X_

#define X_(name) const CompileAndMatchOptions name{PCRE2_##name};
MONGO_PCRE_OPTION_EXPAND_TABLE_COMPILE_AND_MATCH_(X_)
#undef X_
}  // namespace options

const std::error_category& pcreCategory() noexcept {
    class PcreCategory : public std::error_category {
    public:
        const char* name() const noexcept override {
            return "pcre2";
        }
        std::string message(int e) const override {
            return pcre2ErrorMessage(fromErrc(Errc{e}));
        }
    };
    static StaticImmortal<PcreCategory> singleton{};
    return *singleton;
}

namespace detail {

class MatchDataImpl;

// Global.
inline constexpr size_t kMaxPatternLength = 16384;

/** Wrapper around a pcre2_compile_context. */
class CompileContext {
public:
    CompileContext() {
        invariant(_ptr);
    }

    std::error_code setMaxPatternLength(size_t sz) {
        invariant(_ptr);
        if (int err = pcre2_set_max_pattern_length(_ptr.get(), sz))
            return toErrc(err);
        return {};
    }

    pcre2_compile_context* get() const {
        return _ptr.get();
    }

private:
    struct D {
        void operator()(pcre2_compile_context* p) const {
            pcre2_compile_context_free(p);
        }
    };
    std::unique_ptr<pcre2_compile_context, D> _ptr{pcre2_compile_context_create(nullptr)};
};

/** Members implement Regex interface and are documented there. */
class RegexImpl {
public:
    RegexImpl(std::string pattern, CompileOptions options) : _pattern{std::move(pattern)} {
        int err = 0;
        CompileContext compileContext;
        if (auto ec = compileContext.setMaxPatternLength(kMaxPatternLength)) {
            _error = ec;
            return;
        }
        _code = pcre2_compile((const unsigned char*)_pattern.data(),
                              _pattern.size(),
                              static_cast<uint32_t>(options),
                              &err,
                              &_errorPos,
                              compileContext.get());
        if (!_code)
            _error = toErrc(err);
    }
    ~RegexImpl() = default;
    RegexImpl(const RegexImpl&) = default;
    RegexImpl& operator=(const RegexImpl&) = default;
    RegexImpl(RegexImpl&&) = default;
    RegexImpl& operator=(RegexImpl&&) = default;

    explicit operator bool() const {
        return !_error;
    }

    std::error_code error() const {
        return _error;
    }

    size_t errorPosition() const {
        return _errorPos;
    }

    const std::string& pattern() const {
        return _pattern;
    }

    CompileOptions options() const {
        uint32_t n = 0;
        if (*this) {
            int e = pcre2_pattern_info(&*_code, PCRE2_INFO_ARGOPTIONS, &n);
            iassert(6527603, errorMessage(toErrc(e)), !e);
        }
        return CompileOptions{n};
    }

    size_t captureCount() const {
        uint32_t n = 0;
        if (*this) {
            int e = pcre2_pattern_info(&*_code, PCRE2_INFO_CAPTURECOUNT, &n);
            iassert(6527604, errorMessage(toErrc(e)), !e);
        }
        return n;
    }

    size_t codeSize() const {
        size_t tot = sizeof(*this);
        if (*this) {
            size_t patSz;
            if (!pcre2_pattern_info(&*_code, PCRE2_INFO_SIZE, &patSz))
                tot += patSz;
        }
        return tot;
    }

    MatchData match(std::string input, MatchOptions options, size_t startPos) const;
    MatchData matchView(StringData input, MatchOptions options, size_t startPos) const;

    int substitute(StringData replacement,
                   std::string* str,
                   MatchOptions options,
                   size_t startPos) const {
        std::string buf;
        buf.resize((str->size() + 16) * 2);
        bool probing = true;
        int subs;
        while (true) {
            MatchOptions trialOptions = options;
            if (probing)
                trialOptions |= SUBSTITUTE_OVERFLOW_LENGTH;
            size_t bufSize = buf.size();
            subs = pcre2_substitute(&*_code,
                                    (PCRE2_SPTR)str->c_str(),
                                    str->size(),
                                    startPos,
                                    static_cast<uint32_t>(trialOptions),
                                    (pcre2_match_data*)nullptr,
                                    (pcre2_match_context*)nullptr,
                                    (PCRE2_SPTR)replacement.rawData(),
                                    replacement.size(),
                                    (PCRE2_UCHAR*)buf.data(),
                                    &bufSize);
            if (subs < 0) {
                if (probing && subs == PCRE2_ERROR_NOMEMORY) {
                    probing = false;
                    buf.resize(bufSize + 1);
                    continue;
                }
                iasserted(ErrorCodes::UnknownError,
                          "substitute: {}"_format(errorMessage(toErrc(subs))));
            }
            buf.resize(bufSize);
            break;
        }
        *str = std::move(buf);
        return subs;
    }

    pcre2_code* code() const {
        return _code;
    };

private:
    class CodeHandle {
    public:
        CodeHandle() = default;
        CodeHandle(pcre2_code* code) : _p{code} {}
        CodeHandle(const CodeHandle& o) : _p{pcre2_code_copy(o._p)} {}
        CodeHandle& operator=(const CodeHandle& o) {
            if (this != &o)
                *this = CodeHandle{o};  // move-assign a forced copy
            return *this;
        }
        CodeHandle(CodeHandle&& o) noexcept : _p{std::exchange(o._p, {})} {}
        CodeHandle& operator=(CodeHandle&& o) noexcept {
            using std::swap;
            swap(_p, o._p);
            return *this;
        }

        operator pcre2_code*() const {
            return _p;
        }

        ~CodeHandle() {
            pcre2_code_free(_p);
        }

    private:
        pcre2_code* _p = nullptr;
    };

    MatchData _doMatch(std::unique_ptr<MatchDataImpl> m,
                       MatchOptions options,
                       size_t startPos) const;

    std::string _pattern;
    CodeHandle _code;
    std::error_code _error;
    size_t _errorPos;
};

/** Members implement MatchData interface and are documented there. */
class MatchDataImpl {
public:
    explicit MatchDataImpl(const RegexImpl* regex) : _regex{regex} {}

    explicit operator bool() const {
        return !_error;
    }

    size_t captureCount() const {
        return _regex->captureCount();
    }

    StringData operator[](size_t i) const {
        invariant(_data);
        // Using direct offset vector access. It's pairs of size_t offsets.
        // Captures can be unpopulated, represented by PCRE2_UNSET elements.
        size_t* p = pcre2_get_ovector_pointer(&*_data);
        size_t n = pcre2_get_ovector_count(&*_data);
        if (!(i < n))
            iasserted(ErrorCodes::NoSuchKey, "Access element {} of {}"_format(i, n));
        size_t b = p[2 * i + 0];
        size_t e = p[2 * i + 1];
        if (b == PCRE2_UNSET)
            return {};
        return StringData(_input.substr(b, e - b));
    }

    StringData operator[](const std::string& name) const {
        invariant(*_regex);
        int rc = pcre2_substring_number_from_name(_regex->code(), (PCRE2_SPTR)name.c_str());
        if (rc < 0) {
            iasserted(ErrorCodes::NoSuchKey,
                      "MatchData[{}]: {}"_format(name, errorMessage(toErrc(rc))));
        }
        return (*this)[rc];
    }

    std::vector<StringData> getMatchList() const {
        std::vector<StringData> vec;
        if (*_regex) {
            for (size_t i = 0; i <= captureCount(); ++i)
                vec.push_back((*this)[i]);
        }
        return vec;
    }

    std::vector<StringData> getCaptures() const {
        std::vector<StringData> vec;
        if (*_regex) {
            for (size_t i = 1; i <= captureCount(); ++i)
                vec.push_back((*this)[i]);
        }
        return vec;
    }

    std::error_code error() const {
        return _error;
    }

    StringData input() const {
        return _input;
    }

    size_t startPos() const {
        return _startPos;
    }

    void setInput(std::string s) {
        _input = _inputStorage = std::move(s);
    }

    void setInputView(StringData s) {
        _input = s;
    }

    pcre2_match_data* matchData() const {
        return _data.get();
    }

    void doMatch(MatchOptions options, size_t startPos) {
        _startPos = startPos;
        _data.reset(pcre2_match_data_create_from_pattern(_regex->code(), nullptr));
        int matched = pcre2_match(_regex->code(),
                                  (PCRE2_SPTR)_input.rawData(),
                                  _input.size(),
                                  startPos,
                                  static_cast<uint32_t>(options),
                                  _data.get(),
                                  nullptr);
        if (matched < 0)
            _error = toErrc(matched);
        _highestCaptureIndex = matched;
    }

private:
    struct FreeMatchData {
        void operator()(pcre2_match_data* md) const {
            pcre2_match_data_free(md);
        }
    };

    const RegexImpl* _regex;
    std::error_code _error;
    size_t _highestCaptureIndex = size_t(-1);
    std::string _inputStorage;
    StringData _input;
    size_t _startPos = 0;
    std::unique_ptr<pcre2_match_data, FreeMatchData> _data;
};

MatchData RegexImpl::match(std::string input, MatchOptions options, size_t startPos) const {
    auto m = std::make_unique<MatchDataImpl>(this);
    m->setInput(std::move(input));
    return _doMatch(std::move(m), options, startPos);
}

MatchData RegexImpl::matchView(StringData input, MatchOptions options, size_t startPos) const {
    auto m = std::make_unique<MatchDataImpl>(this);
    m->setInputView(input);
    return _doMatch(std::move(m), options, startPos);
}

MatchData RegexImpl::_doMatch(std::unique_ptr<MatchDataImpl> m,
                              MatchOptions options,
                              size_t startPos) const {
    if (*this)
        m->doMatch(options, startPos);
    return MatchData{std::move(m)};
}

}  // namespace detail

Regex::Regex(std::string pattern, CompileOptions options)
    : _impl{std::make_unique<detail::RegexImpl>(std::move(pattern), options)} {}

Regex::~Regex() = default;

Regex::Regex(const Regex& that)
    : _impl{that._impl ? std::make_unique<detail::RegexImpl>(*that._impl) : nullptr} {}

Regex& Regex::operator=(const Regex& that) {
    if (this != &that)
        *this = Regex{that};  // move-assign of forced copy
    return *this;
}

Regex::Regex(Regex&&) noexcept = default;
Regex& Regex::operator=(Regex&&) noexcept = default;

MatchData::MatchData(std::unique_ptr<detail::MatchDataImpl> impl) : _impl{std::move(impl)} {}
MatchData::~MatchData() = default;
MatchData::MatchData(MatchData&&) noexcept = default;
MatchData& MatchData::operator=(MatchData&&) noexcept = default;


#define UNPAREN_ARGS_(...) __VA_ARGS__
#define UNPAREN_STRIP_(x) x
#define UNPAREN(x) UNPAREN_STRIP_(UNPAREN_ARGS_ x)

// Define pimpl-forwarding const member functions.
#define IFWD(Class, MemberFunc, Ret, Args, FwdArgs) \
    UNPAREN(Ret) Class::MemberFunc Args const {     \
        invariant(_impl, "Use after move");         \
        return _impl->MemberFunc FwdArgs;           \
    }

IFWD(Regex, pattern, (const std::string&), (), ())
IFWD(Regex, options, (CompileOptions), (), ())
IFWD(Regex, operator bool,(), (), ())
IFWD(Regex, error, (std::error_code), (), ())
IFWD(Regex, errorPosition, (size_t), (), ())
IFWD(Regex, captureCount, (size_t), (), ())
IFWD(Regex, codeSize, (size_t), (), ())
IFWD(Regex, match, (MatchData), (std::string in, MatchOptions opt, size_t p), (in, opt, p))
IFWD(Regex, matchView, (MatchData), (StringData in, MatchOptions opt, size_t p), (in, opt, p))
IFWD(Regex,
     substitute,
     (int),
     (StringData r, std::string* s, MatchOptions o, size_t p),
     (r, s, o, p))

IFWD(MatchData, operator bool,(), (), ())
IFWD(MatchData, captureCount, (size_t), (), ())
IFWD(MatchData, operator[],(StringData), (size_t i), (i))
IFWD(MatchData, operator[],(StringData), (const std::string& name), (name))
IFWD(MatchData, getCaptures, (std::vector<StringData>), (), ())
IFWD(MatchData, getMatchList, (std::vector<StringData>), (), ())
IFWD(MatchData, error, (std::error_code), (), ())
IFWD(MatchData, input, (StringData), (), ())
IFWD(MatchData, startPos, (size_t), (), ())

#undef IFWD

}  // namespace mongo::pcre
