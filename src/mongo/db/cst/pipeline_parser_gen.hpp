// A Bison parser, made by GNU Bison 3.5.4.

// Skeleton interface for Bison LALR(1) parsers in C++

// Copyright (C) 2002-2015, 2018-2020 Free Software Foundation, Inc.

// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.

// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.

// You should have received a copy of the GNU General Public License
// along with this program.  If not, see <http://www.gnu.org/licenses/>.

// As a special exception, you may create a larger work that contains
// part or all of the Bison parser skeleton and distribute that work
// under terms of your choice, so long as that work isn't itself a
// parser generator using the skeleton or a modified version thereof
// as a parser skeleton.  Alternatively, if you modify or redistribute
// the parser skeleton itself, you may (at your option) remove this
// special exception, which will cause the skeleton and the resulting
// Bison output files to be licensed under the GNU General Public
// License without this special exception.

// This special exception was added by the Free Software Foundation in
// version 2.2 of Bison.


/**
 ** \file src/mongo/db/cst/pipeline_parser_gen.hpp
 ** Define the mongo::parser class.
 */

// C++ LALR(1) parser skeleton written by Akim Demaille.

// Undocumented macros, especially those whose name start with YY_,
// are private implementation details.  Do not rely on them.

#ifndef YY_YY_SRC_MONGO_DB_CST_PIPELINE_PARSER_GEN_HPP_INCLUDED
#define YY_YY_SRC_MONGO_DB_CST_PIPELINE_PARSER_GEN_HPP_INCLUDED
// "%code requires" blocks.
#line 66 "src/mongo/db/cst/pipeline_grammar.yy"

#include "mongo/db/cst/c_node.h"
#include "mongo/db/cst/key_fieldname.h"
#include "mongo/stdx/variant.h"

// Forward declare any parameters needed for lexing/parsing.
namespace mongo {
class BSONLexer;
}

#ifdef _MSC_VER
// warning C4065: switch statement contains 'default' but no 'case' labels.
#pragma warning(disable : 4065)
#endif

#line 64 "src/mongo/db/cst/pipeline_parser_gen.hpp"

#include <cassert>
#include <cstdlib>  // std::abort
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

#if defined __cplusplus
#define YY_CPLUSPLUS __cplusplus
#else
#define YY_CPLUSPLUS 199711L
#endif

// Support move semantics when possible.
#if 201103L <= YY_CPLUSPLUS
#define YY_MOVE std::move
#define YY_MOVE_OR_COPY move
#define YY_MOVE_REF(Type) Type&&
#define YY_RVREF(Type) Type&&
#define YY_COPY(Type) Type
#else
#define YY_MOVE
#define YY_MOVE_OR_COPY copy
#define YY_MOVE_REF(Type) Type&
#define YY_RVREF(Type) const Type&
#define YY_COPY(Type) const Type&
#endif

// Support noexcept when possible.
#if 201103L <= YY_CPLUSPLUS
#define YY_NOEXCEPT noexcept
#define YY_NOTHROW
#else
#define YY_NOEXCEPT
#define YY_NOTHROW throw()
#endif

// Support constexpr when possible.
#if 201703 <= YY_CPLUSPLUS
#define YY_CONSTEXPR constexpr
#else
#define YY_CONSTEXPR
#endif
#include "location_gen.h"
#include <typeinfo>
#ifndef YY_ASSERT
#include <cassert>
#define YY_ASSERT assert
#endif


#ifndef YY_ATTRIBUTE_PURE
#if defined __GNUC__ && 2 < __GNUC__ + (96 <= __GNUC_MINOR__)
#define YY_ATTRIBUTE_PURE __attribute__((__pure__))
#else
#define YY_ATTRIBUTE_PURE
#endif
#endif

#ifndef YY_ATTRIBUTE_UNUSED
#if defined __GNUC__ && 2 < __GNUC__ + (7 <= __GNUC_MINOR__)
#define YY_ATTRIBUTE_UNUSED __attribute__((__unused__))
#else
#define YY_ATTRIBUTE_UNUSED
#endif
#endif

/* Suppress unused-variable warnings by "using" E.  */
#if !defined lint || defined __GNUC__
#define YYUSE(E) ((void)(E))
#else
#define YYUSE(E) /* empty */
#endif

#if defined __GNUC__ && !defined __ICC && 407 <= __GNUC__ * 100 + __GNUC_MINOR__
/* Suppress an incorrect diagnostic about yylval being uninitialized.  */
#define YY_IGNORE_MAYBE_UNINITIALIZED_BEGIN                                              \
    _Pragma("GCC diagnostic push") _Pragma("GCC diagnostic ignored \"-Wuninitialized\"") \
        _Pragma("GCC diagnostic ignored \"-Wmaybe-uninitialized\"")
#define YY_IGNORE_MAYBE_UNINITIALIZED_END _Pragma("GCC diagnostic pop")
#else
#define YY_INITIAL_VALUE(Value) Value
#endif
#ifndef YY_IGNORE_MAYBE_UNINITIALIZED_BEGIN
#define YY_IGNORE_MAYBE_UNINITIALIZED_BEGIN
#define YY_IGNORE_MAYBE_UNINITIALIZED_END
#endif
#ifndef YY_INITIAL_VALUE
#define YY_INITIAL_VALUE(Value) /* Nothing. */
#endif

#if defined __cplusplus && defined __GNUC__ && !defined __ICC && 6 <= __GNUC__
#define YY_IGNORE_USELESS_CAST_BEGIN \
    _Pragma("GCC diagnostic push") _Pragma("GCC diagnostic ignored \"-Wuseless-cast\"")
#define YY_IGNORE_USELESS_CAST_END _Pragma("GCC diagnostic pop")
#endif
#ifndef YY_IGNORE_USELESS_CAST_BEGIN
#define YY_IGNORE_USELESS_CAST_BEGIN
#define YY_IGNORE_USELESS_CAST_END
#endif

#ifndef YY_CAST
#ifdef __cplusplus
#define YY_CAST(Type, Val) static_cast<Type>(Val)
#define YY_REINTERPRET_CAST(Type, Val) reinterpret_cast<Type>(Val)
#else
#define YY_CAST(Type, Val) ((Type)(Val))
#define YY_REINTERPRET_CAST(Type, Val) ((Type)(Val))
#endif
#endif
#ifndef YY_NULLPTR
#if defined __cplusplus
#if 201103L <= __cplusplus
#define YY_NULLPTR nullptr
#else
#define YY_NULLPTR 0
#endif
#else
#define YY_NULLPTR ((void*)0)
#endif
#endif

/* Debug traces.  */
#ifndef YYDEBUG
#define YYDEBUG 0
#endif

#line 58 "src/mongo/db/cst/pipeline_grammar.yy"
namespace mongo {
#line 199 "src/mongo/db/cst/pipeline_parser_gen.hpp"


/// A Bison parser.
class PipelineParserGen {
public:
#ifndef YYSTYPE
    /// A buffer to store and retrieve objects.
    ///
    /// Sort of a variant, but does not keep track of the nature
    /// of the stored data, since that knowledge is available
    /// via the current parser state.
    class semantic_type {
    public:
        /// Type of *this.
        typedef semantic_type self_type;

        /// Empty construction.
        semantic_type() YY_NOEXCEPT : yybuffer_(), yytypeid_(YY_NULLPTR) {}

        /// Construct and fill.
        template <typename T>
        semantic_type(YY_RVREF(T) t) : yytypeid_(&typeid(T)) {
            YY_ASSERT(sizeof(T) <= size);
            new (yyas_<T>()) T(YY_MOVE(t));
        }

        /// Destruction, allowed only if empty.
        ~semantic_type() YY_NOEXCEPT {
            YY_ASSERT(!yytypeid_);
        }

#if 201103L <= YY_CPLUSPLUS
        /// Instantiate a \a T in here from \a t.
        template <typename T, typename... U>
        T& emplace(U&&... u) {
            YY_ASSERT(!yytypeid_);
            YY_ASSERT(sizeof(T) <= size);
            yytypeid_ = &typeid(T);
            return *new (yyas_<T>()) T(std::forward<U>(u)...);
        }
#else
        /// Instantiate an empty \a T in here.
        template <typename T>
        T& emplace() {
            YY_ASSERT(!yytypeid_);
            YY_ASSERT(sizeof(T) <= size);
            yytypeid_ = &typeid(T);
            return *new (yyas_<T>()) T();
        }

        /// Instantiate a \a T in here from \a t.
        template <typename T>
        T& emplace(const T& t) {
            YY_ASSERT(!yytypeid_);
            YY_ASSERT(sizeof(T) <= size);
            yytypeid_ = &typeid(T);
            return *new (yyas_<T>()) T(t);
        }
#endif

        /// Instantiate an empty \a T in here.
        /// Obsolete, use emplace.
        template <typename T>
        T& build() {
            return emplace<T>();
        }

        /// Instantiate a \a T in here from \a t.
        /// Obsolete, use emplace.
        template <typename T>
        T& build(const T& t) {
            return emplace<T>(t);
        }

        /// Accessor to a built \a T.
        template <typename T>
        T& as() YY_NOEXCEPT {
            YY_ASSERT(yytypeid_);
            YY_ASSERT(*yytypeid_ == typeid(T));
            YY_ASSERT(sizeof(T) <= size);
            return *yyas_<T>();
        }

        /// Const accessor to a built \a T (for %printer).
        template <typename T>
        const T& as() const YY_NOEXCEPT {
            YY_ASSERT(yytypeid_);
            YY_ASSERT(*yytypeid_ == typeid(T));
            YY_ASSERT(sizeof(T) <= size);
            return *yyas_<T>();
        }

        /// Swap the content with \a that, of same type.
        ///
        /// Both variants must be built beforehand, because swapping the actual
        /// data requires reading it (with as()), and this is not possible on
        /// unconstructed variants: it would require some dynamic testing, which
        /// should not be the variant's responsibility.
        /// Swapping between built and (possibly) non-built is done with
        /// self_type::move ().
        template <typename T>
        void swap(self_type& that) YY_NOEXCEPT {
            YY_ASSERT(yytypeid_);
            YY_ASSERT(*yytypeid_ == *that.yytypeid_);
            std::swap(as<T>(), that.as<T>());
        }

        /// Move the content of \a that to this.
        ///
        /// Destroys \a that.
        template <typename T>
        void move(self_type& that) {
#if 201103L <= YY_CPLUSPLUS
            emplace<T>(std::move(that.as<T>()));
#else
            emplace<T>();
            swap<T>(that);
#endif
            that.destroy<T>();
        }

#if 201103L <= YY_CPLUSPLUS
        /// Move the content of \a that to this.
        template <typename T>
        void move(self_type&& that) {
            emplace<T>(std::move(that.as<T>()));
            that.destroy<T>();
        }
#endif

        /// Copy the content of \a that to this.
        template <typename T>
        void copy(const self_type& that) {
            emplace<T>(that.as<T>());
        }

        /// Destroy the stored \a T.
        template <typename T>
        void destroy() {
            as<T>().~T();
            yytypeid_ = YY_NULLPTR;
        }

    private:
        /// Prohibit blind copies.
        self_type& operator=(const self_type&);
        semantic_type(const self_type&);

        /// Accessor to raw memory as \a T.
        template <typename T>
        T* yyas_() YY_NOEXCEPT {
            void* yyp = yybuffer_.yyraw;
            return static_cast<T*>(yyp);
        }

        /// Const accessor to raw memory as \a T.
        template <typename T>
        const T* yyas_() const YY_NOEXCEPT {
            const void* yyp = yybuffer_.yyraw;
            return static_cast<const T*>(yyp);
        }

        /// An auxiliary type to compute the largest semantic type.
        union union_type {
            // BINARY
            char dummy1[sizeof(BSONBinData)];

            // JAVASCRIPT
            char dummy2[sizeof(BSONCode)];

            // JAVASCRIPT_W_SCOPE
            char dummy3[sizeof(BSONCodeWScope)];

            // DB_POINTER
            char dummy4[sizeof(BSONDBRef)];

            // REGEX
            char dummy5[sizeof(BSONRegEx)];

            // SYMBOL
            char dummy6[sizeof(BSONSymbol)];

            // dbPointer
            // javascript
            // symbol
            // javascriptWScope
            // int
            // timestamp
            // long
            // double
            // decimal
            // minKey
            // maxKey
            // value
            // string
            // binary
            // undefined
            // objectId
            // bool
            // date
            // null
            // regex
            // simpleValue
            // compoundValue
            // valueArray
            // valueObject
            // valueFields
            // stageList
            // stage
            // inhibitOptimization
            // unionWith
            // skip
            // limit
            // project
            // sample
            // projectFields
            // projection
            // num
            // expression
            // compoundExpression
            // exprFixedTwoArg
            // expressionArray
            // expressionObject
            // expressionFields
            // maths
            // add
            // atan2
            // boolExps
            // and
            // or
            // not
            // literalEscapes
            // const
            // literal
            // compExprs
            // cmp
            // eq
            // gt
            // gte
            // lt
            // lte
            // ne
            char dummy7[sizeof(CNode)];

            // projectionFieldname
            // expressionFieldname
            // stageAsUserFieldname
            // argAsUserFieldname
            // aggExprAsUserFieldname
            // invariableUserFieldname
            // idAsUserFieldname
            // valueFieldname
            char dummy8[sizeof(CNode::Fieldname)];

            // DATE_LITERAL
            char dummy9[sizeof(Date_t)];

            // DECIMAL_NON_ZERO
            char dummy10[sizeof(Decimal128)];

            // OBJECT_ID
            char dummy11[sizeof(OID)];

            // TIMESTAMP
            char dummy12[sizeof(Timestamp)];

            // MAX_KEY
            char dummy13[sizeof(UserMaxKey)];

            // MIN_KEY
            char dummy14[sizeof(UserMinKey)];

            // JSNULL
            char dummy15[sizeof(UserNull)];

            // UNDEFINED
            char dummy16[sizeof(UserUndefined)];

            // DOUBLE_NON_ZERO
            char dummy17[sizeof(double)];

            // INT_NON_ZERO
            char dummy18[sizeof(int)];

            // LONG_NON_ZERO
            char dummy19[sizeof(long long)];

            // projectField
            // expressionField
            // valueField
            char dummy20[sizeof(std::pair<CNode::Fieldname, CNode>)];

            // FIELDNAME
            // STRING
            char dummy21[sizeof(std::string)];

            // expressions
            // values
            char dummy22[sizeof(std::vector<CNode>)];
        };

        /// The size of the largest semantic type.
        enum { size = sizeof(union_type) };

        /// A buffer to store semantic values.
        union {
            /// Strongest alignment constraints.
            long double yyalign_me;
            /// A buffer large enough to store any of the semantic values.
            char yyraw[size];
        } yybuffer_;

        /// Whether the content is built: if defined, the name of the stored type.
        const std::type_info* yytypeid_;
    };

#else
    typedef YYSTYPE semantic_type;
#endif
    /// Symbol locations.
    typedef location location_type;

    /// Syntax errors thrown from user actions.
    struct syntax_error : std::runtime_error {
        syntax_error(const location_type& l, const std::string& m)
            : std::runtime_error(m), location(l) {}

        syntax_error(const syntax_error& s) : std::runtime_error(s.what()), location(s.location) {}

        ~syntax_error() YY_NOEXCEPT YY_NOTHROW;

        location_type location;
    };

    /// Tokens.
    struct token {
        enum yytokentype {
            END_OF_FILE = 0,
            START_OBJECT = 3,
            END_OBJECT = 4,
            START_ARRAY = 5,
            END_ARRAY = 6,
            ID = 7,
            INT_ZERO = 8,
            LONG_ZERO = 9,
            DOUBLE_ZERO = 10,
            DECIMAL_ZERO = 11,
            BOOL_TRUE = 12,
            BOOL_FALSE = 13,
            STAGE_INHIBIT_OPTIMIZATION = 14,
            STAGE_LIMIT = 15,
            STAGE_PROJECT = 16,
            STAGE_SAMPLE = 17,
            STAGE_SKIP = 18,
            STAGE_UNION_WITH = 19,
            COLL_ARG = 20,
            PIPELINE_ARG = 21,
            SIZE_ARG = 22,
            ADD = 23,
            ATAN2 = 24,
            AND = 25,
            CONST_EXPR = 26,
            LITERAL = 27,
            OR = 28,
            NOT = 29,
            CMP = 30,
            EQ = 31,
            GT = 32,
            GTE = 33,
            LT = 34,
            LTE = 35,
            NE = 36,
            FIELDNAME = 37,
            STRING = 38,
            BINARY = 39,
            UNDEFINED = 40,
            OBJECT_ID = 41,
            DATE_LITERAL = 42,
            JSNULL = 43,
            REGEX = 44,
            DB_POINTER = 45,
            JAVASCRIPT = 46,
            SYMBOL = 47,
            JAVASCRIPT_W_SCOPE = 48,
            INT_NON_ZERO = 49,
            TIMESTAMP = 50,
            LONG_NON_ZERO = 51,
            DOUBLE_NON_ZERO = 52,
            DECIMAL_NON_ZERO = 53,
            MIN_KEY = 54,
            MAX_KEY = 55
        };
    };

    /// (External) token type, as returned by yylex.
    typedef token::yytokentype token_type;

    /// Symbol type: an internal symbol number.
    typedef int symbol_number_type;

    /// The symbol type number to denote an empty symbol.
    enum { empty_symbol = -2 };

    /// Internal symbol number for tokens (subsumed by symbol_number_type).
    typedef signed char token_number_type;

    /// A complete symbol.
    ///
    /// Expects its Base type to provide access to the symbol type
    /// via type_get ().
    ///
    /// Provide access to semantic value and location.
    template <typename Base>
    struct basic_symbol : Base {
        /// Alias to Base.
        typedef Base super_type;

        /// Default constructor.
        basic_symbol() : value(), location() {}

#if 201103L <= YY_CPLUSPLUS
        /// Move constructor.
        basic_symbol(basic_symbol&& that);
#endif

        /// Copy constructor.
        basic_symbol(const basic_symbol& that);

        /// Constructor for valueless symbols, and symbols from each type.
#if 201103L <= YY_CPLUSPLUS
        basic_symbol(typename Base::kind_type t, location_type&& l)
            : Base(t), location(std::move(l)) {}
#else
        basic_symbol(typename Base::kind_type t, const location_type& l) : Base(t), location(l) {}
#endif
#if 201103L <= YY_CPLUSPLUS
        basic_symbol(typename Base::kind_type t, BSONBinData&& v, location_type&& l)
            : Base(t), value(std::move(v)), location(std::move(l)) {}
#else
        basic_symbol(typename Base::kind_type t, const BSONBinData& v, const location_type& l)
            : Base(t), value(v), location(l) {}
#endif
#if 201103L <= YY_CPLUSPLUS
        basic_symbol(typename Base::kind_type t, BSONCode&& v, location_type&& l)
            : Base(t), value(std::move(v)), location(std::move(l)) {}
#else
        basic_symbol(typename Base::kind_type t, const BSONCode& v, const location_type& l)
            : Base(t), value(v), location(l) {}
#endif
#if 201103L <= YY_CPLUSPLUS
        basic_symbol(typename Base::kind_type t, BSONCodeWScope&& v, location_type&& l)
            : Base(t), value(std::move(v)), location(std::move(l)) {}
#else
        basic_symbol(typename Base::kind_type t, const BSONCodeWScope& v, const location_type& l)
            : Base(t), value(v), location(l) {}
#endif
#if 201103L <= YY_CPLUSPLUS
        basic_symbol(typename Base::kind_type t, BSONDBRef&& v, location_type&& l)
            : Base(t), value(std::move(v)), location(std::move(l)) {}
#else
        basic_symbol(typename Base::kind_type t, const BSONDBRef& v, const location_type& l)
            : Base(t), value(v), location(l) {}
#endif
#if 201103L <= YY_CPLUSPLUS
        basic_symbol(typename Base::kind_type t, BSONRegEx&& v, location_type&& l)
            : Base(t), value(std::move(v)), location(std::move(l)) {}
#else
        basic_symbol(typename Base::kind_type t, const BSONRegEx& v, const location_type& l)
            : Base(t), value(v), location(l) {}
#endif
#if 201103L <= YY_CPLUSPLUS
        basic_symbol(typename Base::kind_type t, BSONSymbol&& v, location_type&& l)
            : Base(t), value(std::move(v)), location(std::move(l)) {}
#else
        basic_symbol(typename Base::kind_type t, const BSONSymbol& v, const location_type& l)
            : Base(t), value(v), location(l) {}
#endif
#if 201103L <= YY_CPLUSPLUS
        basic_symbol(typename Base::kind_type t, CNode&& v, location_type&& l)
            : Base(t), value(std::move(v)), location(std::move(l)) {}
#else
        basic_symbol(typename Base::kind_type t, const CNode& v, const location_type& l)
            : Base(t), value(v), location(l) {}
#endif
#if 201103L <= YY_CPLUSPLUS
        basic_symbol(typename Base::kind_type t, CNode::Fieldname&& v, location_type&& l)
            : Base(t), value(std::move(v)), location(std::move(l)) {}
#else
        basic_symbol(typename Base::kind_type t, const CNode::Fieldname& v, const location_type& l)
            : Base(t), value(v), location(l) {}
#endif
#if 201103L <= YY_CPLUSPLUS
        basic_symbol(typename Base::kind_type t, Date_t&& v, location_type&& l)
            : Base(t), value(std::move(v)), location(std::move(l)) {}
#else
        basic_symbol(typename Base::kind_type t, const Date_t& v, const location_type& l)
            : Base(t), value(v), location(l) {}
#endif
#if 201103L <= YY_CPLUSPLUS
        basic_symbol(typename Base::kind_type t, Decimal128&& v, location_type&& l)
            : Base(t), value(std::move(v)), location(std::move(l)) {}
#else
        basic_symbol(typename Base::kind_type t, const Decimal128& v, const location_type& l)
            : Base(t), value(v), location(l) {}
#endif
#if 201103L <= YY_CPLUSPLUS
        basic_symbol(typename Base::kind_type t, OID&& v, location_type&& l)
            : Base(t), value(std::move(v)), location(std::move(l)) {}
#else
        basic_symbol(typename Base::kind_type t, const OID& v, const location_type& l)
            : Base(t), value(v), location(l) {}
#endif
#if 201103L <= YY_CPLUSPLUS
        basic_symbol(typename Base::kind_type t, Timestamp&& v, location_type&& l)
            : Base(t), value(std::move(v)), location(std::move(l)) {}
#else
        basic_symbol(typename Base::kind_type t, const Timestamp& v, const location_type& l)
            : Base(t), value(v), location(l) {}
#endif
#if 201103L <= YY_CPLUSPLUS
        basic_symbol(typename Base::kind_type t, UserMaxKey&& v, location_type&& l)
            : Base(t), value(std::move(v)), location(std::move(l)) {}
#else
        basic_symbol(typename Base::kind_type t, const UserMaxKey& v, const location_type& l)
            : Base(t), value(v), location(l) {}
#endif
#if 201103L <= YY_CPLUSPLUS
        basic_symbol(typename Base::kind_type t, UserMinKey&& v, location_type&& l)
            : Base(t), value(std::move(v)), location(std::move(l)) {}
#else
        basic_symbol(typename Base::kind_type t, const UserMinKey& v, const location_type& l)
            : Base(t), value(v), location(l) {}
#endif
#if 201103L <= YY_CPLUSPLUS
        basic_symbol(typename Base::kind_type t, UserNull&& v, location_type&& l)
            : Base(t), value(std::move(v)), location(std::move(l)) {}
#else
        basic_symbol(typename Base::kind_type t, const UserNull& v, const location_type& l)
            : Base(t), value(v), location(l) {}
#endif
#if 201103L <= YY_CPLUSPLUS
        basic_symbol(typename Base::kind_type t, UserUndefined&& v, location_type&& l)
            : Base(t), value(std::move(v)), location(std::move(l)) {}
#else
        basic_symbol(typename Base::kind_type t, const UserUndefined& v, const location_type& l)
            : Base(t), value(v), location(l) {}
#endif
#if 201103L <= YY_CPLUSPLUS
        basic_symbol(typename Base::kind_type t, double&& v, location_type&& l)
            : Base(t), value(std::move(v)), location(std::move(l)) {}
#else
        basic_symbol(typename Base::kind_type t, const double& v, const location_type& l)
            : Base(t), value(v), location(l) {}
#endif
#if 201103L <= YY_CPLUSPLUS
        basic_symbol(typename Base::kind_type t, int&& v, location_type&& l)
            : Base(t), value(std::move(v)), location(std::move(l)) {}
#else
        basic_symbol(typename Base::kind_type t, const int& v, const location_type& l)
            : Base(t), value(v), location(l) {}
#endif
#if 201103L <= YY_CPLUSPLUS
        basic_symbol(typename Base::kind_type t, long long&& v, location_type&& l)
            : Base(t), value(std::move(v)), location(std::move(l)) {}
#else
        basic_symbol(typename Base::kind_type t, const long long& v, const location_type& l)
            : Base(t), value(v), location(l) {}
#endif
#if 201103L <= YY_CPLUSPLUS
        basic_symbol(typename Base::kind_type t,
                     std::pair<CNode::Fieldname, CNode>&& v,
                     location_type&& l)
            : Base(t), value(std::move(v)), location(std::move(l)) {}
#else
        basic_symbol(typename Base::kind_type t,
                     const std::pair<CNode::Fieldname, CNode>& v,
                     const location_type& l)
            : Base(t), value(v), location(l) {}
#endif
#if 201103L <= YY_CPLUSPLUS
        basic_symbol(typename Base::kind_type t, std::string&& v, location_type&& l)
            : Base(t), value(std::move(v)), location(std::move(l)) {}
#else
        basic_symbol(typename Base::kind_type t, const std::string& v, const location_type& l)
            : Base(t), value(v), location(l) {}
#endif
#if 201103L <= YY_CPLUSPLUS
        basic_symbol(typename Base::kind_type t, std::vector<CNode>&& v, location_type&& l)
            : Base(t), value(std::move(v)), location(std::move(l)) {}
#else
        basic_symbol(typename Base::kind_type t,
                     const std::vector<CNode>& v,
                     const location_type& l)
            : Base(t), value(v), location(l) {}
#endif

        /// Destroy the symbol.
        ~basic_symbol() {
            clear();
        }

        /// Destroy contents, and record that is empty.
        void clear() {
            // User destructor.
            symbol_number_type yytype = this->type_get();
            basic_symbol<Base>& yysym = *this;
            (void)yysym;
            switch (yytype) {
                default:
                    break;
            }

            // Type destructor.
            switch (yytype) {
                case 39:  // BINARY
                    value.template destroy<BSONBinData>();
                    break;

                case 46:  // JAVASCRIPT
                    value.template destroy<BSONCode>();
                    break;

                case 48:  // JAVASCRIPT_W_SCOPE
                    value.template destroy<BSONCodeWScope>();
                    break;

                case 45:  // DB_POINTER
                    value.template destroy<BSONDBRef>();
                    break;

                case 44:  // REGEX
                    value.template destroy<BSONRegEx>();
                    break;

                case 47:  // SYMBOL
                    value.template destroy<BSONSymbol>();
                    break;

                case 68:   // dbPointer
                case 69:   // javascript
                case 70:   // symbol
                case 71:   // javascriptWScope
                case 72:   // int
                case 73:   // timestamp
                case 74:   // long
                case 75:   // double
                case 76:   // decimal
                case 77:   // minKey
                case 78:   // maxKey
                case 79:   // value
                case 80:   // string
                case 81:   // binary
                case 82:   // undefined
                case 83:   // objectId
                case 84:   // bool
                case 85:   // date
                case 86:   // null
                case 87:   // regex
                case 88:   // simpleValue
                case 89:   // compoundValue
                case 90:   // valueArray
                case 91:   // valueObject
                case 92:   // valueFields
                case 93:   // stageList
                case 94:   // stage
                case 95:   // inhibitOptimization
                case 96:   // unionWith
                case 97:   // skip
                case 98:   // limit
                case 99:   // project
                case 100:  // sample
                case 101:  // projectFields
                case 102:  // projection
                case 103:  // num
                case 104:  // expression
                case 105:  // compoundExpression
                case 106:  // exprFixedTwoArg
                case 107:  // expressionArray
                case 108:  // expressionObject
                case 109:  // expressionFields
                case 110:  // maths
                case 111:  // add
                case 112:  // atan2
                case 113:  // boolExps
                case 114:  // and
                case 115:  // or
                case 116:  // not
                case 117:  // literalEscapes
                case 118:  // const
                case 119:  // literal
                case 120:  // compExprs
                case 121:  // cmp
                case 122:  // eq
                case 123:  // gt
                case 124:  // gte
                case 125:  // lt
                case 126:  // lte
                case 127:  // ne
                    value.template destroy<CNode>();
                    break;

                case 57:  // projectionFieldname
                case 58:  // expressionFieldname
                case 59:  // stageAsUserFieldname
                case 60:  // argAsUserFieldname
                case 61:  // aggExprAsUserFieldname
                case 62:  // invariableUserFieldname
                case 63:  // idAsUserFieldname
                case 64:  // valueFieldname
                    value.template destroy<CNode::Fieldname>();
                    break;

                case 42:  // DATE_LITERAL
                    value.template destroy<Date_t>();
                    break;

                case 53:  // DECIMAL_NON_ZERO
                    value.template destroy<Decimal128>();
                    break;

                case 41:  // OBJECT_ID
                    value.template destroy<OID>();
                    break;

                case 50:  // TIMESTAMP
                    value.template destroy<Timestamp>();
                    break;

                case 55:  // MAX_KEY
                    value.template destroy<UserMaxKey>();
                    break;

                case 54:  // MIN_KEY
                    value.template destroy<UserMinKey>();
                    break;

                case 43:  // JSNULL
                    value.template destroy<UserNull>();
                    break;

                case 40:  // UNDEFINED
                    value.template destroy<UserUndefined>();
                    break;

                case 52:  // DOUBLE_NON_ZERO
                    value.template destroy<double>();
                    break;

                case 49:  // INT_NON_ZERO
                    value.template destroy<int>();
                    break;

                case 51:  // LONG_NON_ZERO
                    value.template destroy<long long>();
                    break;

                case 65:  // projectField
                case 66:  // expressionField
                case 67:  // valueField
                    value.template destroy<std::pair<CNode::Fieldname, CNode>>();
                    break;

                case 37:  // FIELDNAME
                case 38:  // STRING
                    value.template destroy<std::string>();
                    break;

                case 128:  // expressions
                case 129:  // values
                    value.template destroy<std::vector<CNode>>();
                    break;

                default:
                    break;
            }

            Base::clear();
        }

        /// Whether empty.
        bool empty() const YY_NOEXCEPT;

        /// Destructive move, \a s is emptied into this.
        void move(basic_symbol& s);

        /// The semantic value.
        semantic_type value;

        /// The location.
        location_type location;

    private:
#if YY_CPLUSPLUS < 201103L
        /// Assignment operator.
        basic_symbol& operator=(const basic_symbol& that);
#endif
    };

    /// Type access provider for token (enum) based symbols.
    struct by_type {
        /// Default constructor.
        by_type();

#if 201103L <= YY_CPLUSPLUS
        /// Move constructor.
        by_type(by_type&& that);
#endif

        /// Copy constructor.
        by_type(const by_type& that);

        /// The symbol type as needed by the constructor.
        typedef token_type kind_type;

        /// Constructor from (external) token numbers.
        by_type(kind_type t);

        /// Record that this symbol is empty.
        void clear();

        /// Steal the symbol type from \a that.
        void move(by_type& that);

        /// The (internal) type number (corresponding to \a type).
        /// \a empty when empty.
        symbol_number_type type_get() const YY_NOEXCEPT;

        /// The symbol type.
        /// \a empty_symbol when empty.
        /// An int, not token_number_type, to be able to store empty_symbol.
        int type;
    };

    /// "External" symbols: returned by the scanner.
    struct symbol_type : basic_symbol<by_type> {
        /// Superclass.
        typedef basic_symbol<by_type> super_type;

        /// Empty symbol.
        symbol_type() {}

        /// Constructor for valueless symbols, and symbols from each type.
#if 201103L <= YY_CPLUSPLUS
        symbol_type(int tok, location_type l) : super_type(token_type(tok), std::move(l)) {
            YY_ASSERT(
                tok == token::END_OF_FILE || tok == token::START_OBJECT ||
                tok == token::END_OBJECT || tok == token::START_ARRAY || tok == token::END_ARRAY ||
                tok == token::ID || tok == token::INT_ZERO || tok == token::LONG_ZERO ||
                tok == token::DOUBLE_ZERO || tok == token::DECIMAL_ZERO ||
                tok == token::BOOL_TRUE || tok == token::BOOL_FALSE ||
                tok == token::STAGE_INHIBIT_OPTIMIZATION || tok == token::STAGE_LIMIT ||
                tok == token::STAGE_PROJECT || tok == token::STAGE_SAMPLE ||
                tok == token::STAGE_SKIP || tok == token::STAGE_UNION_WITH ||
                tok == token::COLL_ARG || tok == token::PIPELINE_ARG || tok == token::SIZE_ARG ||
                tok == token::ADD || tok == token::ATAN2 || tok == token::AND ||
                tok == token::CONST_EXPR || tok == token::LITERAL || tok == token::OR ||
                tok == token::NOT || tok == token::CMP || tok == token::EQ || tok == token::GT ||
                tok == token::GTE || tok == token::LT || tok == token::LTE || tok == token::NE);
        }
#else
        symbol_type(int tok, const location_type& l) : super_type(token_type(tok), l) {
            YY_ASSERT(
                tok == token::END_OF_FILE || tok == token::START_OBJECT ||
                tok == token::END_OBJECT || tok == token::START_ARRAY || tok == token::END_ARRAY ||
                tok == token::ID || tok == token::INT_ZERO || tok == token::LONG_ZERO ||
                tok == token::DOUBLE_ZERO || tok == token::DECIMAL_ZERO ||
                tok == token::BOOL_TRUE || tok == token::BOOL_FALSE ||
                tok == token::STAGE_INHIBIT_OPTIMIZATION || tok == token::STAGE_LIMIT ||
                tok == token::STAGE_PROJECT || tok == token::STAGE_SAMPLE ||
                tok == token::STAGE_SKIP || tok == token::STAGE_UNION_WITH ||
                tok == token::COLL_ARG || tok == token::PIPELINE_ARG || tok == token::SIZE_ARG ||
                tok == token::ADD || tok == token::ATAN2 || tok == token::AND ||
                tok == token::CONST_EXPR || tok == token::LITERAL || tok == token::OR ||
                tok == token::NOT || tok == token::CMP || tok == token::EQ || tok == token::GT ||
                tok == token::GTE || tok == token::LT || tok == token::LTE || tok == token::NE);
        }
#endif
#if 201103L <= YY_CPLUSPLUS
        symbol_type(int tok, BSONBinData v, location_type l)
            : super_type(token_type(tok), std::move(v), std::move(l)) {
            YY_ASSERT(tok == token::BINARY);
        }
#else
        symbol_type(int tok, const BSONBinData& v, const location_type& l)
            : super_type(token_type(tok), v, l) {
            YY_ASSERT(tok == token::BINARY);
        }
#endif
#if 201103L <= YY_CPLUSPLUS
        symbol_type(int tok, BSONCode v, location_type l)
            : super_type(token_type(tok), std::move(v), std::move(l)) {
            YY_ASSERT(tok == token::JAVASCRIPT);
        }
#else
        symbol_type(int tok, const BSONCode& v, const location_type& l)
            : super_type(token_type(tok), v, l) {
            YY_ASSERT(tok == token::JAVASCRIPT);
        }
#endif
#if 201103L <= YY_CPLUSPLUS
        symbol_type(int tok, BSONCodeWScope v, location_type l)
            : super_type(token_type(tok), std::move(v), std::move(l)) {
            YY_ASSERT(tok == token::JAVASCRIPT_W_SCOPE);
        }
#else
        symbol_type(int tok, const BSONCodeWScope& v, const location_type& l)
            : super_type(token_type(tok), v, l) {
            YY_ASSERT(tok == token::JAVASCRIPT_W_SCOPE);
        }
#endif
#if 201103L <= YY_CPLUSPLUS
        symbol_type(int tok, BSONDBRef v, location_type l)
            : super_type(token_type(tok), std::move(v), std::move(l)) {
            YY_ASSERT(tok == token::DB_POINTER);
        }
#else
        symbol_type(int tok, const BSONDBRef& v, const location_type& l)
            : super_type(token_type(tok), v, l) {
            YY_ASSERT(tok == token::DB_POINTER);
        }
#endif
#if 201103L <= YY_CPLUSPLUS
        symbol_type(int tok, BSONRegEx v, location_type l)
            : super_type(token_type(tok), std::move(v), std::move(l)) {
            YY_ASSERT(tok == token::REGEX);
        }
#else
        symbol_type(int tok, const BSONRegEx& v, const location_type& l)
            : super_type(token_type(tok), v, l) {
            YY_ASSERT(tok == token::REGEX);
        }
#endif
#if 201103L <= YY_CPLUSPLUS
        symbol_type(int tok, BSONSymbol v, location_type l)
            : super_type(token_type(tok), std::move(v), std::move(l)) {
            YY_ASSERT(tok == token::SYMBOL);
        }
#else
        symbol_type(int tok, const BSONSymbol& v, const location_type& l)
            : super_type(token_type(tok), v, l) {
            YY_ASSERT(tok == token::SYMBOL);
        }
#endif
#if 201103L <= YY_CPLUSPLUS
        symbol_type(int tok, Date_t v, location_type l)
            : super_type(token_type(tok), std::move(v), std::move(l)) {
            YY_ASSERT(tok == token::DATE_LITERAL);
        }
#else
        symbol_type(int tok, const Date_t& v, const location_type& l)
            : super_type(token_type(tok), v, l) {
            YY_ASSERT(tok == token::DATE_LITERAL);
        }
#endif
#if 201103L <= YY_CPLUSPLUS
        symbol_type(int tok, Decimal128 v, location_type l)
            : super_type(token_type(tok), std::move(v), std::move(l)) {
            YY_ASSERT(tok == token::DECIMAL_NON_ZERO);
        }
#else
        symbol_type(int tok, const Decimal128& v, const location_type& l)
            : super_type(token_type(tok), v, l) {
            YY_ASSERT(tok == token::DECIMAL_NON_ZERO);
        }
#endif
#if 201103L <= YY_CPLUSPLUS
        symbol_type(int tok, OID v, location_type l)
            : super_type(token_type(tok), std::move(v), std::move(l)) {
            YY_ASSERT(tok == token::OBJECT_ID);
        }
#else
        symbol_type(int tok, const OID& v, const location_type& l)
            : super_type(token_type(tok), v, l) {
            YY_ASSERT(tok == token::OBJECT_ID);
        }
#endif
#if 201103L <= YY_CPLUSPLUS
        symbol_type(int tok, Timestamp v, location_type l)
            : super_type(token_type(tok), std::move(v), std::move(l)) {
            YY_ASSERT(tok == token::TIMESTAMP);
        }
#else
        symbol_type(int tok, const Timestamp& v, const location_type& l)
            : super_type(token_type(tok), v, l) {
            YY_ASSERT(tok == token::TIMESTAMP);
        }
#endif
#if 201103L <= YY_CPLUSPLUS
        symbol_type(int tok, UserMaxKey v, location_type l)
            : super_type(token_type(tok), std::move(v), std::move(l)) {
            YY_ASSERT(tok == token::MAX_KEY);
        }
#else
        symbol_type(int tok, const UserMaxKey& v, const location_type& l)
            : super_type(token_type(tok), v, l) {
            YY_ASSERT(tok == token::MAX_KEY);
        }
#endif
#if 201103L <= YY_CPLUSPLUS
        symbol_type(int tok, UserMinKey v, location_type l)
            : super_type(token_type(tok), std::move(v), std::move(l)) {
            YY_ASSERT(tok == token::MIN_KEY);
        }
#else
        symbol_type(int tok, const UserMinKey& v, const location_type& l)
            : super_type(token_type(tok), v, l) {
            YY_ASSERT(tok == token::MIN_KEY);
        }
#endif
#if 201103L <= YY_CPLUSPLUS
        symbol_type(int tok, UserNull v, location_type l)
            : super_type(token_type(tok), std::move(v), std::move(l)) {
            YY_ASSERT(tok == token::JSNULL);
        }
#else
        symbol_type(int tok, const UserNull& v, const location_type& l)
            : super_type(token_type(tok), v, l) {
            YY_ASSERT(tok == token::JSNULL);
        }
#endif
#if 201103L <= YY_CPLUSPLUS
        symbol_type(int tok, UserUndefined v, location_type l)
            : super_type(token_type(tok), std::move(v), std::move(l)) {
            YY_ASSERT(tok == token::UNDEFINED);
        }
#else
        symbol_type(int tok, const UserUndefined& v, const location_type& l)
            : super_type(token_type(tok), v, l) {
            YY_ASSERT(tok == token::UNDEFINED);
        }
#endif
#if 201103L <= YY_CPLUSPLUS
        symbol_type(int tok, double v, location_type l)
            : super_type(token_type(tok), std::move(v), std::move(l)) {
            YY_ASSERT(tok == token::DOUBLE_NON_ZERO);
        }
#else
        symbol_type(int tok, const double& v, const location_type& l)
            : super_type(token_type(tok), v, l) {
            YY_ASSERT(tok == token::DOUBLE_NON_ZERO);
        }
#endif
#if 201103L <= YY_CPLUSPLUS
        symbol_type(int tok, int v, location_type l)
            : super_type(token_type(tok), std::move(v), std::move(l)) {
            YY_ASSERT(tok == token::INT_NON_ZERO);
        }
#else
        symbol_type(int tok, const int& v, const location_type& l)
            : super_type(token_type(tok), v, l) {
            YY_ASSERT(tok == token::INT_NON_ZERO);
        }
#endif
#if 201103L <= YY_CPLUSPLUS
        symbol_type(int tok, long long v, location_type l)
            : super_type(token_type(tok), std::move(v), std::move(l)) {
            YY_ASSERT(tok == token::LONG_NON_ZERO);
        }
#else
        symbol_type(int tok, const long long& v, const location_type& l)
            : super_type(token_type(tok), v, l) {
            YY_ASSERT(tok == token::LONG_NON_ZERO);
        }
#endif
#if 201103L <= YY_CPLUSPLUS
        symbol_type(int tok, std::string v, location_type l)
            : super_type(token_type(tok), std::move(v), std::move(l)) {
            YY_ASSERT(tok == token::FIELDNAME || tok == token::STRING);
        }
#else
        symbol_type(int tok, const std::string& v, const location_type& l)
            : super_type(token_type(tok), v, l) {
            YY_ASSERT(tok == token::FIELDNAME || tok == token::STRING);
        }
#endif
    };

    /// Build a parser object.
    PipelineParserGen(BSONLexer& lexer_yyarg, CNode* cst_yyarg);
    virtual ~PipelineParserGen();

    /// Parse.  An alias for parse ().
    /// \returns  0 iff parsing succeeded.
    int operator()();

    /// Parse.
    /// \returns  0 iff parsing succeeded.
    virtual int parse();

#if YYDEBUG
    /// The current debugging stream.
    std::ostream& debug_stream() const YY_ATTRIBUTE_PURE;
    /// Set the current debugging stream.
    void set_debug_stream(std::ostream&);

    /// Type for debugging levels.
    typedef int debug_level_type;
    /// The current debugging level.
    debug_level_type debug_level() const YY_ATTRIBUTE_PURE;
    /// Set the current debugging level.
    void set_debug_level(debug_level_type l);
#endif

    /// Report a syntax error.
    /// \param loc    where the syntax error is found.
    /// \param msg    a description of the syntax error.
    virtual void error(const location_type& loc, const std::string& msg);

    /// Report a syntax error.
    void error(const syntax_error& err);

    // Implementation of make_symbol for each symbol type.
#if 201103L <= YY_CPLUSPLUS
    static symbol_type make_END_OF_FILE(location_type l) {
        return symbol_type(token::END_OF_FILE, std::move(l));
    }
#else
    static symbol_type make_END_OF_FILE(const location_type& l) {
        return symbol_type(token::END_OF_FILE, l);
    }
#endif
#if 201103L <= YY_CPLUSPLUS
    static symbol_type make_START_OBJECT(location_type l) {
        return symbol_type(token::START_OBJECT, std::move(l));
    }
#else
    static symbol_type make_START_OBJECT(const location_type& l) {
        return symbol_type(token::START_OBJECT, l);
    }
#endif
#if 201103L <= YY_CPLUSPLUS
    static symbol_type make_END_OBJECT(location_type l) {
        return symbol_type(token::END_OBJECT, std::move(l));
    }
#else
    static symbol_type make_END_OBJECT(const location_type& l) {
        return symbol_type(token::END_OBJECT, l);
    }
#endif
#if 201103L <= YY_CPLUSPLUS
    static symbol_type make_START_ARRAY(location_type l) {
        return symbol_type(token::START_ARRAY, std::move(l));
    }
#else
    static symbol_type make_START_ARRAY(const location_type& l) {
        return symbol_type(token::START_ARRAY, l);
    }
#endif
#if 201103L <= YY_CPLUSPLUS
    static symbol_type make_END_ARRAY(location_type l) {
        return symbol_type(token::END_ARRAY, std::move(l));
    }
#else
    static symbol_type make_END_ARRAY(const location_type& l) {
        return symbol_type(token::END_ARRAY, l);
    }
#endif
#if 201103L <= YY_CPLUSPLUS
    static symbol_type make_ID(location_type l) {
        return symbol_type(token::ID, std::move(l));
    }
#else
    static symbol_type make_ID(const location_type& l) {
        return symbol_type(token::ID, l);
    }
#endif
#if 201103L <= YY_CPLUSPLUS
    static symbol_type make_INT_ZERO(location_type l) {
        return symbol_type(token::INT_ZERO, std::move(l));
    }
#else
    static symbol_type make_INT_ZERO(const location_type& l) {
        return symbol_type(token::INT_ZERO, l);
    }
#endif
#if 201103L <= YY_CPLUSPLUS
    static symbol_type make_LONG_ZERO(location_type l) {
        return symbol_type(token::LONG_ZERO, std::move(l));
    }
#else
    static symbol_type make_LONG_ZERO(const location_type& l) {
        return symbol_type(token::LONG_ZERO, l);
    }
#endif
#if 201103L <= YY_CPLUSPLUS
    static symbol_type make_DOUBLE_ZERO(location_type l) {
        return symbol_type(token::DOUBLE_ZERO, std::move(l));
    }
#else
    static symbol_type make_DOUBLE_ZERO(const location_type& l) {
        return symbol_type(token::DOUBLE_ZERO, l);
    }
#endif
#if 201103L <= YY_CPLUSPLUS
    static symbol_type make_DECIMAL_ZERO(location_type l) {
        return symbol_type(token::DECIMAL_ZERO, std::move(l));
    }
#else
    static symbol_type make_DECIMAL_ZERO(const location_type& l) {
        return symbol_type(token::DECIMAL_ZERO, l);
    }
#endif
#if 201103L <= YY_CPLUSPLUS
    static symbol_type make_BOOL_TRUE(location_type l) {
        return symbol_type(token::BOOL_TRUE, std::move(l));
    }
#else
    static symbol_type make_BOOL_TRUE(const location_type& l) {
        return symbol_type(token::BOOL_TRUE, l);
    }
#endif
#if 201103L <= YY_CPLUSPLUS
    static symbol_type make_BOOL_FALSE(location_type l) {
        return symbol_type(token::BOOL_FALSE, std::move(l));
    }
#else
    static symbol_type make_BOOL_FALSE(const location_type& l) {
        return symbol_type(token::BOOL_FALSE, l);
    }
#endif
#if 201103L <= YY_CPLUSPLUS
    static symbol_type make_STAGE_INHIBIT_OPTIMIZATION(location_type l) {
        return symbol_type(token::STAGE_INHIBIT_OPTIMIZATION, std::move(l));
    }
#else
    static symbol_type make_STAGE_INHIBIT_OPTIMIZATION(const location_type& l) {
        return symbol_type(token::STAGE_INHIBIT_OPTIMIZATION, l);
    }
#endif
#if 201103L <= YY_CPLUSPLUS
    static symbol_type make_STAGE_LIMIT(location_type l) {
        return symbol_type(token::STAGE_LIMIT, std::move(l));
    }
#else
    static symbol_type make_STAGE_LIMIT(const location_type& l) {
        return symbol_type(token::STAGE_LIMIT, l);
    }
#endif
#if 201103L <= YY_CPLUSPLUS
    static symbol_type make_STAGE_PROJECT(location_type l) {
        return symbol_type(token::STAGE_PROJECT, std::move(l));
    }
#else
    static symbol_type make_STAGE_PROJECT(const location_type& l) {
        return symbol_type(token::STAGE_PROJECT, l);
    }
#endif
#if 201103L <= YY_CPLUSPLUS
    static symbol_type make_STAGE_SAMPLE(location_type l) {
        return symbol_type(token::STAGE_SAMPLE, std::move(l));
    }
#else
    static symbol_type make_STAGE_SAMPLE(const location_type& l) {
        return symbol_type(token::STAGE_SAMPLE, l);
    }
#endif
#if 201103L <= YY_CPLUSPLUS
    static symbol_type make_STAGE_SKIP(location_type l) {
        return symbol_type(token::STAGE_SKIP, std::move(l));
    }
#else
    static symbol_type make_STAGE_SKIP(const location_type& l) {
        return symbol_type(token::STAGE_SKIP, l);
    }
#endif
#if 201103L <= YY_CPLUSPLUS
    static symbol_type make_STAGE_UNION_WITH(location_type l) {
        return symbol_type(token::STAGE_UNION_WITH, std::move(l));
    }
#else
    static symbol_type make_STAGE_UNION_WITH(const location_type& l) {
        return symbol_type(token::STAGE_UNION_WITH, l);
    }
#endif
#if 201103L <= YY_CPLUSPLUS
    static symbol_type make_COLL_ARG(location_type l) {
        return symbol_type(token::COLL_ARG, std::move(l));
    }
#else
    static symbol_type make_COLL_ARG(const location_type& l) {
        return symbol_type(token::COLL_ARG, l);
    }
#endif
#if 201103L <= YY_CPLUSPLUS
    static symbol_type make_PIPELINE_ARG(location_type l) {
        return symbol_type(token::PIPELINE_ARG, std::move(l));
    }
#else
    static symbol_type make_PIPELINE_ARG(const location_type& l) {
        return symbol_type(token::PIPELINE_ARG, l);
    }
#endif
#if 201103L <= YY_CPLUSPLUS
    static symbol_type make_SIZE_ARG(location_type l) {
        return symbol_type(token::SIZE_ARG, std::move(l));
    }
#else
    static symbol_type make_SIZE_ARG(const location_type& l) {
        return symbol_type(token::SIZE_ARG, l);
    }
#endif
#if 201103L <= YY_CPLUSPLUS
    static symbol_type make_ADD(location_type l) {
        return symbol_type(token::ADD, std::move(l));
    }
#else
    static symbol_type make_ADD(const location_type& l) {
        return symbol_type(token::ADD, l);
    }
#endif
#if 201103L <= YY_CPLUSPLUS
    static symbol_type make_ATAN2(location_type l) {
        return symbol_type(token::ATAN2, std::move(l));
    }
#else
    static symbol_type make_ATAN2(const location_type& l) {
        return symbol_type(token::ATAN2, l);
    }
#endif
#if 201103L <= YY_CPLUSPLUS
    static symbol_type make_AND(location_type l) {
        return symbol_type(token::AND, std::move(l));
    }
#else
    static symbol_type make_AND(const location_type& l) {
        return symbol_type(token::AND, l);
    }
#endif
#if 201103L <= YY_CPLUSPLUS
    static symbol_type make_CONST_EXPR(location_type l) {
        return symbol_type(token::CONST_EXPR, std::move(l));
    }
#else
    static symbol_type make_CONST_EXPR(const location_type& l) {
        return symbol_type(token::CONST_EXPR, l);
    }
#endif
#if 201103L <= YY_CPLUSPLUS
    static symbol_type make_LITERAL(location_type l) {
        return symbol_type(token::LITERAL, std::move(l));
    }
#else
    static symbol_type make_LITERAL(const location_type& l) {
        return symbol_type(token::LITERAL, l);
    }
#endif
#if 201103L <= YY_CPLUSPLUS
    static symbol_type make_OR(location_type l) {
        return symbol_type(token::OR, std::move(l));
    }
#else
    static symbol_type make_OR(const location_type& l) {
        return symbol_type(token::OR, l);
    }
#endif
#if 201103L <= YY_CPLUSPLUS
    static symbol_type make_NOT(location_type l) {
        return symbol_type(token::NOT, std::move(l));
    }
#else
    static symbol_type make_NOT(const location_type& l) {
        return symbol_type(token::NOT, l);
    }
#endif
#if 201103L <= YY_CPLUSPLUS
    static symbol_type make_CMP(location_type l) {
        return symbol_type(token::CMP, std::move(l));
    }
#else
    static symbol_type make_CMP(const location_type& l) {
        return symbol_type(token::CMP, l);
    }
#endif
#if 201103L <= YY_CPLUSPLUS
    static symbol_type make_EQ(location_type l) {
        return symbol_type(token::EQ, std::move(l));
    }
#else
    static symbol_type make_EQ(const location_type& l) {
        return symbol_type(token::EQ, l);
    }
#endif
#if 201103L <= YY_CPLUSPLUS
    static symbol_type make_GT(location_type l) {
        return symbol_type(token::GT, std::move(l));
    }
#else
    static symbol_type make_GT(const location_type& l) {
        return symbol_type(token::GT, l);
    }
#endif
#if 201103L <= YY_CPLUSPLUS
    static symbol_type make_GTE(location_type l) {
        return symbol_type(token::GTE, std::move(l));
    }
#else
    static symbol_type make_GTE(const location_type& l) {
        return symbol_type(token::GTE, l);
    }
#endif
#if 201103L <= YY_CPLUSPLUS
    static symbol_type make_LT(location_type l) {
        return symbol_type(token::LT, std::move(l));
    }
#else
    static symbol_type make_LT(const location_type& l) {
        return symbol_type(token::LT, l);
    }
#endif
#if 201103L <= YY_CPLUSPLUS
    static symbol_type make_LTE(location_type l) {
        return symbol_type(token::LTE, std::move(l));
    }
#else
    static symbol_type make_LTE(const location_type& l) {
        return symbol_type(token::LTE, l);
    }
#endif
#if 201103L <= YY_CPLUSPLUS
    static symbol_type make_NE(location_type l) {
        return symbol_type(token::NE, std::move(l));
    }
#else
    static symbol_type make_NE(const location_type& l) {
        return symbol_type(token::NE, l);
    }
#endif
#if 201103L <= YY_CPLUSPLUS
    static symbol_type make_FIELDNAME(std::string v, location_type l) {
        return symbol_type(token::FIELDNAME, std::move(v), std::move(l));
    }
#else
    static symbol_type make_FIELDNAME(const std::string& v, const location_type& l) {
        return symbol_type(token::FIELDNAME, v, l);
    }
#endif
#if 201103L <= YY_CPLUSPLUS
    static symbol_type make_STRING(std::string v, location_type l) {
        return symbol_type(token::STRING, std::move(v), std::move(l));
    }
#else
    static symbol_type make_STRING(const std::string& v, const location_type& l) {
        return symbol_type(token::STRING, v, l);
    }
#endif
#if 201103L <= YY_CPLUSPLUS
    static symbol_type make_BINARY(BSONBinData v, location_type l) {
        return symbol_type(token::BINARY, std::move(v), std::move(l));
    }
#else
    static symbol_type make_BINARY(const BSONBinData& v, const location_type& l) {
        return symbol_type(token::BINARY, v, l);
    }
#endif
#if 201103L <= YY_CPLUSPLUS
    static symbol_type make_UNDEFINED(UserUndefined v, location_type l) {
        return symbol_type(token::UNDEFINED, std::move(v), std::move(l));
    }
#else
    static symbol_type make_UNDEFINED(const UserUndefined& v, const location_type& l) {
        return symbol_type(token::UNDEFINED, v, l);
    }
#endif
#if 201103L <= YY_CPLUSPLUS
    static symbol_type make_OBJECT_ID(OID v, location_type l) {
        return symbol_type(token::OBJECT_ID, std::move(v), std::move(l));
    }
#else
    static symbol_type make_OBJECT_ID(const OID& v, const location_type& l) {
        return symbol_type(token::OBJECT_ID, v, l);
    }
#endif
#if 201103L <= YY_CPLUSPLUS
    static symbol_type make_DATE_LITERAL(Date_t v, location_type l) {
        return symbol_type(token::DATE_LITERAL, std::move(v), std::move(l));
    }
#else
    static symbol_type make_DATE_LITERAL(const Date_t& v, const location_type& l) {
        return symbol_type(token::DATE_LITERAL, v, l);
    }
#endif
#if 201103L <= YY_CPLUSPLUS
    static symbol_type make_JSNULL(UserNull v, location_type l) {
        return symbol_type(token::JSNULL, std::move(v), std::move(l));
    }
#else
    static symbol_type make_JSNULL(const UserNull& v, const location_type& l) {
        return symbol_type(token::JSNULL, v, l);
    }
#endif
#if 201103L <= YY_CPLUSPLUS
    static symbol_type make_REGEX(BSONRegEx v, location_type l) {
        return symbol_type(token::REGEX, std::move(v), std::move(l));
    }
#else
    static symbol_type make_REGEX(const BSONRegEx& v, const location_type& l) {
        return symbol_type(token::REGEX, v, l);
    }
#endif
#if 201103L <= YY_CPLUSPLUS
    static symbol_type make_DB_POINTER(BSONDBRef v, location_type l) {
        return symbol_type(token::DB_POINTER, std::move(v), std::move(l));
    }
#else
    static symbol_type make_DB_POINTER(const BSONDBRef& v, const location_type& l) {
        return symbol_type(token::DB_POINTER, v, l);
    }
#endif
#if 201103L <= YY_CPLUSPLUS
    static symbol_type make_JAVASCRIPT(BSONCode v, location_type l) {
        return symbol_type(token::JAVASCRIPT, std::move(v), std::move(l));
    }
#else
    static symbol_type make_JAVASCRIPT(const BSONCode& v, const location_type& l) {
        return symbol_type(token::JAVASCRIPT, v, l);
    }
#endif
#if 201103L <= YY_CPLUSPLUS
    static symbol_type make_SYMBOL(BSONSymbol v, location_type l) {
        return symbol_type(token::SYMBOL, std::move(v), std::move(l));
    }
#else
    static symbol_type make_SYMBOL(const BSONSymbol& v, const location_type& l) {
        return symbol_type(token::SYMBOL, v, l);
    }
#endif
#if 201103L <= YY_CPLUSPLUS
    static symbol_type make_JAVASCRIPT_W_SCOPE(BSONCodeWScope v, location_type l) {
        return symbol_type(token::JAVASCRIPT_W_SCOPE, std::move(v), std::move(l));
    }
#else
    static symbol_type make_JAVASCRIPT_W_SCOPE(const BSONCodeWScope& v, const location_type& l) {
        return symbol_type(token::JAVASCRIPT_W_SCOPE, v, l);
    }
#endif
#if 201103L <= YY_CPLUSPLUS
    static symbol_type make_INT_NON_ZERO(int v, location_type l) {
        return symbol_type(token::INT_NON_ZERO, std::move(v), std::move(l));
    }
#else
    static symbol_type make_INT_NON_ZERO(const int& v, const location_type& l) {
        return symbol_type(token::INT_NON_ZERO, v, l);
    }
#endif
#if 201103L <= YY_CPLUSPLUS
    static symbol_type make_TIMESTAMP(Timestamp v, location_type l) {
        return symbol_type(token::TIMESTAMP, std::move(v), std::move(l));
    }
#else
    static symbol_type make_TIMESTAMP(const Timestamp& v, const location_type& l) {
        return symbol_type(token::TIMESTAMP, v, l);
    }
#endif
#if 201103L <= YY_CPLUSPLUS
    static symbol_type make_LONG_NON_ZERO(long long v, location_type l) {
        return symbol_type(token::LONG_NON_ZERO, std::move(v), std::move(l));
    }
#else
    static symbol_type make_LONG_NON_ZERO(const long long& v, const location_type& l) {
        return symbol_type(token::LONG_NON_ZERO, v, l);
    }
#endif
#if 201103L <= YY_CPLUSPLUS
    static symbol_type make_DOUBLE_NON_ZERO(double v, location_type l) {
        return symbol_type(token::DOUBLE_NON_ZERO, std::move(v), std::move(l));
    }
#else
    static symbol_type make_DOUBLE_NON_ZERO(const double& v, const location_type& l) {
        return symbol_type(token::DOUBLE_NON_ZERO, v, l);
    }
#endif
#if 201103L <= YY_CPLUSPLUS
    static symbol_type make_DECIMAL_NON_ZERO(Decimal128 v, location_type l) {
        return symbol_type(token::DECIMAL_NON_ZERO, std::move(v), std::move(l));
    }
#else
    static symbol_type make_DECIMAL_NON_ZERO(const Decimal128& v, const location_type& l) {
        return symbol_type(token::DECIMAL_NON_ZERO, v, l);
    }
#endif
#if 201103L <= YY_CPLUSPLUS
    static symbol_type make_MIN_KEY(UserMinKey v, location_type l) {
        return symbol_type(token::MIN_KEY, std::move(v), std::move(l));
    }
#else
    static symbol_type make_MIN_KEY(const UserMinKey& v, const location_type& l) {
        return symbol_type(token::MIN_KEY, v, l);
    }
#endif
#if 201103L <= YY_CPLUSPLUS
    static symbol_type make_MAX_KEY(UserMaxKey v, location_type l) {
        return symbol_type(token::MAX_KEY, std::move(v), std::move(l));
    }
#else
    static symbol_type make_MAX_KEY(const UserMaxKey& v, const location_type& l) {
        return symbol_type(token::MAX_KEY, v, l);
    }
#endif


private:
    /// This class is not copyable.
    PipelineParserGen(const PipelineParserGen&);
    PipelineParserGen& operator=(const PipelineParserGen&);

    /// Stored state numbers (used for stacks).
    typedef unsigned char state_type;

    /// Generate an error message.
    /// \param yystate   the state where the error occurred.
    /// \param yyla      the lookahead token.
    virtual std::string yysyntax_error_(state_type yystate, const symbol_type& yyla) const;

    /// Compute post-reduction state.
    /// \param yystate   the current state
    /// \param yysym     the nonterminal to push on the stack
    static state_type yy_lr_goto_state_(state_type yystate, int yysym);

    /// Whether the given \c yypact_ value indicates a defaulted state.
    /// \param yyvalue   the value to check
    static bool yy_pact_value_is_default_(int yyvalue);

    /// Whether the given \c yytable_ value indicates a syntax error.
    /// \param yyvalue   the value to check
    static bool yy_table_value_is_error_(int yyvalue);

    static const short yypact_ninf_;
    static const signed char yytable_ninf_;

    /// Convert a scanner token number \a t to a symbol number.
    /// In theory \a t should be a token_type, but character literals
    /// are valid, yet not members of the token_type enum.
    static token_number_type yytranslate_(int t);

    // Tables.
    // YYPACT[STATE-NUM] -- Index in YYTABLE of the portion describing
    // STATE-NUM.
    static const short yypact_[];

    // YYDEFACT[STATE-NUM] -- Default reduction number in state STATE-NUM.
    // Performed when YYTABLE does not specify something else to do.  Zero
    // means the default is an error.
    static const unsigned char yydefact_[];

    // YYPGOTO[NTERM-NUM].
    static const short yypgoto_[];

    // YYDEFGOTO[NTERM-NUM].
    static const short yydefgoto_[];

    // YYTABLE[YYPACT[STATE-NUM]] -- What to do in state STATE-NUM.  If
    // positive, shift that token.  If negative, reduce the rule whose
    // number is the opposite.  If YYTABLE_NINF, syntax error.
    static const unsigned char yytable_[];

    static const short yycheck_[];

    // YYSTOS[STATE-NUM] -- The (internal number of the) accessing
    // symbol of state STATE-NUM.
    static const unsigned char yystos_[];

    // YYR1[YYN] -- Symbol number of symbol that rule YYN derives.
    static const unsigned char yyr1_[];

    // YYR2[YYN] -- Number of symbols on the right hand side of rule YYN.
    static const signed char yyr2_[];


#if YYDEBUG
    /// For a symbol, its name in clear.
    static const char* const yytname_[];

    // YYRLINE[YYN] -- Source line where rule number YYN was defined.
    static const short yyrline_[];
    /// Report on the debug stream that the rule \a r is going to be reduced.
    virtual void yy_reduce_print_(int r);
    /// Print the state stack on the debug stream.
    virtual void yystack_print_();

    /// Debugging level.
    int yydebug_;
    /// Debug stream.
    std::ostream* yycdebug_;

    /// \brief Display a symbol type, value and location.
    /// \param yyo    The output stream.
    /// \param yysym  The symbol.
    template <typename Base>
    void yy_print_(std::ostream& yyo, const basic_symbol<Base>& yysym) const;
#endif

    /// \brief Reclaim the memory associated to a symbol.
    /// \param yymsg     Why this token is reclaimed.
    ///                  If null, print nothing.
    /// \param yysym     The symbol.
    template <typename Base>
    void yy_destroy_(const char* yymsg, basic_symbol<Base>& yysym) const;

private:
    /// Type access provider for state based symbols.
    struct by_state {
        /// Default constructor.
        by_state() YY_NOEXCEPT;

        /// The symbol type as needed by the constructor.
        typedef state_type kind_type;

        /// Constructor.
        by_state(kind_type s) YY_NOEXCEPT;

        /// Copy constructor.
        by_state(const by_state& that) YY_NOEXCEPT;

        /// Record that this symbol is empty.
        void clear() YY_NOEXCEPT;

        /// Steal the symbol type from \a that.
        void move(by_state& that);

        /// The (internal) type number (corresponding to \a state).
        /// \a empty_symbol when empty.
        symbol_number_type type_get() const YY_NOEXCEPT;

        /// The state number used to denote an empty symbol.
        /// We use the initial state, as it does not have a value.
        enum { empty_state = 0 };

        /// The state.
        /// \a empty when empty.
        state_type state;
    };

    /// "Internal" symbol: element of the stack.
    struct stack_symbol_type : basic_symbol<by_state> {
        /// Superclass.
        typedef basic_symbol<by_state> super_type;
        /// Construct an empty symbol.
        stack_symbol_type();
        /// Move or copy construction.
        stack_symbol_type(YY_RVREF(stack_symbol_type) that);
        /// Steal the contents from \a sym to build this.
        stack_symbol_type(state_type s, YY_MOVE_REF(symbol_type) sym);
#if YY_CPLUSPLUS < 201103L
        /// Assignment, needed by push_back by some old implementations.
        /// Moves the contents of that.
        stack_symbol_type& operator=(stack_symbol_type& that);

        /// Assignment, needed by push_back by other implementations.
        /// Needed by some other old implementations.
        stack_symbol_type& operator=(const stack_symbol_type& that);
#endif
    };

    /// A stack with random access from its top.
    template <typename T, typename S = std::vector<T>>
    class stack {
    public:
        // Hide our reversed order.
        typedef typename S::reverse_iterator iterator;
        typedef typename S::const_reverse_iterator const_iterator;
        typedef typename S::size_type size_type;
        typedef typename std::ptrdiff_t index_type;

        stack(size_type n = 200) : seq_(n) {}

        /// Random access.
        ///
        /// Index 0 returns the topmost element.
        const T& operator[](index_type i) const {
            return seq_[size_type(size() - 1 - i)];
        }

        /// Random access.
        ///
        /// Index 0 returns the topmost element.
        T& operator[](index_type i) {
            return seq_[size_type(size() - 1 - i)];
        }

        /// Steal the contents of \a t.
        ///
        /// Close to move-semantics.
        void push(YY_MOVE_REF(T) t) {
            seq_.push_back(T());
            operator[](0).move(t);
        }

        /// Pop elements from the stack.
        void pop(std::ptrdiff_t n = 1) YY_NOEXCEPT {
            for (; 0 < n; --n)
                seq_.pop_back();
        }

        /// Pop all elements from the stack.
        void clear() YY_NOEXCEPT {
            seq_.clear();
        }

        /// Number of elements on the stack.
        index_type size() const YY_NOEXCEPT {
            return index_type(seq_.size());
        }

        std::ptrdiff_t ssize() const YY_NOEXCEPT {
            return std::ptrdiff_t(size());
        }

        /// Iterator on top of the stack (going downwards).
        const_iterator begin() const YY_NOEXCEPT {
            return seq_.rbegin();
        }

        /// Bottom of the stack.
        const_iterator end() const YY_NOEXCEPT {
            return seq_.rend();
        }

        /// Present a slice of the top of a stack.
        class slice {
        public:
            slice(const stack& stack, index_type range) : stack_(stack), range_(range) {}

            const T& operator[](index_type i) const {
                return stack_[range_ - i];
            }

        private:
            const stack& stack_;
            index_type range_;
        };

    private:
        stack(const stack&);
        stack& operator=(const stack&);
        /// The wrapped container.
        S seq_;
    };


    /// Stack type.
    typedef stack<stack_symbol_type> stack_type;

    /// The stack.
    stack_type yystack_;

    /// Push a new state on the stack.
    /// \param m    a debug message to display
    ///             if null, no trace is output.
    /// \param sym  the symbol
    /// \warning the contents of \a s.value is stolen.
    void yypush_(const char* m, YY_MOVE_REF(stack_symbol_type) sym);

    /// Push a new look ahead token on the state on the stack.
    /// \param m    a debug message to display
    ///             if null, no trace is output.
    /// \param s    the state
    /// \param sym  the symbol (for its value and location).
    /// \warning the contents of \a sym.value is stolen.
    void yypush_(const char* m, state_type s, YY_MOVE_REF(symbol_type) sym);

    /// Pop \a n symbols from the stack.
    void yypop_(int n = 1);

    /// Some specific tokens.
    static const token_number_type yy_error_token_ = 1;
    static const token_number_type yy_undef_token_ = 2;

    /// Constants.
    enum {
        yyeof_ = 0,
        yylast_ = 292,   ///< Last index in yytable_.
        yynnts_ = 77,    ///< Number of nonterminal symbols.
        yyfinal_ = 5,    ///< Termination state number.
        yyntokens_ = 56  ///< Number of tokens.
    };


    // User arguments.
    BSONLexer& lexer;
    CNode* cst;
};

inline PipelineParserGen::token_number_type PipelineParserGen::yytranslate_(int t) {
    return static_cast<token_number_type>(t);
}

// basic_symbol.
#if 201103L <= YY_CPLUSPLUS
template <typename Base>
PipelineParserGen::basic_symbol<Base>::basic_symbol(basic_symbol&& that)
    : Base(std::move(that)), value(), location(std::move(that.location)) {
    switch (this->type_get()) {
        case 39:  // BINARY
            value.move<BSONBinData>(std::move(that.value));
            break;

        case 46:  // JAVASCRIPT
            value.move<BSONCode>(std::move(that.value));
            break;

        case 48:  // JAVASCRIPT_W_SCOPE
            value.move<BSONCodeWScope>(std::move(that.value));
            break;

        case 45:  // DB_POINTER
            value.move<BSONDBRef>(std::move(that.value));
            break;

        case 44:  // REGEX
            value.move<BSONRegEx>(std::move(that.value));
            break;

        case 47:  // SYMBOL
            value.move<BSONSymbol>(std::move(that.value));
            break;

        case 68:   // dbPointer
        case 69:   // javascript
        case 70:   // symbol
        case 71:   // javascriptWScope
        case 72:   // int
        case 73:   // timestamp
        case 74:   // long
        case 75:   // double
        case 76:   // decimal
        case 77:   // minKey
        case 78:   // maxKey
        case 79:   // value
        case 80:   // string
        case 81:   // binary
        case 82:   // undefined
        case 83:   // objectId
        case 84:   // bool
        case 85:   // date
        case 86:   // null
        case 87:   // regex
        case 88:   // simpleValue
        case 89:   // compoundValue
        case 90:   // valueArray
        case 91:   // valueObject
        case 92:   // valueFields
        case 93:   // stageList
        case 94:   // stage
        case 95:   // inhibitOptimization
        case 96:   // unionWith
        case 97:   // skip
        case 98:   // limit
        case 99:   // project
        case 100:  // sample
        case 101:  // projectFields
        case 102:  // projection
        case 103:  // num
        case 104:  // expression
        case 105:  // compoundExpression
        case 106:  // exprFixedTwoArg
        case 107:  // expressionArray
        case 108:  // expressionObject
        case 109:  // expressionFields
        case 110:  // maths
        case 111:  // add
        case 112:  // atan2
        case 113:  // boolExps
        case 114:  // and
        case 115:  // or
        case 116:  // not
        case 117:  // literalEscapes
        case 118:  // const
        case 119:  // literal
        case 120:  // compExprs
        case 121:  // cmp
        case 122:  // eq
        case 123:  // gt
        case 124:  // gte
        case 125:  // lt
        case 126:  // lte
        case 127:  // ne
            value.move<CNode>(std::move(that.value));
            break;

        case 57:  // projectionFieldname
        case 58:  // expressionFieldname
        case 59:  // stageAsUserFieldname
        case 60:  // argAsUserFieldname
        case 61:  // aggExprAsUserFieldname
        case 62:  // invariableUserFieldname
        case 63:  // idAsUserFieldname
        case 64:  // valueFieldname
            value.move<CNode::Fieldname>(std::move(that.value));
            break;

        case 42:  // DATE_LITERAL
            value.move<Date_t>(std::move(that.value));
            break;

        case 53:  // DECIMAL_NON_ZERO
            value.move<Decimal128>(std::move(that.value));
            break;

        case 41:  // OBJECT_ID
            value.move<OID>(std::move(that.value));
            break;

        case 50:  // TIMESTAMP
            value.move<Timestamp>(std::move(that.value));
            break;

        case 55:  // MAX_KEY
            value.move<UserMaxKey>(std::move(that.value));
            break;

        case 54:  // MIN_KEY
            value.move<UserMinKey>(std::move(that.value));
            break;

        case 43:  // JSNULL
            value.move<UserNull>(std::move(that.value));
            break;

        case 40:  // UNDEFINED
            value.move<UserUndefined>(std::move(that.value));
            break;

        case 52:  // DOUBLE_NON_ZERO
            value.move<double>(std::move(that.value));
            break;

        case 49:  // INT_NON_ZERO
            value.move<int>(std::move(that.value));
            break;

        case 51:  // LONG_NON_ZERO
            value.move<long long>(std::move(that.value));
            break;

        case 65:  // projectField
        case 66:  // expressionField
        case 67:  // valueField
            value.move<std::pair<CNode::Fieldname, CNode>>(std::move(that.value));
            break;

        case 37:  // FIELDNAME
        case 38:  // STRING
            value.move<std::string>(std::move(that.value));
            break;

        case 128:  // expressions
        case 129:  // values
            value.move<std::vector<CNode>>(std::move(that.value));
            break;

        default:
            break;
    }
}
#endif

template <typename Base>
PipelineParserGen::basic_symbol<Base>::basic_symbol(const basic_symbol& that)
    : Base(that), value(), location(that.location) {
    switch (this->type_get()) {
        case 39:  // BINARY
            value.copy<BSONBinData>(YY_MOVE(that.value));
            break;

        case 46:  // JAVASCRIPT
            value.copy<BSONCode>(YY_MOVE(that.value));
            break;

        case 48:  // JAVASCRIPT_W_SCOPE
            value.copy<BSONCodeWScope>(YY_MOVE(that.value));
            break;

        case 45:  // DB_POINTER
            value.copy<BSONDBRef>(YY_MOVE(that.value));
            break;

        case 44:  // REGEX
            value.copy<BSONRegEx>(YY_MOVE(that.value));
            break;

        case 47:  // SYMBOL
            value.copy<BSONSymbol>(YY_MOVE(that.value));
            break;

        case 68:   // dbPointer
        case 69:   // javascript
        case 70:   // symbol
        case 71:   // javascriptWScope
        case 72:   // int
        case 73:   // timestamp
        case 74:   // long
        case 75:   // double
        case 76:   // decimal
        case 77:   // minKey
        case 78:   // maxKey
        case 79:   // value
        case 80:   // string
        case 81:   // binary
        case 82:   // undefined
        case 83:   // objectId
        case 84:   // bool
        case 85:   // date
        case 86:   // null
        case 87:   // regex
        case 88:   // simpleValue
        case 89:   // compoundValue
        case 90:   // valueArray
        case 91:   // valueObject
        case 92:   // valueFields
        case 93:   // stageList
        case 94:   // stage
        case 95:   // inhibitOptimization
        case 96:   // unionWith
        case 97:   // skip
        case 98:   // limit
        case 99:   // project
        case 100:  // sample
        case 101:  // projectFields
        case 102:  // projection
        case 103:  // num
        case 104:  // expression
        case 105:  // compoundExpression
        case 106:  // exprFixedTwoArg
        case 107:  // expressionArray
        case 108:  // expressionObject
        case 109:  // expressionFields
        case 110:  // maths
        case 111:  // add
        case 112:  // atan2
        case 113:  // boolExps
        case 114:  // and
        case 115:  // or
        case 116:  // not
        case 117:  // literalEscapes
        case 118:  // const
        case 119:  // literal
        case 120:  // compExprs
        case 121:  // cmp
        case 122:  // eq
        case 123:  // gt
        case 124:  // gte
        case 125:  // lt
        case 126:  // lte
        case 127:  // ne
            value.copy<CNode>(YY_MOVE(that.value));
            break;

        case 57:  // projectionFieldname
        case 58:  // expressionFieldname
        case 59:  // stageAsUserFieldname
        case 60:  // argAsUserFieldname
        case 61:  // aggExprAsUserFieldname
        case 62:  // invariableUserFieldname
        case 63:  // idAsUserFieldname
        case 64:  // valueFieldname
            value.copy<CNode::Fieldname>(YY_MOVE(that.value));
            break;

        case 42:  // DATE_LITERAL
            value.copy<Date_t>(YY_MOVE(that.value));
            break;

        case 53:  // DECIMAL_NON_ZERO
            value.copy<Decimal128>(YY_MOVE(that.value));
            break;

        case 41:  // OBJECT_ID
            value.copy<OID>(YY_MOVE(that.value));
            break;

        case 50:  // TIMESTAMP
            value.copy<Timestamp>(YY_MOVE(that.value));
            break;

        case 55:  // MAX_KEY
            value.copy<UserMaxKey>(YY_MOVE(that.value));
            break;

        case 54:  // MIN_KEY
            value.copy<UserMinKey>(YY_MOVE(that.value));
            break;

        case 43:  // JSNULL
            value.copy<UserNull>(YY_MOVE(that.value));
            break;

        case 40:  // UNDEFINED
            value.copy<UserUndefined>(YY_MOVE(that.value));
            break;

        case 52:  // DOUBLE_NON_ZERO
            value.copy<double>(YY_MOVE(that.value));
            break;

        case 49:  // INT_NON_ZERO
            value.copy<int>(YY_MOVE(that.value));
            break;

        case 51:  // LONG_NON_ZERO
            value.copy<long long>(YY_MOVE(that.value));
            break;

        case 65:  // projectField
        case 66:  // expressionField
        case 67:  // valueField
            value.copy<std::pair<CNode::Fieldname, CNode>>(YY_MOVE(that.value));
            break;

        case 37:  // FIELDNAME
        case 38:  // STRING
            value.copy<std::string>(YY_MOVE(that.value));
            break;

        case 128:  // expressions
        case 129:  // values
            value.copy<std::vector<CNode>>(YY_MOVE(that.value));
            break;

        default:
            break;
    }
}


template <typename Base>
bool PipelineParserGen::basic_symbol<Base>::empty() const YY_NOEXCEPT {
    return Base::type_get() == empty_symbol;
}

template <typename Base>
void PipelineParserGen::basic_symbol<Base>::move(basic_symbol& s) {
    super_type::move(s);
    switch (this->type_get()) {
        case 39:  // BINARY
            value.move<BSONBinData>(YY_MOVE(s.value));
            break;

        case 46:  // JAVASCRIPT
            value.move<BSONCode>(YY_MOVE(s.value));
            break;

        case 48:  // JAVASCRIPT_W_SCOPE
            value.move<BSONCodeWScope>(YY_MOVE(s.value));
            break;

        case 45:  // DB_POINTER
            value.move<BSONDBRef>(YY_MOVE(s.value));
            break;

        case 44:  // REGEX
            value.move<BSONRegEx>(YY_MOVE(s.value));
            break;

        case 47:  // SYMBOL
            value.move<BSONSymbol>(YY_MOVE(s.value));
            break;

        case 68:   // dbPointer
        case 69:   // javascript
        case 70:   // symbol
        case 71:   // javascriptWScope
        case 72:   // int
        case 73:   // timestamp
        case 74:   // long
        case 75:   // double
        case 76:   // decimal
        case 77:   // minKey
        case 78:   // maxKey
        case 79:   // value
        case 80:   // string
        case 81:   // binary
        case 82:   // undefined
        case 83:   // objectId
        case 84:   // bool
        case 85:   // date
        case 86:   // null
        case 87:   // regex
        case 88:   // simpleValue
        case 89:   // compoundValue
        case 90:   // valueArray
        case 91:   // valueObject
        case 92:   // valueFields
        case 93:   // stageList
        case 94:   // stage
        case 95:   // inhibitOptimization
        case 96:   // unionWith
        case 97:   // skip
        case 98:   // limit
        case 99:   // project
        case 100:  // sample
        case 101:  // projectFields
        case 102:  // projection
        case 103:  // num
        case 104:  // expression
        case 105:  // compoundExpression
        case 106:  // exprFixedTwoArg
        case 107:  // expressionArray
        case 108:  // expressionObject
        case 109:  // expressionFields
        case 110:  // maths
        case 111:  // add
        case 112:  // atan2
        case 113:  // boolExps
        case 114:  // and
        case 115:  // or
        case 116:  // not
        case 117:  // literalEscapes
        case 118:  // const
        case 119:  // literal
        case 120:  // compExprs
        case 121:  // cmp
        case 122:  // eq
        case 123:  // gt
        case 124:  // gte
        case 125:  // lt
        case 126:  // lte
        case 127:  // ne
            value.move<CNode>(YY_MOVE(s.value));
            break;

        case 57:  // projectionFieldname
        case 58:  // expressionFieldname
        case 59:  // stageAsUserFieldname
        case 60:  // argAsUserFieldname
        case 61:  // aggExprAsUserFieldname
        case 62:  // invariableUserFieldname
        case 63:  // idAsUserFieldname
        case 64:  // valueFieldname
            value.move<CNode::Fieldname>(YY_MOVE(s.value));
            break;

        case 42:  // DATE_LITERAL
            value.move<Date_t>(YY_MOVE(s.value));
            break;

        case 53:  // DECIMAL_NON_ZERO
            value.move<Decimal128>(YY_MOVE(s.value));
            break;

        case 41:  // OBJECT_ID
            value.move<OID>(YY_MOVE(s.value));
            break;

        case 50:  // TIMESTAMP
            value.move<Timestamp>(YY_MOVE(s.value));
            break;

        case 55:  // MAX_KEY
            value.move<UserMaxKey>(YY_MOVE(s.value));
            break;

        case 54:  // MIN_KEY
            value.move<UserMinKey>(YY_MOVE(s.value));
            break;

        case 43:  // JSNULL
            value.move<UserNull>(YY_MOVE(s.value));
            break;

        case 40:  // UNDEFINED
            value.move<UserUndefined>(YY_MOVE(s.value));
            break;

        case 52:  // DOUBLE_NON_ZERO
            value.move<double>(YY_MOVE(s.value));
            break;

        case 49:  // INT_NON_ZERO
            value.move<int>(YY_MOVE(s.value));
            break;

        case 51:  // LONG_NON_ZERO
            value.move<long long>(YY_MOVE(s.value));
            break;

        case 65:  // projectField
        case 66:  // expressionField
        case 67:  // valueField
            value.move<std::pair<CNode::Fieldname, CNode>>(YY_MOVE(s.value));
            break;

        case 37:  // FIELDNAME
        case 38:  // STRING
            value.move<std::string>(YY_MOVE(s.value));
            break;

        case 128:  // expressions
        case 129:  // values
            value.move<std::vector<CNode>>(YY_MOVE(s.value));
            break;

        default:
            break;
    }

    location = YY_MOVE(s.location);
}

// by_type.
inline PipelineParserGen::by_type::by_type() : type(empty_symbol) {}

#if 201103L <= YY_CPLUSPLUS
inline PipelineParserGen::by_type::by_type(by_type&& that) : type(that.type) {
    that.clear();
}
#endif

inline PipelineParserGen::by_type::by_type(const by_type& that) : type(that.type) {}

inline PipelineParserGen::by_type::by_type(token_type t) : type(yytranslate_(t)) {}

inline void PipelineParserGen::by_type::clear() {
    type = empty_symbol;
}

inline void PipelineParserGen::by_type::move(by_type& that) {
    type = that.type;
    that.clear();
}

inline int PipelineParserGen::by_type::type_get() const YY_NOEXCEPT {
    return type;
}

#line 58 "src/mongo/db/cst/pipeline_grammar.yy"
}  // namespace mongo
#line 3224 "src/mongo/db/cst/pipeline_parser_gen.hpp"


#endif  // !YY_YY_SRC_MONGO_DB_CST_PIPELINE_PARSER_GEN_HPP_INCLUDED
