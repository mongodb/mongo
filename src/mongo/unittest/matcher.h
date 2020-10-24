/**
 *    Copyright (C) 2021-present MongoDB, Inc.
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

#include <algorithm>
#include <fmt/format.h>
#include <memory>
#include <string>
#include <tuple>
#include <utility>

#include "mongo/base/string_data.h"
#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/stdx/type_traits.h"
#include "mongo/unittest/matcher_core.h"

/**
 * Defines a basic set of matchers to be used with the ASSERT_THAT macro (see
 * `assert_that.h`). It's intended that matchers to support higher-level
 * components will be defined alongside that component's other unit testing
 * support classes, rather than in this file.
 */
namespace mongo::unittest::match {

/*
 * A uniform wrapper around any matcher that accepts a `T`, so they can
 * be treated homogeneously.
 *
 * Example:
 *     std::vector<TypeErasedMatcher<int>> vec{Eq(123), AllOf(Gt(100), Lt(200))};
 **/
template <typename T>
class TypeErasedMatcher {
public:
    using value_type = T;

    template <typename M>
    explicit TypeErasedMatcher(const M& m) : _m{std::make_shared<TypedMatch<M>>(m)} {}

    virtual ~TypeErasedMatcher() = default;

    std::string describe() const {
        return _m->describe();
    }

    MatchResult match(const T& v) const {
        return _m->match(v);
    }

private:
    struct BasicMatch {
        virtual std::string describe() const = 0;
        virtual MatchResult match(const T& v) const = 0;
    };

    template <typename M>
    class TypedMatch : public BasicMatch {
        template <typename X>
        using CanMatchOp = decltype(std::declval<M>().match(std::declval<X>()));

    public:
        explicit TypedMatch(const M& m) : _m{&m} {}
        virtual ~TypedMatch() = default;

        std::string describe() const override {
            return _m->describe();
        }

        MatchResult match(const T& v) const override {
            if constexpr (!stdx::is_detected_v<CanMatchOp, T>) {
                return MatchResult{
                    false,
                    format(FMT_STRING("Matcher does not accept {}"), demangleName(typeid(T)))};
            } else {
                return _m->match(v);
            }
        }

    private:
        const M* _m;
    };

    std::shared_ptr<BasicMatch> _m;
};

/** Always true: matches any value of any type. */
class Any : public Matcher {
public:
    std::string describe() const {
        return "Any";
    }

    template <typename X>
    MatchResult match(const X&) const {
        return MatchResult{true};
    }
};

namespace detail {

/**
 * MatchResult will be false when `m.match(v)` fails template substitution.
 * Can be used e.g. to produce a runtime-dispatched matcher for variant types.
 *
 * Example:
 *     typeTolerantMatch(Eq("hello"), 1234);  // Fails to match but compiles
 */
template <typename M, typename T>
MatchResult typeTolerantMatch(const M& m, const T& v) {
    return TypeErasedMatcher<T>(m).match(v);
}

template <template <typename> class D, typename T, typename Cmp>
class RelOpBase : public Matcher {
    const D<T>& self() const {
        return static_cast<const D<T>&>(*this);
    }

    template <typename X>
    using CanMatchOp = decltype(Cmp()(std::declval<X>(), std::declval<T>()));

public:
    explicit RelOpBase(T v) : _v{std::move(v)} {}

    std::string describe() const {
        return format(FMT_STRING("{}({})"), self().name, stringifyForAssert(_v));
    }

    template <typename X, std::enable_if_t<stdx::is_detected_v<CanMatchOp, X>, int> = 0>
    MatchResult match(const X& x) const {
        return Cmp{}(x, _v);
    }

private:
    T _v;
};

}  // namespace detail

/** Equal to. */
template <typename T>
struct Eq : detail::RelOpBase<Eq, T, std::equal_to<>> {
    using detail::RelOpBase<Eq, T, std::equal_to<>>::RelOpBase;
    static constexpr auto name = "Eq"_sd;
};
template <typename T>
Eq(T v)->Eq<T>;

/** Not equal. */
template <typename T>
struct Ne : detail::RelOpBase<Ne, T, std::not_equal_to<>> {
    using detail::RelOpBase<Ne, T, std::not_equal_to<>>::RelOpBase;
    static constexpr auto name = "Ne"_sd;
};
template <typename T>
Ne(T v)->Ne<T>;

/** Less than. */
template <typename T>
struct Lt : detail::RelOpBase<Lt, T, std::less<>> {
    using detail::RelOpBase<Lt, T, std::less<>>::RelOpBase;
    static constexpr auto name = "Lt"_sd;
};
template <typename T>
Lt(T v)->Lt<T>;

/** Greater than. */
template <typename T>
struct Gt : detail::RelOpBase<Gt, T, std::greater<>> {
    using detail::RelOpBase<Gt, T, std::greater<>>::RelOpBase;
    static constexpr auto name = "Gt"_sd;
};
template <typename T>
Gt(T v)->Gt<T>;

/** Less than or equal to. */
template <typename T>
struct Le : detail::RelOpBase<Le, T, std::less_equal<>> {
    using detail::RelOpBase<Le, T, std::less_equal<>>::RelOpBase;
    static constexpr auto name = "Le"_sd;
};
template <typename T>
Le(T v)->Le<T>;

/** Greater than or equal to. */
template <typename T>
struct Ge : detail::RelOpBase<Ge, T, std::greater_equal<>> {
    using detail::RelOpBase<Ge, T, std::greater_equal<>>::RelOpBase;
    static constexpr auto name = "Ge"_sd;
};
template <typename T>
Ge(T v)->Ge<T>;

/**
 * Wrapper that inverts the sense of a matcher.
 * Example:
 *     ASSERT_THAT("hi there", Not(ContainsRegex("hello")));
 */
template <typename M>
class Not : public Matcher {
public:
    explicit Not(M m) : _m(std::move(m)) {}

    std::string describe() const {
        return format(FMT_STRING("Not({})"), _m.describe());
    }

    template <typename X>
    MatchResult match(X&& x) const {
        auto r = _m.match(x);
        return MatchResult{!r};
    }

private:
    M _m;
};

/**
 * Given a pack of matchers, composes a matcher that passes when all matchers
 * in the pack pass.
 *
 * Example:
 *    ASSERT_THAT(123, AllOf(Gt(100), Lt(200), Eq(123)));
 */
template <typename... Ms>
class AllOf : public Matcher {
public:
    explicit AllOf(Ms... ms) : _ms(std::move(ms)...) {}

    std::string describe() const {
        return format(FMT_STRING("AllOf({})"), detail::describeTupleOfMatchers(_ms));
    }

    template <typename X>
    MatchResult match(const X& x) const {
        return _match(x, std::index_sequence_for<Ms...>{});
    }

private:
    template <typename X, size_t... Is>
    MatchResult _match(const X& x, std::index_sequence<Is...>) const {
        std::array arr{std::get<Is>(_ms).match(x)...};
        if (!std::all_of(arr.begin(), arr.end(), [](auto&& re) { return !!re; }))
            return MatchResult{false, detail::matchTupleMessage(_ms, arr)};
        return MatchResult{true};
    }

    std::tuple<Ms...> _ms;
};

/**
 * Given a pack of matchers, composees a matcher that passes when any matcher
 * in the pack passes.
 *
 * Example:
 *    ASSERT_THAT(123, AnyOf(Lt(100), Gt(200), Eq(123)));
 */
template <typename... Ms>
class AnyOf : public Matcher {
public:
    explicit AnyOf(Ms... ms) : _ms(std::move(ms)...) {}

    std::string describe() const {
        return format(FMT_STRING("AnyOf({})"), detail::describeTupleOfMatchers(_ms));
    }

    template <typename X>
    MatchResult match(const X& x) const {
        return _match(x, std::index_sequence_for<Ms...>{});
    }

private:
    template <typename X, size_t... Is>
    MatchResult _match(const X& x, std::index_sequence<Is...>) const {
        std::array arr{std::get<Is>(_ms).match(x)...};
        if (!std::any_of(arr.begin(), arr.end(), [](auto&& re) { return !!re; }))
            return MatchResult{false, detail::matchTupleMessage(_ms, arr)};
        return MatchResult{true};
    }

    std::tuple<Ms...> _ms;
};

/**
 * Match the result dereferencing pointer-like expression with unary `*`.
 * Also fails if `!x`.
 *
 * Example:
 *    int x = 123;
 *    ASSERT_THAT(&x, Pointee(Eq(123)));
 */
template <typename M>
class Pointee : public Matcher {
public:
    explicit Pointee(M m) : _m(std::move(m)) {}

    std::string describe() const {
        return format(FMT_STRING("Pointee({})"), _m.describe());
    }

    template <typename X>
    MatchResult match(const X& x) const {
        if (!x)
            return MatchResult{false, "empty pointer"};
        MatchResult res = _m.match(*x);
        if (res)
            return MatchResult{true};
        return MatchResult{false, format(FMT_STRING("{}"), res.message())};
    }

private:
    M _m;
};

/**
 * Match a string-like expression using a PCRE partial match.
 *
 * Example:
 *     ASSERT_THAT("Hello, world!", ContainsRegex("world"));
 */
class ContainsRegex : public Matcher {
public:
    explicit ContainsRegex(std::string pattern);
    ~ContainsRegex();

    std::string describe() const;

    // Should accept anything string-like
    MatchResult match(StringData x) const;

private:
    struct Impl;
    std::shared_ptr<Impl> _impl;
};


/**
 * Match a sequence container's elements against a sequence of matchers.
 * The matchers need not be of the same type.
 *
 * Example:
 *     std::vector<int> vec{5,6,7};
 *     ASSERT_THAT(vec, ElementsAre(Eq(5), Eq(6), Ge(5)));
 */
template <typename... Ms>
class ElementsAre : public Matcher {
public:
    explicit ElementsAre(const Ms&... ms) : _ms(std::move(ms)...) {}

    std::string describe() const {
        return format(FMT_STRING("ElementsAre({})"), detail::describeTupleOfMatchers(_ms));
    }

    template <typename X>
    MatchResult match(X&& x) const {
        if (x.size() != sizeof...(Ms)) {
            return MatchResult{
                false,
                format(FMT_STRING("failed: size {} != expected size {}"), x.size(), sizeof...(Ms))};
        }
        return _match(x, std::make_index_sequence<sizeof...(Ms)>{});
    }

private:
    template <typename X, size_t... Is>
    MatchResult _match(const X& x, std::index_sequence<Is...>) const {
        using std::begin;
        auto it = begin(x);
        std::array arr{std::get<Is>(_ms).match(*it++)...};
        bool allOk = true;
        detail::Joiner joiner;
        for (size_t i = 0; i != sizeof...(Ms); ++i) {
            if (!arr[i]) {
                allOk = false;
                std::string m;
                if (!arr[i].message().empty())
                    m = format(FMT_STRING(":{}"), arr[i].message());
                joiner(format(FMT_STRING("{}{}"), i, m));
            }
        }
        if (!allOk)
            return MatchResult{false, format(FMT_STRING("failed: [{}]"), std::string{joiner})};
        return MatchResult{true};
    }

    std::tuple<Ms...> _ms;
};

/**
 * Match the tuple elements of an expression.
 *
 * Example:
 *  ASSERT_THAT(std::tuple(123, "Hello, world!"),
 *              TupleElementsAre(Gt(100), ContainsRegex("Hello")));
 */
template <typename... Ms>
class TupleElementsAre : public Matcher {
public:
    explicit TupleElementsAre(const Ms&... ms) : _ms(std::move(ms)...) {}

    std::string describe() const {
        return format(FMT_STRING("TupleElementsAre({})"), detail::describeTupleOfMatchers(_ms));
    }

    template <typename X>
    MatchResult match(X&& x) const {
        size_t xSize = std::tuple_size_v<std::decay_t<X>>;
        if (xSize != sizeof...(Ms))
            return MatchResult{
                false,
                format(FMT_STRING("failed: size {} != expected size {}"), xSize, sizeof...(Ms))};
        return _match(x, std::make_index_sequence<sizeof...(Ms)>{});
    }

private:
    template <typename X, size_t... Is>
    MatchResult _match(const X& x, std::index_sequence<Is...>) const {
        std::array arr{std::get<Is>(_ms).match(std::get<Is>(x))...};
        if (!std::all_of(arr.begin(), arr.end(), [](auto&& r) { return !!r; }))
            return MatchResult{false, detail::matchTupleMessage(_ms, arr)};
        return MatchResult{true};
    }

    std::tuple<Ms...> _ms;
};

/**
 * Match that each of the structured bindings for an expression match a field matcher.
 *
 * Example:
 *  struct Obj { int x; std::string s; } obj{123, "Hello, world!"};
 *  ASSERT_THAT(obj, StructuredBindingsAre(Gt(100), ContainsRegex("Hello")));
 */
template <typename... Ms>
class StructuredBindingsAre : public Matcher {
public:
    explicit StructuredBindingsAre(const Ms&... ms) : _ms(std::move(ms)...) {}

    std::string describe() const {
        return format(FMT_STRING("StructuredBindingsAre({})"),
                      detail::describeTupleOfMatchers(_ms));
    }

    template <typename X>
    MatchResult match(const X& x) const {
        return _match(x, std::make_index_sequence<sizeof...(Ms)>{});
    }

private:
    /**
     * There are no variadic structured bindings, but it can be simulated
     * for a fixed member count up to a hardcoded limit.
     */
    template <size_t N, typename X>
    static auto _tieStruct(const X& x) {
        /*
            Can be regenerated by Python:
            N = 10
            print("    if constexpr (N == 0) {")
            print("        return std::tie();")
            for n in range(1, N):
                fs = ["f{}".format(j) for j in range(n)]
                print("    }} else if constexpr (N == {}) {{".format(n))
                print("        const auto& [{}] = x;".format(",".join(fs)))
                print("        return std::tie({});".format(",".join(fs)))
            print("    }")
        */
        if constexpr (N == 0) {
            return std::tie();
        } else if constexpr (N == 1) {
            const auto& [f0] = x;
            return std::tie(f0);
        } else if constexpr (N == 2) {
            const auto& [f0, f1] = x;
            return std::tie(f0, f1);
        } else if constexpr (N == 3) {
            const auto& [f0, f1, f2] = x;
            return std::tie(f0, f1, f2);
        } else if constexpr (N == 4) {
            const auto& [f0, f1, f2, f3] = x;
            return std::tie(f0, f1, f2, f3);
        } else if constexpr (N == 5) {
            const auto& [f0, f1, f2, f3, f4] = x;
            return std::tie(f0, f1, f2, f3, f4);
        } else if constexpr (N == 6) {
            const auto& [f0, f1, f2, f3, f4, f5] = x;
            return std::tie(f0, f1, f2, f3, f4, f5);
        } else if constexpr (N == 7) {
            const auto& [f0, f1, f2, f3, f4, f5, f6] = x;
            return std::tie(f0, f1, f2, f3, f4, f5, f6);
        } else if constexpr (N == 8) {
            const auto& [f0, f1, f2, f3, f4, f5, f6, f7] = x;
            return std::tie(f0, f1, f2, f3, f4, f5, f6, f7);
        } else if constexpr (N == 9) {
            const auto& [f0, f1, f2, f3, f4, f5, f6, f7, f8] = x;
            return std::tie(f0, f1, f2, f3, f4, f5, f6, f7, f8);
        }
        MONGO_UNREACHABLE;
    }

    template <typename X, size_t... Is>
    MatchResult _match(const X& x, std::index_sequence<Is...>) const {
        auto tied = _tieStruct<sizeof...(Ms)>(x);
        std::array arr{std::get<Is>(_ms).match(std::get<Is>(tied))...};
        if (!std::all_of(arr.begin(), arr.end(), [](auto&& r) { return !!r; }))
            return MatchResult{false, detail::matchTupleMessage(_ms, arr)};
        return MatchResult{true};
    }

private:
    std::tuple<Ms...> _ms;
};

/**
 * `StatusIs(code, reason)` matches a `Status` against matchers
 * for its code and its reason string.
 *
 * Example:
 *  ASSERT_THAT(status, StatusIs(Eq(ErrorCodes::InternalError), ContainsRegex("ouch")));
 */
template <typename CodeM, typename ReasonM>
class StatusIs : public Matcher {
public:
    StatusIs(CodeM code, ReasonM reason) : _code{std::move(code)}, _reason{std::move(reason)} {}
    std::string describe() const {
        return format(FMT_STRING("StatusIs({}, {})"), _code.describe(), _reason.describe());
    }
    MatchResult match(const Status& st) const {
        MatchResult cr = _code.match(st.code());
        MatchResult rr = _reason.match(st.reason());
        detail::Joiner joiner;
        if (!cr.message().empty())
            joiner(format(FMT_STRING("code:{}"), cr.message()));
        if (!rr.message().empty()) {
            joiner(format(FMT_STRING("reason:{}"), rr.message()));
        }
        return MatchResult{cr && rr, std::string{joiner}};
    }

private:
    CodeM _code;
    ReasonM _reason;
};

/**
 * `BSONElementIs(name,type,value)` matches a `BSONElement` against matchers
 * for its name, type, and value. Experimental: only covers some simple scalar
 * types.
 *
 * Example:
 *  ASSERT_THAT(obj, BSONObjHas(BSONElementIs(Eq("i"), Eq(NumberInt), Any())));
 */
template <typename NameM, typename TypeM, typename ValueM>
class BSONElementIs : public Matcher {
public:
    BSONElementIs(NameM nameM, TypeM typeM, ValueM valueM)
        : _name{std::move(nameM)}, _type{std::move(typeM)}, _value{std::move(valueM)} {}

    std::string describe() const {
        return format(FMT_STRING("BSONElementIs(name:{}, type:{}, value:{})"),
                      _name.describe(),
                      _type.describe(),
                      _value.describe());
    }

    MatchResult match(const BSONElement& x) const {
        auto nr = _name.match(std::string{x.fieldNameStringData()});
        if (!nr)
            return MatchResult{
                false,
                format(FMT_STRING("name failed: {} {}"), x.fieldNameStringData(), nr.message())};
        auto t = x.type();
        auto tr = _type.match(t);
        if (!tr)
            return MatchResult{
                false, format(FMT_STRING("type failed: {} {}"), typeName(x.type()), tr.message())};
        if (t == NumberInt)
            return detail::typeTolerantMatch(_value, x.Int());
        if (t == NumberLong)
            return detail::typeTolerantMatch(_value, x.Long());
        if (t == NumberDouble)
            return detail::typeTolerantMatch(_value, x.Double());
        if (t == String)
            return detail::typeTolerantMatch(_value, x.String());
        // need to support more BSON element types.
        return MatchResult{
            false, format(FMT_STRING("Cannot match BSON Elements holding type {}"), typeName(t))};
    }

private:
    NameM _name;
    TypeM _type;
    ValueM _value;
};

/**
 * `BSONObjHas(m)` matches a `BSONObj` having an element matching `m`.
 */
template <typename M>
class BSONObjHas : public Matcher {
public:
    explicit BSONObjHas(M m) : _m{std::move(m)} {}

    std::string describe() const {
        return format(FMT_STRING("BSONObjHas({})"), _m.describe());
    }

    MatchResult match(const BSONObj& x) const {
        std::vector<MatchResult> res;
        for (const auto& e : x) {
            if (auto mr = _m.match(e))
                return mr;
            else
                res.push_back(mr);
        }
        return MatchResult{false, "None of the elements matched"};
    }

private:
    M _m;
};

}  // namespace mongo::unittest::match
