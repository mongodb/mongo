// A Bison parser, made by GNU Bison 3.5.

// Skeleton interface for Bison LALR(1) parsers in C++

// Copyright (C) 2002-2015, 2018-2019 Free Software Foundation, Inc.

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
 ** \file src/mongo/db/cst/parser_gen.hpp
 ** Define the mongo::parser class.
 */

// C++ LALR(1) parser skeleton written by Akim Demaille.

// Undocumented macros, especially those whose name start with YY_,
// are private implementation details.  Do not rely on them.

#ifndef YY_YY_SRC_MONGO_DB_CST_PARSER_GEN_HPP_INCLUDED
#define YY_YY_SRC_MONGO_DB_CST_PARSER_GEN_HPP_INCLUDED
// "%code requires" blocks.
#line 66 "src/mongo/db/cst/grammar.yy"

#include "mongo/db/cst/bson_location.h"
#include "mongo/db/cst/c_node.h"

// Forward declare any parameters needed for lexing/parsing.
namespace mongo {
class BSONLexer;
}

#ifdef _MSC_VER
// warning C4065: switch statement contains 'default' but no 'case' labels.
#pragma warning(disable : 4065)
#endif

#line 63 "src/mongo/db/cst/parser_gen.hpp"

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

#line 57 "src/mongo/db/cst/grammar.yy"
namespace mongo {
#line 198 "src/mongo/db/cst/parser_gen.hpp"


/// A Bison parser.
class ParserGen {
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
            // "BinData"
            char dummy1[sizeof(BSONBinData)];

            // "Code"
            char dummy2[sizeof(BSONCode)];

            // "CodeWScope"
            char dummy3[sizeof(BSONCodeWScope)];

            // "dbPointer"
            char dummy4[sizeof(BSONDBRef)];

            // "regex"
            char dummy5[sizeof(BSONRegEx)];

            // "Symbol"
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
            // aggregationFieldPath
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
            // variable
            // typeArray
            // typeValue
            // pipeline
            // stageList
            // stage
            // inhibitOptimization
            // unionWith
            // skip
            // limit
            // matchStage
            // project
            // sample
            // aggregationProjectFields
            // aggregationProjectionObjectFields
            // topLevelAggregationProjection
            // aggregationProjection
            // projectionCommon
            // aggregationProjectionObject
            // num
            // expression
            // exprFixedTwoArg
            // exprFixedThreeArg
            // slice
            // expressionArray
            // expressionObject
            // expressionFields
            // maths
            // meta
            // add
            // boolExprs
            // and
            // or
            // not
            // literalEscapes
            // const
            // literal
            // stringExps
            // concat
            // dateFromString
            // dateToString
            // indexOfBytes
            // indexOfCP
            // ltrim
            // regexFind
            // regexFindAll
            // regexMatch
            // regexArgs
            // replaceOne
            // replaceAll
            // rtrim
            // split
            // strLenBytes
            // strLenCP
            // strcasecmp
            // substr
            // substrBytes
            // substrCP
            // toLower
            // toUpper
            // trim
            // compExprs
            // cmp
            // eq
            // gt
            // gte
            // lt
            // lte
            // ne
            // dateExps
            // dateFromParts
            // dateToParts
            // dayOfMonth
            // dayOfWeek
            // dayOfYear
            // hour
            // isoDayOfWeek
            // isoWeek
            // isoWeekYear
            // millisecond
            // minute
            // month
            // second
            // week
            // year
            // typeExpression
            // convert
            // toBool
            // toDate
            // toDecimal
            // toDouble
            // toInt
            // toLong
            // toObjectId
            // toString
            // type
            // abs
            // ceil
            // divide
            // exponent
            // floor
            // ln
            // log
            // logten
            // mod
            // multiply
            // pow
            // round
            // sqrt
            // subtract
            // trunc
            // setExpression
            // allElementsTrue
            // anyElementTrue
            // setDifference
            // setEquals
            // setIntersection
            // setIsSubset
            // setUnion
            // trig
            // sin
            // cos
            // tan
            // sinh
            // cosh
            // tanh
            // asin
            // acos
            // atan
            // asinh
            // acosh
            // atanh
            // atan2
            // degreesToRadians
            // radiansToDegrees
            // nonArrayExpression
            // nonArrayCompoundExpression
            // aggregationOperator
            // aggregationOperatorWithoutSlice
            // expressionSingletonArray
            // singleArgExpression
            // nonArrayNonObjExpression
            // matchExpression
            // predicates
            // compoundMatchExprs
            // predValue
            // additionalExprs
            // textArgCaseSensitive
            // textArgDiacriticSensitive
            // textArgLanguage
            // textArgSearch
            // findProject
            // findProjectFields
            // topLevelFindProjection
            // findProjection
            // findProjectionSlice
            // elemMatch
            // findProjectionObject
            // findProjectionObjectFields
            // sortSpecs
            // specList
            // metaSort
            // oneOrNegOne
            // metaSortKeyword
            char dummy7[sizeof(CNode)];

            // aggregationProjectionFieldname
            // projectionFieldname
            // expressionFieldname
            // stageAsUserFieldname
            // argAsUserFieldname
            // argAsProjectionPath
            // aggExprAsUserFieldname
            // invariableUserFieldname
            // sortFieldname
            // idAsUserFieldname
            // elemMatchAsUserFieldname
            // idAsProjectionPath
            // valueFieldname
            // predFieldname
            // logicalExprField
            char dummy8[sizeof(CNode::Fieldname)];

            // "Date"
            char dummy9[sizeof(Date_t)];

            // "arbitrary decimal"
            char dummy10[sizeof(Decimal128)];

            // "ObjectID"
            char dummy11[sizeof(OID)];

            // "Timestamp"
            char dummy12[sizeof(Timestamp)];

            // "maxKey"
            char dummy13[sizeof(UserMaxKey)];

            // "minKey"
            char dummy14[sizeof(UserMinKey)];

            // "null"
            char dummy15[sizeof(UserNull)];

            // "undefined"
            char dummy16[sizeof(UserUndefined)];

            // "arbitrary double"
            char dummy17[sizeof(double)];

            // "arbitrary integer"
            char dummy18[sizeof(int)];

            // "arbitrary long"
            char dummy19[sizeof(long long)];

            // aggregationProjectField
            // aggregationProjectionObjectField
            // expressionField
            // valueField
            // onErrorArg
            // onNullArg
            // formatArg
            // timezoneArg
            // charsArg
            // optionsArg
            // hourArg
            // minuteArg
            // secondArg
            // millisecondArg
            // dayArg
            // isoWeekArg
            // iso8601Arg
            // monthArg
            // isoDayOfWeekArg
            // predicate
            // fieldPredicate
            // logicalExpr
            // operatorExpression
            // notExpr
            // matchMod
            // existsExpr
            // typeExpr
            // commentExpr
            // matchExpr
            // matchText
            // matchWhere
            // findProjectField
            // findProjectionObjectField
            // sortSpec
            char dummy20[sizeof(std::pair<CNode::Fieldname, CNode>)];

            // "fieldname"
            // "$-prefixed fieldname"
            // "string"
            // "$-prefixed string"
            // "$$-prefixed string"
            // arg
            char dummy21[sizeof(std::string)];

            // expressions
            // values
            // exprZeroToTwo
            // typeValues
            char dummy22[sizeof(std::vector<CNode>)];

            // "fieldname containing dotted path"
            char dummy23[sizeof(std::vector<std::string>)];
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
    typedef mongo::BSONLocation location_type;

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
            ABS = 3,
            ACOS = 4,
            ACOSH = 5,
            ADD = 6,
            ALL_ELEMENTS_TRUE = 7,
            AND = 8,
            ANY_ELEMENT_TRUE = 9,
            ARG_CASE_SENSITIVE = 10,
            ARG_CHARS = 11,
            ARG_COLL = 12,
            ARG_DATE = 13,
            ARG_DATE_STRING = 14,
            ARG_DAY = 15,
            ARG_DIACRITIC_SENSITIVE = 16,
            ARG_FILTER = 17,
            ARG_FIND = 18,
            ARG_FORMAT = 19,
            ARG_HOUR = 20,
            ARG_INPUT = 21,
            ARG_ISO_8601 = 22,
            ARG_ISO_DAY_OF_WEEK = 23,
            ARG_ISO_WEEK = 24,
            ARG_ISO_WEEK_YEAR = 25,
            ARG_LANGUAGE = 26,
            ARG_MILLISECOND = 27,
            ARG_MINUTE = 28,
            ARG_MONTH = 29,
            ARG_ON_ERROR = 30,
            ARG_ON_NULL = 31,
            ARG_OPTIONS = 32,
            ARG_PIPELINE = 33,
            ARG_REGEX = 34,
            ARG_REPLACEMENT = 35,
            ARG_SEARCH = 36,
            ARG_SECOND = 37,
            ARG_SIZE = 38,
            ARG_TIMEZONE = 39,
            ARG_TO = 40,
            ARG_YEAR = 41,
            ASIN = 42,
            ASINH = 43,
            ATAN = 44,
            ATAN2 = 45,
            ATANH = 46,
            BOOL_FALSE = 47,
            BOOL_TRUE = 48,
            CEIL = 49,
            CMP = 50,
            COMMENT = 51,
            CONCAT = 52,
            CONST_EXPR = 53,
            CONVERT = 54,
            COS = 55,
            COSH = 56,
            DATE_FROM_PARTS = 57,
            DATE_FROM_STRING = 58,
            DATE_TO_PARTS = 59,
            DATE_TO_STRING = 60,
            DAY_OF_MONTH = 61,
            DAY_OF_WEEK = 62,
            DAY_OF_YEAR = 63,
            DECIMAL_NEGATIVE_ONE = 64,
            DECIMAL_ONE = 65,
            DECIMAL_ZERO = 66,
            DEGREES_TO_RADIANS = 67,
            DIVIDE = 68,
            DOUBLE_NEGATIVE_ONE = 69,
            DOUBLE_ONE = 70,
            DOUBLE_ZERO = 71,
            ELEM_MATCH = 72,
            END_ARRAY = 73,
            END_OBJECT = 74,
            EQ = 75,
            EXISTS = 76,
            EXPONENT = 77,
            EXPR = 78,
            FLOOR = 79,
            GEO_NEAR_DISTANCE = 80,
            GEO_NEAR_POINT = 81,
            GT = 82,
            GTE = 83,
            HOUR = 84,
            ID = 85,
            INDEX_KEY = 86,
            INDEX_OF_BYTES = 87,
            INDEX_OF_CP = 88,
            INT_NEGATIVE_ONE = 89,
            INT_ONE = 90,
            INT_ZERO = 91,
            ISO_DAY_OF_WEEK = 92,
            ISO_WEEK = 93,
            ISO_WEEK_YEAR = 94,
            LITERAL = 95,
            LN = 96,
            LOG = 97,
            LOGTEN = 98,
            LONG_NEGATIVE_ONE = 99,
            LONG_ONE = 100,
            LONG_ZERO = 101,
            LT = 102,
            LTE = 103,
            LTRIM = 104,
            META = 105,
            MILLISECOND = 106,
            MINUTE = 107,
            MOD = 108,
            MONTH = 109,
            MULTIPLY = 110,
            NE = 111,
            NOR = 112,
            NOT = 113,
            OR = 114,
            POW = 115,
            RADIANS_TO_DEGREES = 116,
            RAND_VAL = 117,
            RECORD_ID = 118,
            REGEX_FIND = 119,
            REGEX_FIND_ALL = 120,
            REGEX_MATCH = 121,
            REPLACE_ALL = 122,
            REPLACE_ONE = 123,
            ROUND = 124,
            RTRIM = 125,
            SEARCH_HIGHLIGHTS = 126,
            SEARCH_SCORE = 127,
            SECOND = 128,
            SET_DIFFERENCE = 129,
            SET_EQUALS = 130,
            SET_INTERSECTION = 131,
            SET_IS_SUBSET = 132,
            SET_UNION = 133,
            SIN = 134,
            SINH = 135,
            SLICE = 136,
            SORT_KEY = 137,
            SPLIT = 138,
            SQRT = 139,
            STAGE_INHIBIT_OPTIMIZATION = 140,
            STAGE_LIMIT = 141,
            STAGE_MATCH = 142,
            STAGE_PROJECT = 143,
            STAGE_SAMPLE = 144,
            STAGE_SKIP = 145,
            STAGE_UNION_WITH = 146,
            START_ARRAY = 147,
            START_OBJECT = 148,
            STR_CASE_CMP = 149,
            STR_LEN_BYTES = 150,
            STR_LEN_CP = 151,
            SUBSTR = 152,
            SUBSTR_BYTES = 153,
            SUBSTR_CP = 154,
            SUBTRACT = 155,
            TAN = 156,
            TANH = 157,
            TEXT = 158,
            TEXT_SCORE = 159,
            TO_BOOL = 160,
            TO_DATE = 161,
            TO_DECIMAL = 162,
            TO_DOUBLE = 163,
            TO_INT = 164,
            TO_LONG = 165,
            TO_LOWER = 166,
            TO_OBJECT_ID = 167,
            TO_STRING = 168,
            TO_UPPER = 169,
            TRIM = 170,
            TRUNC = 171,
            TYPE = 172,
            WEEK = 173,
            WHERE = 174,
            YEAR = 175,
            FIELDNAME = 176,
            DOTTED_FIELDNAME = 177,
            DOLLAR_PREF_FIELDNAME = 178,
            STRING = 179,
            DOLLAR_STRING = 180,
            DOLLAR_DOLLAR_STRING = 181,
            BINARY = 182,
            UNDEFINED = 183,
            OBJECT_ID = 184,
            DATE_LITERAL = 185,
            JSNULL = 186,
            REGEX = 187,
            DB_POINTER = 188,
            JAVASCRIPT = 189,
            SYMBOL = 190,
            JAVASCRIPT_W_SCOPE = 191,
            INT_OTHER = 192,
            LONG_OTHER = 193,
            DOUBLE_OTHER = 194,
            DECIMAL_OTHER = 195,
            TIMESTAMP = 196,
            MIN_KEY = 197,
            MAX_KEY = 198,
            START_PIPELINE = 199,
            START_MATCH = 200,
            START_PROJECT = 201,
            START_SORT = 202
        };
    };

    /// (External) token type, as returned by yylex.
    typedef token::yytokentype token_type;

    /// Symbol type: an internal symbol number.
    typedef int symbol_number_type;

    /// The symbol type number to denote an empty symbol.
    enum { empty_symbol = -2 };

    /// Internal symbol number for tokens (subsumed by symbol_number_type).
    typedef unsigned char token_number_type;

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
#if 201103L <= YY_CPLUSPLUS
        basic_symbol(typename Base::kind_type t, std::vector<std::string>&& v, location_type&& l)
            : Base(t), value(std::move(v)), location(std::move(l)) {}
#else
        basic_symbol(typename Base::kind_type t,
                     const std::vector<std::string>& v,
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
                case 182:  // "BinData"
                    value.template destroy<BSONBinData>();
                    break;

                case 189:  // "Code"
                    value.template destroy<BSONCode>();
                    break;

                case 191:  // "CodeWScope"
                    value.template destroy<BSONCodeWScope>();
                    break;

                case 188:  // "dbPointer"
                    value.template destroy<BSONDBRef>();
                    break;

                case 187:  // "regex"
                    value.template destroy<BSONRegEx>();
                    break;

                case 190:  // "Symbol"
                    value.template destroy<BSONSymbol>();
                    break;

                case 223:  // dbPointer
                case 224:  // javascript
                case 225:  // symbol
                case 226:  // javascriptWScope
                case 227:  // int
                case 228:  // timestamp
                case 229:  // long
                case 230:  // double
                case 231:  // decimal
                case 232:  // minKey
                case 233:  // maxKey
                case 234:  // value
                case 235:  // string
                case 236:  // aggregationFieldPath
                case 237:  // binary
                case 238:  // undefined
                case 239:  // objectId
                case 240:  // bool
                case 241:  // date
                case 242:  // null
                case 243:  // regex
                case 244:  // simpleValue
                case 245:  // compoundValue
                case 246:  // valueArray
                case 247:  // valueObject
                case 248:  // valueFields
                case 249:  // variable
                case 250:  // typeArray
                case 251:  // typeValue
                case 252:  // pipeline
                case 253:  // stageList
                case 254:  // stage
                case 255:  // inhibitOptimization
                case 256:  // unionWith
                case 257:  // skip
                case 258:  // limit
                case 259:  // matchStage
                case 260:  // project
                case 261:  // sample
                case 262:  // aggregationProjectFields
                case 263:  // aggregationProjectionObjectFields
                case 264:  // topLevelAggregationProjection
                case 265:  // aggregationProjection
                case 266:  // projectionCommon
                case 267:  // aggregationProjectionObject
                case 268:  // num
                case 269:  // expression
                case 270:  // exprFixedTwoArg
                case 271:  // exprFixedThreeArg
                case 272:  // slice
                case 273:  // expressionArray
                case 274:  // expressionObject
                case 275:  // expressionFields
                case 276:  // maths
                case 277:  // meta
                case 278:  // add
                case 279:  // boolExprs
                case 280:  // and
                case 281:  // or
                case 282:  // not
                case 283:  // literalEscapes
                case 284:  // const
                case 285:  // literal
                case 286:  // stringExps
                case 287:  // concat
                case 288:  // dateFromString
                case 289:  // dateToString
                case 290:  // indexOfBytes
                case 291:  // indexOfCP
                case 292:  // ltrim
                case 293:  // regexFind
                case 294:  // regexFindAll
                case 295:  // regexMatch
                case 296:  // regexArgs
                case 297:  // replaceOne
                case 298:  // replaceAll
                case 299:  // rtrim
                case 300:  // split
                case 301:  // strLenBytes
                case 302:  // strLenCP
                case 303:  // strcasecmp
                case 304:  // substr
                case 305:  // substrBytes
                case 306:  // substrCP
                case 307:  // toLower
                case 308:  // toUpper
                case 309:  // trim
                case 310:  // compExprs
                case 311:  // cmp
                case 312:  // eq
                case 313:  // gt
                case 314:  // gte
                case 315:  // lt
                case 316:  // lte
                case 317:  // ne
                case 318:  // dateExps
                case 319:  // dateFromParts
                case 320:  // dateToParts
                case 321:  // dayOfMonth
                case 322:  // dayOfWeek
                case 323:  // dayOfYear
                case 324:  // hour
                case 325:  // isoDayOfWeek
                case 326:  // isoWeek
                case 327:  // isoWeekYear
                case 328:  // millisecond
                case 329:  // minute
                case 330:  // month
                case 331:  // second
                case 332:  // week
                case 333:  // year
                case 334:  // typeExpression
                case 335:  // convert
                case 336:  // toBool
                case 337:  // toDate
                case 338:  // toDecimal
                case 339:  // toDouble
                case 340:  // toInt
                case 341:  // toLong
                case 342:  // toObjectId
                case 343:  // toString
                case 344:  // type
                case 345:  // abs
                case 346:  // ceil
                case 347:  // divide
                case 348:  // exponent
                case 349:  // floor
                case 350:  // ln
                case 351:  // log
                case 352:  // logten
                case 353:  // mod
                case 354:  // multiply
                case 355:  // pow
                case 356:  // round
                case 357:  // sqrt
                case 358:  // subtract
                case 359:  // trunc
                case 378:  // setExpression
                case 379:  // allElementsTrue
                case 380:  // anyElementTrue
                case 381:  // setDifference
                case 382:  // setEquals
                case 383:  // setIntersection
                case 384:  // setIsSubset
                case 385:  // setUnion
                case 386:  // trig
                case 387:  // sin
                case 388:  // cos
                case 389:  // tan
                case 390:  // sinh
                case 391:  // cosh
                case 392:  // tanh
                case 393:  // asin
                case 394:  // acos
                case 395:  // atan
                case 396:  // asinh
                case 397:  // acosh
                case 398:  // atanh
                case 399:  // atan2
                case 400:  // degreesToRadians
                case 401:  // radiansToDegrees
                case 402:  // nonArrayExpression
                case 403:  // nonArrayCompoundExpression
                case 404:  // aggregationOperator
                case 405:  // aggregationOperatorWithoutSlice
                case 406:  // expressionSingletonArray
                case 407:  // singleArgExpression
                case 408:  // nonArrayNonObjExpression
                case 409:  // matchExpression
                case 410:  // predicates
                case 411:  // compoundMatchExprs
                case 412:  // predValue
                case 413:  // additionalExprs
                case 428:  // textArgCaseSensitive
                case 429:  // textArgDiacriticSensitive
                case 430:  // textArgLanguage
                case 431:  // textArgSearch
                case 432:  // findProject
                case 433:  // findProjectFields
                case 434:  // topLevelFindProjection
                case 435:  // findProjection
                case 436:  // findProjectionSlice
                case 437:  // elemMatch
                case 438:  // findProjectionObject
                case 439:  // findProjectionObjectFields
                case 442:  // sortSpecs
                case 443:  // specList
                case 444:  // metaSort
                case 445:  // oneOrNegOne
                case 446:  // metaSortKeyword
                    value.template destroy<CNode>();
                    break;

                case 204:  // aggregationProjectionFieldname
                case 205:  // projectionFieldname
                case 206:  // expressionFieldname
                case 207:  // stageAsUserFieldname
                case 208:  // argAsUserFieldname
                case 209:  // argAsProjectionPath
                case 210:  // aggExprAsUserFieldname
                case 211:  // invariableUserFieldname
                case 212:  // sortFieldname
                case 213:  // idAsUserFieldname
                case 214:  // elemMatchAsUserFieldname
                case 215:  // idAsProjectionPath
                case 216:  // valueFieldname
                case 217:  // predFieldname
                case 423:  // logicalExprField
                    value.template destroy<CNode::Fieldname>();
                    break;

                case 185:  // "Date"
                    value.template destroy<Date_t>();
                    break;

                case 195:  // "arbitrary decimal"
                    value.template destroy<Decimal128>();
                    break;

                case 184:  // "ObjectID"
                    value.template destroy<OID>();
                    break;

                case 196:  // "Timestamp"
                    value.template destroy<Timestamp>();
                    break;

                case 198:  // "maxKey"
                    value.template destroy<UserMaxKey>();
                    break;

                case 197:  // "minKey"
                    value.template destroy<UserMinKey>();
                    break;

                case 186:  // "null"
                    value.template destroy<UserNull>();
                    break;

                case 183:  // "undefined"
                    value.template destroy<UserUndefined>();
                    break;

                case 194:  // "arbitrary double"
                    value.template destroy<double>();
                    break;

                case 192:  // "arbitrary integer"
                    value.template destroy<int>();
                    break;

                case 193:  // "arbitrary long"
                    value.template destroy<long long>();
                    break;

                case 218:  // aggregationProjectField
                case 219:  // aggregationProjectionObjectField
                case 220:  // expressionField
                case 221:  // valueField
                case 360:  // onErrorArg
                case 361:  // onNullArg
                case 362:  // formatArg
                case 363:  // timezoneArg
                case 364:  // charsArg
                case 365:  // optionsArg
                case 366:  // hourArg
                case 367:  // minuteArg
                case 368:  // secondArg
                case 369:  // millisecondArg
                case 370:  // dayArg
                case 371:  // isoWeekArg
                case 372:  // iso8601Arg
                case 373:  // monthArg
                case 374:  // isoDayOfWeekArg
                case 414:  // predicate
                case 415:  // fieldPredicate
                case 416:  // logicalExpr
                case 417:  // operatorExpression
                case 418:  // notExpr
                case 419:  // matchMod
                case 420:  // existsExpr
                case 421:  // typeExpr
                case 422:  // commentExpr
                case 425:  // matchExpr
                case 426:  // matchText
                case 427:  // matchWhere
                case 440:  // findProjectField
                case 441:  // findProjectionObjectField
                case 447:  // sortSpec
                    value.template destroy<std::pair<CNode::Fieldname, CNode>>();
                    break;

                case 176:  // "fieldname"
                case 178:  // "$-prefixed fieldname"
                case 179:  // "string"
                case 180:  // "$-prefixed string"
                case 181:  // "$$-prefixed string"
                case 222:  // arg
                    value.template destroy<std::string>();
                    break;

                case 375:  // expressions
                case 376:  // values
                case 377:  // exprZeroToTwo
                case 424:  // typeValues
                    value.template destroy<std::vector<CNode>>();
                    break;

                case 177:  // "fieldname containing dotted path"
                    value.template destroy<std::vector<std::string>>();
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
                tok == token::END_OF_FILE || tok == token::ABS || tok == token::ACOS ||
                tok == token::ACOSH || tok == token::ADD || tok == token::ALL_ELEMENTS_TRUE ||
                tok == token::AND || tok == token::ANY_ELEMENT_TRUE ||
                tok == token::ARG_CASE_SENSITIVE || tok == token::ARG_CHARS ||
                tok == token::ARG_COLL || tok == token::ARG_DATE || tok == token::ARG_DATE_STRING ||
                tok == token::ARG_DAY || tok == token::ARG_DIACRITIC_SENSITIVE ||
                tok == token::ARG_FILTER || tok == token::ARG_FIND || tok == token::ARG_FORMAT ||
                tok == token::ARG_HOUR || tok == token::ARG_INPUT || tok == token::ARG_ISO_8601 ||
                tok == token::ARG_ISO_DAY_OF_WEEK || tok == token::ARG_ISO_WEEK ||
                tok == token::ARG_ISO_WEEK_YEAR || tok == token::ARG_LANGUAGE ||
                tok == token::ARG_MILLISECOND || tok == token::ARG_MINUTE ||
                tok == token::ARG_MONTH || tok == token::ARG_ON_ERROR ||
                tok == token::ARG_ON_NULL || tok == token::ARG_OPTIONS ||
                tok == token::ARG_PIPELINE || tok == token::ARG_REGEX ||
                tok == token::ARG_REPLACEMENT || tok == token::ARG_SEARCH ||
                tok == token::ARG_SECOND || tok == token::ARG_SIZE || tok == token::ARG_TIMEZONE ||
                tok == token::ARG_TO || tok == token::ARG_YEAR || tok == token::ASIN ||
                tok == token::ASINH || tok == token::ATAN || tok == token::ATAN2 ||
                tok == token::ATANH || tok == token::BOOL_FALSE || tok == token::BOOL_TRUE ||
                tok == token::CEIL || tok == token::CMP || tok == token::COMMENT ||
                tok == token::CONCAT || tok == token::CONST_EXPR || tok == token::CONVERT ||
                tok == token::COS || tok == token::COSH || tok == token::DATE_FROM_PARTS ||
                tok == token::DATE_FROM_STRING || tok == token::DATE_TO_PARTS ||
                tok == token::DATE_TO_STRING || tok == token::DAY_OF_MONTH ||
                tok == token::DAY_OF_WEEK || tok == token::DAY_OF_YEAR ||
                tok == token::DECIMAL_NEGATIVE_ONE || tok == token::DECIMAL_ONE ||
                tok == token::DECIMAL_ZERO || tok == token::DEGREES_TO_RADIANS ||
                tok == token::DIVIDE || tok == token::DOUBLE_NEGATIVE_ONE ||
                tok == token::DOUBLE_ONE || tok == token::DOUBLE_ZERO || tok == token::ELEM_MATCH ||
                tok == token::END_ARRAY || tok == token::END_OBJECT || tok == token::EQ ||
                tok == token::EXISTS || tok == token::EXPONENT || tok == token::EXPR ||
                tok == token::FLOOR || tok == token::GEO_NEAR_DISTANCE ||
                tok == token::GEO_NEAR_POINT || tok == token::GT || tok == token::GTE ||
                tok == token::HOUR || tok == token::ID || tok == token::INDEX_KEY ||
                tok == token::INDEX_OF_BYTES || tok == token::INDEX_OF_CP ||
                tok == token::INT_NEGATIVE_ONE || tok == token::INT_ONE || tok == token::INT_ZERO ||
                tok == token::ISO_DAY_OF_WEEK || tok == token::ISO_WEEK ||
                tok == token::ISO_WEEK_YEAR || tok == token::LITERAL || tok == token::LN ||
                tok == token::LOG || tok == token::LOGTEN || tok == token::LONG_NEGATIVE_ONE ||
                tok == token::LONG_ONE || tok == token::LONG_ZERO || tok == token::LT ||
                tok == token::LTE || tok == token::LTRIM || tok == token::META ||
                tok == token::MILLISECOND || tok == token::MINUTE || tok == token::MOD ||
                tok == token::MONTH || tok == token::MULTIPLY || tok == token::NE ||
                tok == token::NOR || tok == token::NOT || tok == token::OR || tok == token::POW ||
                tok == token::RADIANS_TO_DEGREES || tok == token::RAND_VAL ||
                tok == token::RECORD_ID || tok == token::REGEX_FIND ||
                tok == token::REGEX_FIND_ALL || tok == token::REGEX_MATCH ||
                tok == token::REPLACE_ALL || tok == token::REPLACE_ONE || tok == token::ROUND ||
                tok == token::RTRIM || tok == token::SEARCH_HIGHLIGHTS ||
                tok == token::SEARCH_SCORE || tok == token::SECOND ||
                tok == token::SET_DIFFERENCE || tok == token::SET_EQUALS ||
                tok == token::SET_INTERSECTION || tok == token::SET_IS_SUBSET ||
                tok == token::SET_UNION || tok == token::SIN || tok == token::SINH ||
                tok == token::SLICE || tok == token::SORT_KEY || tok == token::SPLIT ||
                tok == token::SQRT || tok == token::STAGE_INHIBIT_OPTIMIZATION ||
                tok == token::STAGE_LIMIT || tok == token::STAGE_MATCH ||
                tok == token::STAGE_PROJECT || tok == token::STAGE_SAMPLE ||
                tok == token::STAGE_SKIP || tok == token::STAGE_UNION_WITH ||
                tok == token::START_ARRAY || tok == token::START_OBJECT ||
                tok == token::STR_CASE_CMP || tok == token::STR_LEN_BYTES ||
                tok == token::STR_LEN_CP || tok == token::SUBSTR || tok == token::SUBSTR_BYTES ||
                tok == token::SUBSTR_CP || tok == token::SUBTRACT || tok == token::TAN ||
                tok == token::TANH || tok == token::TEXT || tok == token::TEXT_SCORE ||
                tok == token::TO_BOOL || tok == token::TO_DATE || tok == token::TO_DECIMAL ||
                tok == token::TO_DOUBLE || tok == token::TO_INT || tok == token::TO_LONG ||
                tok == token::TO_LOWER || tok == token::TO_OBJECT_ID || tok == token::TO_STRING ||
                tok == token::TO_UPPER || tok == token::TRIM || tok == token::TRUNC ||
                tok == token::TYPE || tok == token::WEEK || tok == token::WHERE ||
                tok == token::YEAR || tok == token::START_PIPELINE || tok == token::START_MATCH ||
                tok == token::START_PROJECT || tok == token::START_SORT);
        }
#else
        symbol_type(int tok, const location_type& l) : super_type(token_type(tok), l) {
            YY_ASSERT(
                tok == token::END_OF_FILE || tok == token::ABS || tok == token::ACOS ||
                tok == token::ACOSH || tok == token::ADD || tok == token::ALL_ELEMENTS_TRUE ||
                tok == token::AND || tok == token::ANY_ELEMENT_TRUE ||
                tok == token::ARG_CASE_SENSITIVE || tok == token::ARG_CHARS ||
                tok == token::ARG_COLL || tok == token::ARG_DATE || tok == token::ARG_DATE_STRING ||
                tok == token::ARG_DAY || tok == token::ARG_DIACRITIC_SENSITIVE ||
                tok == token::ARG_FILTER || tok == token::ARG_FIND || tok == token::ARG_FORMAT ||
                tok == token::ARG_HOUR || tok == token::ARG_INPUT || tok == token::ARG_ISO_8601 ||
                tok == token::ARG_ISO_DAY_OF_WEEK || tok == token::ARG_ISO_WEEK ||
                tok == token::ARG_ISO_WEEK_YEAR || tok == token::ARG_LANGUAGE ||
                tok == token::ARG_MILLISECOND || tok == token::ARG_MINUTE ||
                tok == token::ARG_MONTH || tok == token::ARG_ON_ERROR ||
                tok == token::ARG_ON_NULL || tok == token::ARG_OPTIONS ||
                tok == token::ARG_PIPELINE || tok == token::ARG_REGEX ||
                tok == token::ARG_REPLACEMENT || tok == token::ARG_SEARCH ||
                tok == token::ARG_SECOND || tok == token::ARG_SIZE || tok == token::ARG_TIMEZONE ||
                tok == token::ARG_TO || tok == token::ARG_YEAR || tok == token::ASIN ||
                tok == token::ASINH || tok == token::ATAN || tok == token::ATAN2 ||
                tok == token::ATANH || tok == token::BOOL_FALSE || tok == token::BOOL_TRUE ||
                tok == token::CEIL || tok == token::CMP || tok == token::COMMENT ||
                tok == token::CONCAT || tok == token::CONST_EXPR || tok == token::CONVERT ||
                tok == token::COS || tok == token::COSH || tok == token::DATE_FROM_PARTS ||
                tok == token::DATE_FROM_STRING || tok == token::DATE_TO_PARTS ||
                tok == token::DATE_TO_STRING || tok == token::DAY_OF_MONTH ||
                tok == token::DAY_OF_WEEK || tok == token::DAY_OF_YEAR ||
                tok == token::DECIMAL_NEGATIVE_ONE || tok == token::DECIMAL_ONE ||
                tok == token::DECIMAL_ZERO || tok == token::DEGREES_TO_RADIANS ||
                tok == token::DIVIDE || tok == token::DOUBLE_NEGATIVE_ONE ||
                tok == token::DOUBLE_ONE || tok == token::DOUBLE_ZERO || tok == token::ELEM_MATCH ||
                tok == token::END_ARRAY || tok == token::END_OBJECT || tok == token::EQ ||
                tok == token::EXISTS || tok == token::EXPONENT || tok == token::EXPR ||
                tok == token::FLOOR || tok == token::GEO_NEAR_DISTANCE ||
                tok == token::GEO_NEAR_POINT || tok == token::GT || tok == token::GTE ||
                tok == token::HOUR || tok == token::ID || tok == token::INDEX_KEY ||
                tok == token::INDEX_OF_BYTES || tok == token::INDEX_OF_CP ||
                tok == token::INT_NEGATIVE_ONE || tok == token::INT_ONE || tok == token::INT_ZERO ||
                tok == token::ISO_DAY_OF_WEEK || tok == token::ISO_WEEK ||
                tok == token::ISO_WEEK_YEAR || tok == token::LITERAL || tok == token::LN ||
                tok == token::LOG || tok == token::LOGTEN || tok == token::LONG_NEGATIVE_ONE ||
                tok == token::LONG_ONE || tok == token::LONG_ZERO || tok == token::LT ||
                tok == token::LTE || tok == token::LTRIM || tok == token::META ||
                tok == token::MILLISECOND || tok == token::MINUTE || tok == token::MOD ||
                tok == token::MONTH || tok == token::MULTIPLY || tok == token::NE ||
                tok == token::NOR || tok == token::NOT || tok == token::OR || tok == token::POW ||
                tok == token::RADIANS_TO_DEGREES || tok == token::RAND_VAL ||
                tok == token::RECORD_ID || tok == token::REGEX_FIND ||
                tok == token::REGEX_FIND_ALL || tok == token::REGEX_MATCH ||
                tok == token::REPLACE_ALL || tok == token::REPLACE_ONE || tok == token::ROUND ||
                tok == token::RTRIM || tok == token::SEARCH_HIGHLIGHTS ||
                tok == token::SEARCH_SCORE || tok == token::SECOND ||
                tok == token::SET_DIFFERENCE || tok == token::SET_EQUALS ||
                tok == token::SET_INTERSECTION || tok == token::SET_IS_SUBSET ||
                tok == token::SET_UNION || tok == token::SIN || tok == token::SINH ||
                tok == token::SLICE || tok == token::SORT_KEY || tok == token::SPLIT ||
                tok == token::SQRT || tok == token::STAGE_INHIBIT_OPTIMIZATION ||
                tok == token::STAGE_LIMIT || tok == token::STAGE_MATCH ||
                tok == token::STAGE_PROJECT || tok == token::STAGE_SAMPLE ||
                tok == token::STAGE_SKIP || tok == token::STAGE_UNION_WITH ||
                tok == token::START_ARRAY || tok == token::START_OBJECT ||
                tok == token::STR_CASE_CMP || tok == token::STR_LEN_BYTES ||
                tok == token::STR_LEN_CP || tok == token::SUBSTR || tok == token::SUBSTR_BYTES ||
                tok == token::SUBSTR_CP || tok == token::SUBTRACT || tok == token::TAN ||
                tok == token::TANH || tok == token::TEXT || tok == token::TEXT_SCORE ||
                tok == token::TO_BOOL || tok == token::TO_DATE || tok == token::TO_DECIMAL ||
                tok == token::TO_DOUBLE || tok == token::TO_INT || tok == token::TO_LONG ||
                tok == token::TO_LOWER || tok == token::TO_OBJECT_ID || tok == token::TO_STRING ||
                tok == token::TO_UPPER || tok == token::TRIM || tok == token::TRUNC ||
                tok == token::TYPE || tok == token::WEEK || tok == token::WHERE ||
                tok == token::YEAR || tok == token::START_PIPELINE || tok == token::START_MATCH ||
                tok == token::START_PROJECT || tok == token::START_SORT);
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
            YY_ASSERT(tok == token::DECIMAL_OTHER);
        }
#else
        symbol_type(int tok, const Decimal128& v, const location_type& l)
            : super_type(token_type(tok), v, l) {
            YY_ASSERT(tok == token::DECIMAL_OTHER);
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
            YY_ASSERT(tok == token::DOUBLE_OTHER);
        }
#else
        symbol_type(int tok, const double& v, const location_type& l)
            : super_type(token_type(tok), v, l) {
            YY_ASSERT(tok == token::DOUBLE_OTHER);
        }
#endif
#if 201103L <= YY_CPLUSPLUS
        symbol_type(int tok, int v, location_type l)
            : super_type(token_type(tok), std::move(v), std::move(l)) {
            YY_ASSERT(tok == token::INT_OTHER);
        }
#else
        symbol_type(int tok, const int& v, const location_type& l)
            : super_type(token_type(tok), v, l) {
            YY_ASSERT(tok == token::INT_OTHER);
        }
#endif
#if 201103L <= YY_CPLUSPLUS
        symbol_type(int tok, long long v, location_type l)
            : super_type(token_type(tok), std::move(v), std::move(l)) {
            YY_ASSERT(tok == token::LONG_OTHER);
        }
#else
        symbol_type(int tok, const long long& v, const location_type& l)
            : super_type(token_type(tok), v, l) {
            YY_ASSERT(tok == token::LONG_OTHER);
        }
#endif
#if 201103L <= YY_CPLUSPLUS
        symbol_type(int tok, std::string v, location_type l)
            : super_type(token_type(tok), std::move(v), std::move(l)) {
            YY_ASSERT(tok == token::FIELDNAME || tok == token::DOLLAR_PREF_FIELDNAME ||
                      tok == token::STRING || tok == token::DOLLAR_STRING ||
                      tok == token::DOLLAR_DOLLAR_STRING);
        }
#else
        symbol_type(int tok, const std::string& v, const location_type& l)
            : super_type(token_type(tok), v, l) {
            YY_ASSERT(tok == token::FIELDNAME || tok == token::DOLLAR_PREF_FIELDNAME ||
                      tok == token::STRING || tok == token::DOLLAR_STRING ||
                      tok == token::DOLLAR_DOLLAR_STRING);
        }
#endif
#if 201103L <= YY_CPLUSPLUS
        symbol_type(int tok, std::vector<std::string> v, location_type l)
            : super_type(token_type(tok), std::move(v), std::move(l)) {
            YY_ASSERT(tok == token::DOTTED_FIELDNAME);
        }
#else
        symbol_type(int tok, const std::vector<std::string>& v, const location_type& l)
            : super_type(token_type(tok), v, l) {
            YY_ASSERT(tok == token::DOTTED_FIELDNAME);
        }
#endif
    };

    /// Build a parser object.
    ParserGen(BSONLexer& lexer_yyarg, CNode* cst_yyarg);
    virtual ~ParserGen();

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
    static symbol_type make_ABS(location_type l) {
        return symbol_type(token::ABS, std::move(l));
    }
#else
    static symbol_type make_ABS(const location_type& l) {
        return symbol_type(token::ABS, l);
    }
#endif
#if 201103L <= YY_CPLUSPLUS
    static symbol_type make_ACOS(location_type l) {
        return symbol_type(token::ACOS, std::move(l));
    }
#else
    static symbol_type make_ACOS(const location_type& l) {
        return symbol_type(token::ACOS, l);
    }
#endif
#if 201103L <= YY_CPLUSPLUS
    static symbol_type make_ACOSH(location_type l) {
        return symbol_type(token::ACOSH, std::move(l));
    }
#else
    static symbol_type make_ACOSH(const location_type& l) {
        return symbol_type(token::ACOSH, l);
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
    static symbol_type make_ALL_ELEMENTS_TRUE(location_type l) {
        return symbol_type(token::ALL_ELEMENTS_TRUE, std::move(l));
    }
#else
    static symbol_type make_ALL_ELEMENTS_TRUE(const location_type& l) {
        return symbol_type(token::ALL_ELEMENTS_TRUE, l);
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
    static symbol_type make_ANY_ELEMENT_TRUE(location_type l) {
        return symbol_type(token::ANY_ELEMENT_TRUE, std::move(l));
    }
#else
    static symbol_type make_ANY_ELEMENT_TRUE(const location_type& l) {
        return symbol_type(token::ANY_ELEMENT_TRUE, l);
    }
#endif
#if 201103L <= YY_CPLUSPLUS
    static symbol_type make_ARG_CASE_SENSITIVE(location_type l) {
        return symbol_type(token::ARG_CASE_SENSITIVE, std::move(l));
    }
#else
    static symbol_type make_ARG_CASE_SENSITIVE(const location_type& l) {
        return symbol_type(token::ARG_CASE_SENSITIVE, l);
    }
#endif
#if 201103L <= YY_CPLUSPLUS
    static symbol_type make_ARG_CHARS(location_type l) {
        return symbol_type(token::ARG_CHARS, std::move(l));
    }
#else
    static symbol_type make_ARG_CHARS(const location_type& l) {
        return symbol_type(token::ARG_CHARS, l);
    }
#endif
#if 201103L <= YY_CPLUSPLUS
    static symbol_type make_ARG_COLL(location_type l) {
        return symbol_type(token::ARG_COLL, std::move(l));
    }
#else
    static symbol_type make_ARG_COLL(const location_type& l) {
        return symbol_type(token::ARG_COLL, l);
    }
#endif
#if 201103L <= YY_CPLUSPLUS
    static symbol_type make_ARG_DATE(location_type l) {
        return symbol_type(token::ARG_DATE, std::move(l));
    }
#else
    static symbol_type make_ARG_DATE(const location_type& l) {
        return symbol_type(token::ARG_DATE, l);
    }
#endif
#if 201103L <= YY_CPLUSPLUS
    static symbol_type make_ARG_DATE_STRING(location_type l) {
        return symbol_type(token::ARG_DATE_STRING, std::move(l));
    }
#else
    static symbol_type make_ARG_DATE_STRING(const location_type& l) {
        return symbol_type(token::ARG_DATE_STRING, l);
    }
#endif
#if 201103L <= YY_CPLUSPLUS
    static symbol_type make_ARG_DAY(location_type l) {
        return symbol_type(token::ARG_DAY, std::move(l));
    }
#else
    static symbol_type make_ARG_DAY(const location_type& l) {
        return symbol_type(token::ARG_DAY, l);
    }
#endif
#if 201103L <= YY_CPLUSPLUS
    static symbol_type make_ARG_DIACRITIC_SENSITIVE(location_type l) {
        return symbol_type(token::ARG_DIACRITIC_SENSITIVE, std::move(l));
    }
#else
    static symbol_type make_ARG_DIACRITIC_SENSITIVE(const location_type& l) {
        return symbol_type(token::ARG_DIACRITIC_SENSITIVE, l);
    }
#endif
#if 201103L <= YY_CPLUSPLUS
    static symbol_type make_ARG_FILTER(location_type l) {
        return symbol_type(token::ARG_FILTER, std::move(l));
    }
#else
    static symbol_type make_ARG_FILTER(const location_type& l) {
        return symbol_type(token::ARG_FILTER, l);
    }
#endif
#if 201103L <= YY_CPLUSPLUS
    static symbol_type make_ARG_FIND(location_type l) {
        return symbol_type(token::ARG_FIND, std::move(l));
    }
#else
    static symbol_type make_ARG_FIND(const location_type& l) {
        return symbol_type(token::ARG_FIND, l);
    }
#endif
#if 201103L <= YY_CPLUSPLUS
    static symbol_type make_ARG_FORMAT(location_type l) {
        return symbol_type(token::ARG_FORMAT, std::move(l));
    }
#else
    static symbol_type make_ARG_FORMAT(const location_type& l) {
        return symbol_type(token::ARG_FORMAT, l);
    }
#endif
#if 201103L <= YY_CPLUSPLUS
    static symbol_type make_ARG_HOUR(location_type l) {
        return symbol_type(token::ARG_HOUR, std::move(l));
    }
#else
    static symbol_type make_ARG_HOUR(const location_type& l) {
        return symbol_type(token::ARG_HOUR, l);
    }
#endif
#if 201103L <= YY_CPLUSPLUS
    static symbol_type make_ARG_INPUT(location_type l) {
        return symbol_type(token::ARG_INPUT, std::move(l));
    }
#else
    static symbol_type make_ARG_INPUT(const location_type& l) {
        return symbol_type(token::ARG_INPUT, l);
    }
#endif
#if 201103L <= YY_CPLUSPLUS
    static symbol_type make_ARG_ISO_8601(location_type l) {
        return symbol_type(token::ARG_ISO_8601, std::move(l));
    }
#else
    static symbol_type make_ARG_ISO_8601(const location_type& l) {
        return symbol_type(token::ARG_ISO_8601, l);
    }
#endif
#if 201103L <= YY_CPLUSPLUS
    static symbol_type make_ARG_ISO_DAY_OF_WEEK(location_type l) {
        return symbol_type(token::ARG_ISO_DAY_OF_WEEK, std::move(l));
    }
#else
    static symbol_type make_ARG_ISO_DAY_OF_WEEK(const location_type& l) {
        return symbol_type(token::ARG_ISO_DAY_OF_WEEK, l);
    }
#endif
#if 201103L <= YY_CPLUSPLUS
    static symbol_type make_ARG_ISO_WEEK(location_type l) {
        return symbol_type(token::ARG_ISO_WEEK, std::move(l));
    }
#else
    static symbol_type make_ARG_ISO_WEEK(const location_type& l) {
        return symbol_type(token::ARG_ISO_WEEK, l);
    }
#endif
#if 201103L <= YY_CPLUSPLUS
    static symbol_type make_ARG_ISO_WEEK_YEAR(location_type l) {
        return symbol_type(token::ARG_ISO_WEEK_YEAR, std::move(l));
    }
#else
    static symbol_type make_ARG_ISO_WEEK_YEAR(const location_type& l) {
        return symbol_type(token::ARG_ISO_WEEK_YEAR, l);
    }
#endif
#if 201103L <= YY_CPLUSPLUS
    static symbol_type make_ARG_LANGUAGE(location_type l) {
        return symbol_type(token::ARG_LANGUAGE, std::move(l));
    }
#else
    static symbol_type make_ARG_LANGUAGE(const location_type& l) {
        return symbol_type(token::ARG_LANGUAGE, l);
    }
#endif
#if 201103L <= YY_CPLUSPLUS
    static symbol_type make_ARG_MILLISECOND(location_type l) {
        return symbol_type(token::ARG_MILLISECOND, std::move(l));
    }
#else
    static symbol_type make_ARG_MILLISECOND(const location_type& l) {
        return symbol_type(token::ARG_MILLISECOND, l);
    }
#endif
#if 201103L <= YY_CPLUSPLUS
    static symbol_type make_ARG_MINUTE(location_type l) {
        return symbol_type(token::ARG_MINUTE, std::move(l));
    }
#else
    static symbol_type make_ARG_MINUTE(const location_type& l) {
        return symbol_type(token::ARG_MINUTE, l);
    }
#endif
#if 201103L <= YY_CPLUSPLUS
    static symbol_type make_ARG_MONTH(location_type l) {
        return symbol_type(token::ARG_MONTH, std::move(l));
    }
#else
    static symbol_type make_ARG_MONTH(const location_type& l) {
        return symbol_type(token::ARG_MONTH, l);
    }
#endif
#if 201103L <= YY_CPLUSPLUS
    static symbol_type make_ARG_ON_ERROR(location_type l) {
        return symbol_type(token::ARG_ON_ERROR, std::move(l));
    }
#else
    static symbol_type make_ARG_ON_ERROR(const location_type& l) {
        return symbol_type(token::ARG_ON_ERROR, l);
    }
#endif
#if 201103L <= YY_CPLUSPLUS
    static symbol_type make_ARG_ON_NULL(location_type l) {
        return symbol_type(token::ARG_ON_NULL, std::move(l));
    }
#else
    static symbol_type make_ARG_ON_NULL(const location_type& l) {
        return symbol_type(token::ARG_ON_NULL, l);
    }
#endif
#if 201103L <= YY_CPLUSPLUS
    static symbol_type make_ARG_OPTIONS(location_type l) {
        return symbol_type(token::ARG_OPTIONS, std::move(l));
    }
#else
    static symbol_type make_ARG_OPTIONS(const location_type& l) {
        return symbol_type(token::ARG_OPTIONS, l);
    }
#endif
#if 201103L <= YY_CPLUSPLUS
    static symbol_type make_ARG_PIPELINE(location_type l) {
        return symbol_type(token::ARG_PIPELINE, std::move(l));
    }
#else
    static symbol_type make_ARG_PIPELINE(const location_type& l) {
        return symbol_type(token::ARG_PIPELINE, l);
    }
#endif
#if 201103L <= YY_CPLUSPLUS
    static symbol_type make_ARG_REGEX(location_type l) {
        return symbol_type(token::ARG_REGEX, std::move(l));
    }
#else
    static symbol_type make_ARG_REGEX(const location_type& l) {
        return symbol_type(token::ARG_REGEX, l);
    }
#endif
#if 201103L <= YY_CPLUSPLUS
    static symbol_type make_ARG_REPLACEMENT(location_type l) {
        return symbol_type(token::ARG_REPLACEMENT, std::move(l));
    }
#else
    static symbol_type make_ARG_REPLACEMENT(const location_type& l) {
        return symbol_type(token::ARG_REPLACEMENT, l);
    }
#endif
#if 201103L <= YY_CPLUSPLUS
    static symbol_type make_ARG_SEARCH(location_type l) {
        return symbol_type(token::ARG_SEARCH, std::move(l));
    }
#else
    static symbol_type make_ARG_SEARCH(const location_type& l) {
        return symbol_type(token::ARG_SEARCH, l);
    }
#endif
#if 201103L <= YY_CPLUSPLUS
    static symbol_type make_ARG_SECOND(location_type l) {
        return symbol_type(token::ARG_SECOND, std::move(l));
    }
#else
    static symbol_type make_ARG_SECOND(const location_type& l) {
        return symbol_type(token::ARG_SECOND, l);
    }
#endif
#if 201103L <= YY_CPLUSPLUS
    static symbol_type make_ARG_SIZE(location_type l) {
        return symbol_type(token::ARG_SIZE, std::move(l));
    }
#else
    static symbol_type make_ARG_SIZE(const location_type& l) {
        return symbol_type(token::ARG_SIZE, l);
    }
#endif
#if 201103L <= YY_CPLUSPLUS
    static symbol_type make_ARG_TIMEZONE(location_type l) {
        return symbol_type(token::ARG_TIMEZONE, std::move(l));
    }
#else
    static symbol_type make_ARG_TIMEZONE(const location_type& l) {
        return symbol_type(token::ARG_TIMEZONE, l);
    }
#endif
#if 201103L <= YY_CPLUSPLUS
    static symbol_type make_ARG_TO(location_type l) {
        return symbol_type(token::ARG_TO, std::move(l));
    }
#else
    static symbol_type make_ARG_TO(const location_type& l) {
        return symbol_type(token::ARG_TO, l);
    }
#endif
#if 201103L <= YY_CPLUSPLUS
    static symbol_type make_ARG_YEAR(location_type l) {
        return symbol_type(token::ARG_YEAR, std::move(l));
    }
#else
    static symbol_type make_ARG_YEAR(const location_type& l) {
        return symbol_type(token::ARG_YEAR, l);
    }
#endif
#if 201103L <= YY_CPLUSPLUS
    static symbol_type make_ASIN(location_type l) {
        return symbol_type(token::ASIN, std::move(l));
    }
#else
    static symbol_type make_ASIN(const location_type& l) {
        return symbol_type(token::ASIN, l);
    }
#endif
#if 201103L <= YY_CPLUSPLUS
    static symbol_type make_ASINH(location_type l) {
        return symbol_type(token::ASINH, std::move(l));
    }
#else
    static symbol_type make_ASINH(const location_type& l) {
        return symbol_type(token::ASINH, l);
    }
#endif
#if 201103L <= YY_CPLUSPLUS
    static symbol_type make_ATAN(location_type l) {
        return symbol_type(token::ATAN, std::move(l));
    }
#else
    static symbol_type make_ATAN(const location_type& l) {
        return symbol_type(token::ATAN, l);
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
    static symbol_type make_ATANH(location_type l) {
        return symbol_type(token::ATANH, std::move(l));
    }
#else
    static symbol_type make_ATANH(const location_type& l) {
        return symbol_type(token::ATANH, l);
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
    static symbol_type make_BOOL_TRUE(location_type l) {
        return symbol_type(token::BOOL_TRUE, std::move(l));
    }
#else
    static symbol_type make_BOOL_TRUE(const location_type& l) {
        return symbol_type(token::BOOL_TRUE, l);
    }
#endif
#if 201103L <= YY_CPLUSPLUS
    static symbol_type make_CEIL(location_type l) {
        return symbol_type(token::CEIL, std::move(l));
    }
#else
    static symbol_type make_CEIL(const location_type& l) {
        return symbol_type(token::CEIL, l);
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
    static symbol_type make_COMMENT(location_type l) {
        return symbol_type(token::COMMENT, std::move(l));
    }
#else
    static symbol_type make_COMMENT(const location_type& l) {
        return symbol_type(token::COMMENT, l);
    }
#endif
#if 201103L <= YY_CPLUSPLUS
    static symbol_type make_CONCAT(location_type l) {
        return symbol_type(token::CONCAT, std::move(l));
    }
#else
    static symbol_type make_CONCAT(const location_type& l) {
        return symbol_type(token::CONCAT, l);
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
    static symbol_type make_CONVERT(location_type l) {
        return symbol_type(token::CONVERT, std::move(l));
    }
#else
    static symbol_type make_CONVERT(const location_type& l) {
        return symbol_type(token::CONVERT, l);
    }
#endif
#if 201103L <= YY_CPLUSPLUS
    static symbol_type make_COS(location_type l) {
        return symbol_type(token::COS, std::move(l));
    }
#else
    static symbol_type make_COS(const location_type& l) {
        return symbol_type(token::COS, l);
    }
#endif
#if 201103L <= YY_CPLUSPLUS
    static symbol_type make_COSH(location_type l) {
        return symbol_type(token::COSH, std::move(l));
    }
#else
    static symbol_type make_COSH(const location_type& l) {
        return symbol_type(token::COSH, l);
    }
#endif
#if 201103L <= YY_CPLUSPLUS
    static symbol_type make_DATE_FROM_PARTS(location_type l) {
        return symbol_type(token::DATE_FROM_PARTS, std::move(l));
    }
#else
    static symbol_type make_DATE_FROM_PARTS(const location_type& l) {
        return symbol_type(token::DATE_FROM_PARTS, l);
    }
#endif
#if 201103L <= YY_CPLUSPLUS
    static symbol_type make_DATE_FROM_STRING(location_type l) {
        return symbol_type(token::DATE_FROM_STRING, std::move(l));
    }
#else
    static symbol_type make_DATE_FROM_STRING(const location_type& l) {
        return symbol_type(token::DATE_FROM_STRING, l);
    }
#endif
#if 201103L <= YY_CPLUSPLUS
    static symbol_type make_DATE_TO_PARTS(location_type l) {
        return symbol_type(token::DATE_TO_PARTS, std::move(l));
    }
#else
    static symbol_type make_DATE_TO_PARTS(const location_type& l) {
        return symbol_type(token::DATE_TO_PARTS, l);
    }
#endif
#if 201103L <= YY_CPLUSPLUS
    static symbol_type make_DATE_TO_STRING(location_type l) {
        return symbol_type(token::DATE_TO_STRING, std::move(l));
    }
#else
    static symbol_type make_DATE_TO_STRING(const location_type& l) {
        return symbol_type(token::DATE_TO_STRING, l);
    }
#endif
#if 201103L <= YY_CPLUSPLUS
    static symbol_type make_DAY_OF_MONTH(location_type l) {
        return symbol_type(token::DAY_OF_MONTH, std::move(l));
    }
#else
    static symbol_type make_DAY_OF_MONTH(const location_type& l) {
        return symbol_type(token::DAY_OF_MONTH, l);
    }
#endif
#if 201103L <= YY_CPLUSPLUS
    static symbol_type make_DAY_OF_WEEK(location_type l) {
        return symbol_type(token::DAY_OF_WEEK, std::move(l));
    }
#else
    static symbol_type make_DAY_OF_WEEK(const location_type& l) {
        return symbol_type(token::DAY_OF_WEEK, l);
    }
#endif
#if 201103L <= YY_CPLUSPLUS
    static symbol_type make_DAY_OF_YEAR(location_type l) {
        return symbol_type(token::DAY_OF_YEAR, std::move(l));
    }
#else
    static symbol_type make_DAY_OF_YEAR(const location_type& l) {
        return symbol_type(token::DAY_OF_YEAR, l);
    }
#endif
#if 201103L <= YY_CPLUSPLUS
    static symbol_type make_DECIMAL_NEGATIVE_ONE(location_type l) {
        return symbol_type(token::DECIMAL_NEGATIVE_ONE, std::move(l));
    }
#else
    static symbol_type make_DECIMAL_NEGATIVE_ONE(const location_type& l) {
        return symbol_type(token::DECIMAL_NEGATIVE_ONE, l);
    }
#endif
#if 201103L <= YY_CPLUSPLUS
    static symbol_type make_DECIMAL_ONE(location_type l) {
        return symbol_type(token::DECIMAL_ONE, std::move(l));
    }
#else
    static symbol_type make_DECIMAL_ONE(const location_type& l) {
        return symbol_type(token::DECIMAL_ONE, l);
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
    static symbol_type make_DEGREES_TO_RADIANS(location_type l) {
        return symbol_type(token::DEGREES_TO_RADIANS, std::move(l));
    }
#else
    static symbol_type make_DEGREES_TO_RADIANS(const location_type& l) {
        return symbol_type(token::DEGREES_TO_RADIANS, l);
    }
#endif
#if 201103L <= YY_CPLUSPLUS
    static symbol_type make_DIVIDE(location_type l) {
        return symbol_type(token::DIVIDE, std::move(l));
    }
#else
    static symbol_type make_DIVIDE(const location_type& l) {
        return symbol_type(token::DIVIDE, l);
    }
#endif
#if 201103L <= YY_CPLUSPLUS
    static symbol_type make_DOUBLE_NEGATIVE_ONE(location_type l) {
        return symbol_type(token::DOUBLE_NEGATIVE_ONE, std::move(l));
    }
#else
    static symbol_type make_DOUBLE_NEGATIVE_ONE(const location_type& l) {
        return symbol_type(token::DOUBLE_NEGATIVE_ONE, l);
    }
#endif
#if 201103L <= YY_CPLUSPLUS
    static symbol_type make_DOUBLE_ONE(location_type l) {
        return symbol_type(token::DOUBLE_ONE, std::move(l));
    }
#else
    static symbol_type make_DOUBLE_ONE(const location_type& l) {
        return symbol_type(token::DOUBLE_ONE, l);
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
    static symbol_type make_ELEM_MATCH(location_type l) {
        return symbol_type(token::ELEM_MATCH, std::move(l));
    }
#else
    static symbol_type make_ELEM_MATCH(const location_type& l) {
        return symbol_type(token::ELEM_MATCH, l);
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
    static symbol_type make_END_OBJECT(location_type l) {
        return symbol_type(token::END_OBJECT, std::move(l));
    }
#else
    static symbol_type make_END_OBJECT(const location_type& l) {
        return symbol_type(token::END_OBJECT, l);
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
    static symbol_type make_EXISTS(location_type l) {
        return symbol_type(token::EXISTS, std::move(l));
    }
#else
    static symbol_type make_EXISTS(const location_type& l) {
        return symbol_type(token::EXISTS, l);
    }
#endif
#if 201103L <= YY_CPLUSPLUS
    static symbol_type make_EXPONENT(location_type l) {
        return symbol_type(token::EXPONENT, std::move(l));
    }
#else
    static symbol_type make_EXPONENT(const location_type& l) {
        return symbol_type(token::EXPONENT, l);
    }
#endif
#if 201103L <= YY_CPLUSPLUS
    static symbol_type make_EXPR(location_type l) {
        return symbol_type(token::EXPR, std::move(l));
    }
#else
    static symbol_type make_EXPR(const location_type& l) {
        return symbol_type(token::EXPR, l);
    }
#endif
#if 201103L <= YY_CPLUSPLUS
    static symbol_type make_FLOOR(location_type l) {
        return symbol_type(token::FLOOR, std::move(l));
    }
#else
    static symbol_type make_FLOOR(const location_type& l) {
        return symbol_type(token::FLOOR, l);
    }
#endif
#if 201103L <= YY_CPLUSPLUS
    static symbol_type make_GEO_NEAR_DISTANCE(location_type l) {
        return symbol_type(token::GEO_NEAR_DISTANCE, std::move(l));
    }
#else
    static symbol_type make_GEO_NEAR_DISTANCE(const location_type& l) {
        return symbol_type(token::GEO_NEAR_DISTANCE, l);
    }
#endif
#if 201103L <= YY_CPLUSPLUS
    static symbol_type make_GEO_NEAR_POINT(location_type l) {
        return symbol_type(token::GEO_NEAR_POINT, std::move(l));
    }
#else
    static symbol_type make_GEO_NEAR_POINT(const location_type& l) {
        return symbol_type(token::GEO_NEAR_POINT, l);
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
    static symbol_type make_HOUR(location_type l) {
        return symbol_type(token::HOUR, std::move(l));
    }
#else
    static symbol_type make_HOUR(const location_type& l) {
        return symbol_type(token::HOUR, l);
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
    static symbol_type make_INDEX_KEY(location_type l) {
        return symbol_type(token::INDEX_KEY, std::move(l));
    }
#else
    static symbol_type make_INDEX_KEY(const location_type& l) {
        return symbol_type(token::INDEX_KEY, l);
    }
#endif
#if 201103L <= YY_CPLUSPLUS
    static symbol_type make_INDEX_OF_BYTES(location_type l) {
        return symbol_type(token::INDEX_OF_BYTES, std::move(l));
    }
#else
    static symbol_type make_INDEX_OF_BYTES(const location_type& l) {
        return symbol_type(token::INDEX_OF_BYTES, l);
    }
#endif
#if 201103L <= YY_CPLUSPLUS
    static symbol_type make_INDEX_OF_CP(location_type l) {
        return symbol_type(token::INDEX_OF_CP, std::move(l));
    }
#else
    static symbol_type make_INDEX_OF_CP(const location_type& l) {
        return symbol_type(token::INDEX_OF_CP, l);
    }
#endif
#if 201103L <= YY_CPLUSPLUS
    static symbol_type make_INT_NEGATIVE_ONE(location_type l) {
        return symbol_type(token::INT_NEGATIVE_ONE, std::move(l));
    }
#else
    static symbol_type make_INT_NEGATIVE_ONE(const location_type& l) {
        return symbol_type(token::INT_NEGATIVE_ONE, l);
    }
#endif
#if 201103L <= YY_CPLUSPLUS
    static symbol_type make_INT_ONE(location_type l) {
        return symbol_type(token::INT_ONE, std::move(l));
    }
#else
    static symbol_type make_INT_ONE(const location_type& l) {
        return symbol_type(token::INT_ONE, l);
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
    static symbol_type make_ISO_DAY_OF_WEEK(location_type l) {
        return symbol_type(token::ISO_DAY_OF_WEEK, std::move(l));
    }
#else
    static symbol_type make_ISO_DAY_OF_WEEK(const location_type& l) {
        return symbol_type(token::ISO_DAY_OF_WEEK, l);
    }
#endif
#if 201103L <= YY_CPLUSPLUS
    static symbol_type make_ISO_WEEK(location_type l) {
        return symbol_type(token::ISO_WEEK, std::move(l));
    }
#else
    static symbol_type make_ISO_WEEK(const location_type& l) {
        return symbol_type(token::ISO_WEEK, l);
    }
#endif
#if 201103L <= YY_CPLUSPLUS
    static symbol_type make_ISO_WEEK_YEAR(location_type l) {
        return symbol_type(token::ISO_WEEK_YEAR, std::move(l));
    }
#else
    static symbol_type make_ISO_WEEK_YEAR(const location_type& l) {
        return symbol_type(token::ISO_WEEK_YEAR, l);
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
    static symbol_type make_LN(location_type l) {
        return symbol_type(token::LN, std::move(l));
    }
#else
    static symbol_type make_LN(const location_type& l) {
        return symbol_type(token::LN, l);
    }
#endif
#if 201103L <= YY_CPLUSPLUS
    static symbol_type make_LOG(location_type l) {
        return symbol_type(token::LOG, std::move(l));
    }
#else
    static symbol_type make_LOG(const location_type& l) {
        return symbol_type(token::LOG, l);
    }
#endif
#if 201103L <= YY_CPLUSPLUS
    static symbol_type make_LOGTEN(location_type l) {
        return symbol_type(token::LOGTEN, std::move(l));
    }
#else
    static symbol_type make_LOGTEN(const location_type& l) {
        return symbol_type(token::LOGTEN, l);
    }
#endif
#if 201103L <= YY_CPLUSPLUS
    static symbol_type make_LONG_NEGATIVE_ONE(location_type l) {
        return symbol_type(token::LONG_NEGATIVE_ONE, std::move(l));
    }
#else
    static symbol_type make_LONG_NEGATIVE_ONE(const location_type& l) {
        return symbol_type(token::LONG_NEGATIVE_ONE, l);
    }
#endif
#if 201103L <= YY_CPLUSPLUS
    static symbol_type make_LONG_ONE(location_type l) {
        return symbol_type(token::LONG_ONE, std::move(l));
    }
#else
    static symbol_type make_LONG_ONE(const location_type& l) {
        return symbol_type(token::LONG_ONE, l);
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
    static symbol_type make_LTRIM(location_type l) {
        return symbol_type(token::LTRIM, std::move(l));
    }
#else
    static symbol_type make_LTRIM(const location_type& l) {
        return symbol_type(token::LTRIM, l);
    }
#endif
#if 201103L <= YY_CPLUSPLUS
    static symbol_type make_META(location_type l) {
        return symbol_type(token::META, std::move(l));
    }
#else
    static symbol_type make_META(const location_type& l) {
        return symbol_type(token::META, l);
    }
#endif
#if 201103L <= YY_CPLUSPLUS
    static symbol_type make_MILLISECOND(location_type l) {
        return symbol_type(token::MILLISECOND, std::move(l));
    }
#else
    static symbol_type make_MILLISECOND(const location_type& l) {
        return symbol_type(token::MILLISECOND, l);
    }
#endif
#if 201103L <= YY_CPLUSPLUS
    static symbol_type make_MINUTE(location_type l) {
        return symbol_type(token::MINUTE, std::move(l));
    }
#else
    static symbol_type make_MINUTE(const location_type& l) {
        return symbol_type(token::MINUTE, l);
    }
#endif
#if 201103L <= YY_CPLUSPLUS
    static symbol_type make_MOD(location_type l) {
        return symbol_type(token::MOD, std::move(l));
    }
#else
    static symbol_type make_MOD(const location_type& l) {
        return symbol_type(token::MOD, l);
    }
#endif
#if 201103L <= YY_CPLUSPLUS
    static symbol_type make_MONTH(location_type l) {
        return symbol_type(token::MONTH, std::move(l));
    }
#else
    static symbol_type make_MONTH(const location_type& l) {
        return symbol_type(token::MONTH, l);
    }
#endif
#if 201103L <= YY_CPLUSPLUS
    static symbol_type make_MULTIPLY(location_type l) {
        return symbol_type(token::MULTIPLY, std::move(l));
    }
#else
    static symbol_type make_MULTIPLY(const location_type& l) {
        return symbol_type(token::MULTIPLY, l);
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
    static symbol_type make_NOR(location_type l) {
        return symbol_type(token::NOR, std::move(l));
    }
#else
    static symbol_type make_NOR(const location_type& l) {
        return symbol_type(token::NOR, l);
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
    static symbol_type make_OR(location_type l) {
        return symbol_type(token::OR, std::move(l));
    }
#else
    static symbol_type make_OR(const location_type& l) {
        return symbol_type(token::OR, l);
    }
#endif
#if 201103L <= YY_CPLUSPLUS
    static symbol_type make_POW(location_type l) {
        return symbol_type(token::POW, std::move(l));
    }
#else
    static symbol_type make_POW(const location_type& l) {
        return symbol_type(token::POW, l);
    }
#endif
#if 201103L <= YY_CPLUSPLUS
    static symbol_type make_RADIANS_TO_DEGREES(location_type l) {
        return symbol_type(token::RADIANS_TO_DEGREES, std::move(l));
    }
#else
    static symbol_type make_RADIANS_TO_DEGREES(const location_type& l) {
        return symbol_type(token::RADIANS_TO_DEGREES, l);
    }
#endif
#if 201103L <= YY_CPLUSPLUS
    static symbol_type make_RAND_VAL(location_type l) {
        return symbol_type(token::RAND_VAL, std::move(l));
    }
#else
    static symbol_type make_RAND_VAL(const location_type& l) {
        return symbol_type(token::RAND_VAL, l);
    }
#endif
#if 201103L <= YY_CPLUSPLUS
    static symbol_type make_RECORD_ID(location_type l) {
        return symbol_type(token::RECORD_ID, std::move(l));
    }
#else
    static symbol_type make_RECORD_ID(const location_type& l) {
        return symbol_type(token::RECORD_ID, l);
    }
#endif
#if 201103L <= YY_CPLUSPLUS
    static symbol_type make_REGEX_FIND(location_type l) {
        return symbol_type(token::REGEX_FIND, std::move(l));
    }
#else
    static symbol_type make_REGEX_FIND(const location_type& l) {
        return symbol_type(token::REGEX_FIND, l);
    }
#endif
#if 201103L <= YY_CPLUSPLUS
    static symbol_type make_REGEX_FIND_ALL(location_type l) {
        return symbol_type(token::REGEX_FIND_ALL, std::move(l));
    }
#else
    static symbol_type make_REGEX_FIND_ALL(const location_type& l) {
        return symbol_type(token::REGEX_FIND_ALL, l);
    }
#endif
#if 201103L <= YY_CPLUSPLUS
    static symbol_type make_REGEX_MATCH(location_type l) {
        return symbol_type(token::REGEX_MATCH, std::move(l));
    }
#else
    static symbol_type make_REGEX_MATCH(const location_type& l) {
        return symbol_type(token::REGEX_MATCH, l);
    }
#endif
#if 201103L <= YY_CPLUSPLUS
    static symbol_type make_REPLACE_ALL(location_type l) {
        return symbol_type(token::REPLACE_ALL, std::move(l));
    }
#else
    static symbol_type make_REPLACE_ALL(const location_type& l) {
        return symbol_type(token::REPLACE_ALL, l);
    }
#endif
#if 201103L <= YY_CPLUSPLUS
    static symbol_type make_REPLACE_ONE(location_type l) {
        return symbol_type(token::REPLACE_ONE, std::move(l));
    }
#else
    static symbol_type make_REPLACE_ONE(const location_type& l) {
        return symbol_type(token::REPLACE_ONE, l);
    }
#endif
#if 201103L <= YY_CPLUSPLUS
    static symbol_type make_ROUND(location_type l) {
        return symbol_type(token::ROUND, std::move(l));
    }
#else
    static symbol_type make_ROUND(const location_type& l) {
        return symbol_type(token::ROUND, l);
    }
#endif
#if 201103L <= YY_CPLUSPLUS
    static symbol_type make_RTRIM(location_type l) {
        return symbol_type(token::RTRIM, std::move(l));
    }
#else
    static symbol_type make_RTRIM(const location_type& l) {
        return symbol_type(token::RTRIM, l);
    }
#endif
#if 201103L <= YY_CPLUSPLUS
    static symbol_type make_SEARCH_HIGHLIGHTS(location_type l) {
        return symbol_type(token::SEARCH_HIGHLIGHTS, std::move(l));
    }
#else
    static symbol_type make_SEARCH_HIGHLIGHTS(const location_type& l) {
        return symbol_type(token::SEARCH_HIGHLIGHTS, l);
    }
#endif
#if 201103L <= YY_CPLUSPLUS
    static symbol_type make_SEARCH_SCORE(location_type l) {
        return symbol_type(token::SEARCH_SCORE, std::move(l));
    }
#else
    static symbol_type make_SEARCH_SCORE(const location_type& l) {
        return symbol_type(token::SEARCH_SCORE, l);
    }
#endif
#if 201103L <= YY_CPLUSPLUS
    static symbol_type make_SECOND(location_type l) {
        return symbol_type(token::SECOND, std::move(l));
    }
#else
    static symbol_type make_SECOND(const location_type& l) {
        return symbol_type(token::SECOND, l);
    }
#endif
#if 201103L <= YY_CPLUSPLUS
    static symbol_type make_SET_DIFFERENCE(location_type l) {
        return symbol_type(token::SET_DIFFERENCE, std::move(l));
    }
#else
    static symbol_type make_SET_DIFFERENCE(const location_type& l) {
        return symbol_type(token::SET_DIFFERENCE, l);
    }
#endif
#if 201103L <= YY_CPLUSPLUS
    static symbol_type make_SET_EQUALS(location_type l) {
        return symbol_type(token::SET_EQUALS, std::move(l));
    }
#else
    static symbol_type make_SET_EQUALS(const location_type& l) {
        return symbol_type(token::SET_EQUALS, l);
    }
#endif
#if 201103L <= YY_CPLUSPLUS
    static symbol_type make_SET_INTERSECTION(location_type l) {
        return symbol_type(token::SET_INTERSECTION, std::move(l));
    }
#else
    static symbol_type make_SET_INTERSECTION(const location_type& l) {
        return symbol_type(token::SET_INTERSECTION, l);
    }
#endif
#if 201103L <= YY_CPLUSPLUS
    static symbol_type make_SET_IS_SUBSET(location_type l) {
        return symbol_type(token::SET_IS_SUBSET, std::move(l));
    }
#else
    static symbol_type make_SET_IS_SUBSET(const location_type& l) {
        return symbol_type(token::SET_IS_SUBSET, l);
    }
#endif
#if 201103L <= YY_CPLUSPLUS
    static symbol_type make_SET_UNION(location_type l) {
        return symbol_type(token::SET_UNION, std::move(l));
    }
#else
    static symbol_type make_SET_UNION(const location_type& l) {
        return symbol_type(token::SET_UNION, l);
    }
#endif
#if 201103L <= YY_CPLUSPLUS
    static symbol_type make_SIN(location_type l) {
        return symbol_type(token::SIN, std::move(l));
    }
#else
    static symbol_type make_SIN(const location_type& l) {
        return symbol_type(token::SIN, l);
    }
#endif
#if 201103L <= YY_CPLUSPLUS
    static symbol_type make_SINH(location_type l) {
        return symbol_type(token::SINH, std::move(l));
    }
#else
    static symbol_type make_SINH(const location_type& l) {
        return symbol_type(token::SINH, l);
    }
#endif
#if 201103L <= YY_CPLUSPLUS
    static symbol_type make_SLICE(location_type l) {
        return symbol_type(token::SLICE, std::move(l));
    }
#else
    static symbol_type make_SLICE(const location_type& l) {
        return symbol_type(token::SLICE, l);
    }
#endif
#if 201103L <= YY_CPLUSPLUS
    static symbol_type make_SORT_KEY(location_type l) {
        return symbol_type(token::SORT_KEY, std::move(l));
    }
#else
    static symbol_type make_SORT_KEY(const location_type& l) {
        return symbol_type(token::SORT_KEY, l);
    }
#endif
#if 201103L <= YY_CPLUSPLUS
    static symbol_type make_SPLIT(location_type l) {
        return symbol_type(token::SPLIT, std::move(l));
    }
#else
    static symbol_type make_SPLIT(const location_type& l) {
        return symbol_type(token::SPLIT, l);
    }
#endif
#if 201103L <= YY_CPLUSPLUS
    static symbol_type make_SQRT(location_type l) {
        return symbol_type(token::SQRT, std::move(l));
    }
#else
    static symbol_type make_SQRT(const location_type& l) {
        return symbol_type(token::SQRT, l);
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
    static symbol_type make_STAGE_MATCH(location_type l) {
        return symbol_type(token::STAGE_MATCH, std::move(l));
    }
#else
    static symbol_type make_STAGE_MATCH(const location_type& l) {
        return symbol_type(token::STAGE_MATCH, l);
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
    static symbol_type make_START_ARRAY(location_type l) {
        return symbol_type(token::START_ARRAY, std::move(l));
    }
#else
    static symbol_type make_START_ARRAY(const location_type& l) {
        return symbol_type(token::START_ARRAY, l);
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
    static symbol_type make_STR_CASE_CMP(location_type l) {
        return symbol_type(token::STR_CASE_CMP, std::move(l));
    }
#else
    static symbol_type make_STR_CASE_CMP(const location_type& l) {
        return symbol_type(token::STR_CASE_CMP, l);
    }
#endif
#if 201103L <= YY_CPLUSPLUS
    static symbol_type make_STR_LEN_BYTES(location_type l) {
        return symbol_type(token::STR_LEN_BYTES, std::move(l));
    }
#else
    static symbol_type make_STR_LEN_BYTES(const location_type& l) {
        return symbol_type(token::STR_LEN_BYTES, l);
    }
#endif
#if 201103L <= YY_CPLUSPLUS
    static symbol_type make_STR_LEN_CP(location_type l) {
        return symbol_type(token::STR_LEN_CP, std::move(l));
    }
#else
    static symbol_type make_STR_LEN_CP(const location_type& l) {
        return symbol_type(token::STR_LEN_CP, l);
    }
#endif
#if 201103L <= YY_CPLUSPLUS
    static symbol_type make_SUBSTR(location_type l) {
        return symbol_type(token::SUBSTR, std::move(l));
    }
#else
    static symbol_type make_SUBSTR(const location_type& l) {
        return symbol_type(token::SUBSTR, l);
    }
#endif
#if 201103L <= YY_CPLUSPLUS
    static symbol_type make_SUBSTR_BYTES(location_type l) {
        return symbol_type(token::SUBSTR_BYTES, std::move(l));
    }
#else
    static symbol_type make_SUBSTR_BYTES(const location_type& l) {
        return symbol_type(token::SUBSTR_BYTES, l);
    }
#endif
#if 201103L <= YY_CPLUSPLUS
    static symbol_type make_SUBSTR_CP(location_type l) {
        return symbol_type(token::SUBSTR_CP, std::move(l));
    }
#else
    static symbol_type make_SUBSTR_CP(const location_type& l) {
        return symbol_type(token::SUBSTR_CP, l);
    }
#endif
#if 201103L <= YY_CPLUSPLUS
    static symbol_type make_SUBTRACT(location_type l) {
        return symbol_type(token::SUBTRACT, std::move(l));
    }
#else
    static symbol_type make_SUBTRACT(const location_type& l) {
        return symbol_type(token::SUBTRACT, l);
    }
#endif
#if 201103L <= YY_CPLUSPLUS
    static symbol_type make_TAN(location_type l) {
        return symbol_type(token::TAN, std::move(l));
    }
#else
    static symbol_type make_TAN(const location_type& l) {
        return symbol_type(token::TAN, l);
    }
#endif
#if 201103L <= YY_CPLUSPLUS
    static symbol_type make_TANH(location_type l) {
        return symbol_type(token::TANH, std::move(l));
    }
#else
    static symbol_type make_TANH(const location_type& l) {
        return symbol_type(token::TANH, l);
    }
#endif
#if 201103L <= YY_CPLUSPLUS
    static symbol_type make_TEXT(location_type l) {
        return symbol_type(token::TEXT, std::move(l));
    }
#else
    static symbol_type make_TEXT(const location_type& l) {
        return symbol_type(token::TEXT, l);
    }
#endif
#if 201103L <= YY_CPLUSPLUS
    static symbol_type make_TEXT_SCORE(location_type l) {
        return symbol_type(token::TEXT_SCORE, std::move(l));
    }
#else
    static symbol_type make_TEXT_SCORE(const location_type& l) {
        return symbol_type(token::TEXT_SCORE, l);
    }
#endif
#if 201103L <= YY_CPLUSPLUS
    static symbol_type make_TO_BOOL(location_type l) {
        return symbol_type(token::TO_BOOL, std::move(l));
    }
#else
    static symbol_type make_TO_BOOL(const location_type& l) {
        return symbol_type(token::TO_BOOL, l);
    }
#endif
#if 201103L <= YY_CPLUSPLUS
    static symbol_type make_TO_DATE(location_type l) {
        return symbol_type(token::TO_DATE, std::move(l));
    }
#else
    static symbol_type make_TO_DATE(const location_type& l) {
        return symbol_type(token::TO_DATE, l);
    }
#endif
#if 201103L <= YY_CPLUSPLUS
    static symbol_type make_TO_DECIMAL(location_type l) {
        return symbol_type(token::TO_DECIMAL, std::move(l));
    }
#else
    static symbol_type make_TO_DECIMAL(const location_type& l) {
        return symbol_type(token::TO_DECIMAL, l);
    }
#endif
#if 201103L <= YY_CPLUSPLUS
    static symbol_type make_TO_DOUBLE(location_type l) {
        return symbol_type(token::TO_DOUBLE, std::move(l));
    }
#else
    static symbol_type make_TO_DOUBLE(const location_type& l) {
        return symbol_type(token::TO_DOUBLE, l);
    }
#endif
#if 201103L <= YY_CPLUSPLUS
    static symbol_type make_TO_INT(location_type l) {
        return symbol_type(token::TO_INT, std::move(l));
    }
#else
    static symbol_type make_TO_INT(const location_type& l) {
        return symbol_type(token::TO_INT, l);
    }
#endif
#if 201103L <= YY_CPLUSPLUS
    static symbol_type make_TO_LONG(location_type l) {
        return symbol_type(token::TO_LONG, std::move(l));
    }
#else
    static symbol_type make_TO_LONG(const location_type& l) {
        return symbol_type(token::TO_LONG, l);
    }
#endif
#if 201103L <= YY_CPLUSPLUS
    static symbol_type make_TO_LOWER(location_type l) {
        return symbol_type(token::TO_LOWER, std::move(l));
    }
#else
    static symbol_type make_TO_LOWER(const location_type& l) {
        return symbol_type(token::TO_LOWER, l);
    }
#endif
#if 201103L <= YY_CPLUSPLUS
    static symbol_type make_TO_OBJECT_ID(location_type l) {
        return symbol_type(token::TO_OBJECT_ID, std::move(l));
    }
#else
    static symbol_type make_TO_OBJECT_ID(const location_type& l) {
        return symbol_type(token::TO_OBJECT_ID, l);
    }
#endif
#if 201103L <= YY_CPLUSPLUS
    static symbol_type make_TO_STRING(location_type l) {
        return symbol_type(token::TO_STRING, std::move(l));
    }
#else
    static symbol_type make_TO_STRING(const location_type& l) {
        return symbol_type(token::TO_STRING, l);
    }
#endif
#if 201103L <= YY_CPLUSPLUS
    static symbol_type make_TO_UPPER(location_type l) {
        return symbol_type(token::TO_UPPER, std::move(l));
    }
#else
    static symbol_type make_TO_UPPER(const location_type& l) {
        return symbol_type(token::TO_UPPER, l);
    }
#endif
#if 201103L <= YY_CPLUSPLUS
    static symbol_type make_TRIM(location_type l) {
        return symbol_type(token::TRIM, std::move(l));
    }
#else
    static symbol_type make_TRIM(const location_type& l) {
        return symbol_type(token::TRIM, l);
    }
#endif
#if 201103L <= YY_CPLUSPLUS
    static symbol_type make_TRUNC(location_type l) {
        return symbol_type(token::TRUNC, std::move(l));
    }
#else
    static symbol_type make_TRUNC(const location_type& l) {
        return symbol_type(token::TRUNC, l);
    }
#endif
#if 201103L <= YY_CPLUSPLUS
    static symbol_type make_TYPE(location_type l) {
        return symbol_type(token::TYPE, std::move(l));
    }
#else
    static symbol_type make_TYPE(const location_type& l) {
        return symbol_type(token::TYPE, l);
    }
#endif
#if 201103L <= YY_CPLUSPLUS
    static symbol_type make_WEEK(location_type l) {
        return symbol_type(token::WEEK, std::move(l));
    }
#else
    static symbol_type make_WEEK(const location_type& l) {
        return symbol_type(token::WEEK, l);
    }
#endif
#if 201103L <= YY_CPLUSPLUS
    static symbol_type make_WHERE(location_type l) {
        return symbol_type(token::WHERE, std::move(l));
    }
#else
    static symbol_type make_WHERE(const location_type& l) {
        return symbol_type(token::WHERE, l);
    }
#endif
#if 201103L <= YY_CPLUSPLUS
    static symbol_type make_YEAR(location_type l) {
        return symbol_type(token::YEAR, std::move(l));
    }
#else
    static symbol_type make_YEAR(const location_type& l) {
        return symbol_type(token::YEAR, l);
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
    static symbol_type make_DOTTED_FIELDNAME(std::vector<std::string> v, location_type l) {
        return symbol_type(token::DOTTED_FIELDNAME, std::move(v), std::move(l));
    }
#else
    static symbol_type make_DOTTED_FIELDNAME(const std::vector<std::string>& v,
                                             const location_type& l) {
        return symbol_type(token::DOTTED_FIELDNAME, v, l);
    }
#endif
#if 201103L <= YY_CPLUSPLUS
    static symbol_type make_DOLLAR_PREF_FIELDNAME(std::string v, location_type l) {
        return symbol_type(token::DOLLAR_PREF_FIELDNAME, std::move(v), std::move(l));
    }
#else
    static symbol_type make_DOLLAR_PREF_FIELDNAME(const std::string& v, const location_type& l) {
        return symbol_type(token::DOLLAR_PREF_FIELDNAME, v, l);
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
    static symbol_type make_DOLLAR_STRING(std::string v, location_type l) {
        return symbol_type(token::DOLLAR_STRING, std::move(v), std::move(l));
    }
#else
    static symbol_type make_DOLLAR_STRING(const std::string& v, const location_type& l) {
        return symbol_type(token::DOLLAR_STRING, v, l);
    }
#endif
#if 201103L <= YY_CPLUSPLUS
    static symbol_type make_DOLLAR_DOLLAR_STRING(std::string v, location_type l) {
        return symbol_type(token::DOLLAR_DOLLAR_STRING, std::move(v), std::move(l));
    }
#else
    static symbol_type make_DOLLAR_DOLLAR_STRING(const std::string& v, const location_type& l) {
        return symbol_type(token::DOLLAR_DOLLAR_STRING, v, l);
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
    static symbol_type make_INT_OTHER(int v, location_type l) {
        return symbol_type(token::INT_OTHER, std::move(v), std::move(l));
    }
#else
    static symbol_type make_INT_OTHER(const int& v, const location_type& l) {
        return symbol_type(token::INT_OTHER, v, l);
    }
#endif
#if 201103L <= YY_CPLUSPLUS
    static symbol_type make_LONG_OTHER(long long v, location_type l) {
        return symbol_type(token::LONG_OTHER, std::move(v), std::move(l));
    }
#else
    static symbol_type make_LONG_OTHER(const long long& v, const location_type& l) {
        return symbol_type(token::LONG_OTHER, v, l);
    }
#endif
#if 201103L <= YY_CPLUSPLUS
    static symbol_type make_DOUBLE_OTHER(double v, location_type l) {
        return symbol_type(token::DOUBLE_OTHER, std::move(v), std::move(l));
    }
#else
    static symbol_type make_DOUBLE_OTHER(const double& v, const location_type& l) {
        return symbol_type(token::DOUBLE_OTHER, v, l);
    }
#endif
#if 201103L <= YY_CPLUSPLUS
    static symbol_type make_DECIMAL_OTHER(Decimal128 v, location_type l) {
        return symbol_type(token::DECIMAL_OTHER, std::move(v), std::move(l));
    }
#else
    static symbol_type make_DECIMAL_OTHER(const Decimal128& v, const location_type& l) {
        return symbol_type(token::DECIMAL_OTHER, v, l);
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
#if 201103L <= YY_CPLUSPLUS
    static symbol_type make_START_PIPELINE(location_type l) {
        return symbol_type(token::START_PIPELINE, std::move(l));
    }
#else
    static symbol_type make_START_PIPELINE(const location_type& l) {
        return symbol_type(token::START_PIPELINE, l);
    }
#endif
#if 201103L <= YY_CPLUSPLUS
    static symbol_type make_START_MATCH(location_type l) {
        return symbol_type(token::START_MATCH, std::move(l));
    }
#else
    static symbol_type make_START_MATCH(const location_type& l) {
        return symbol_type(token::START_MATCH, l);
    }
#endif
#if 201103L <= YY_CPLUSPLUS
    static symbol_type make_START_PROJECT(location_type l) {
        return symbol_type(token::START_PROJECT, std::move(l));
    }
#else
    static symbol_type make_START_PROJECT(const location_type& l) {
        return symbol_type(token::START_PROJECT, l);
    }
#endif
#if 201103L <= YY_CPLUSPLUS
    static symbol_type make_START_SORT(location_type l) {
        return symbol_type(token::START_SORT, std::move(l));
    }
#else
    static symbol_type make_START_SORT(const location_type& l) {
        return symbol_type(token::START_SORT, l);
    }
#endif


private:
    /// This class is not copyable.
    ParserGen(const ParserGen&);
    ParserGen& operator=(const ParserGen&);

    /// Stored state numbers (used for stacks).
    typedef short state_type;

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
    static const short yytable_ninf_;

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
    static const short yydefact_[];

    // YYPGOTO[NTERM-NUM].
    static const short yypgoto_[];

    // YYDEFGOTO[NTERM-NUM].
    static const short yydefgoto_[];

    // YYTABLE[YYPACT[STATE-NUM]] -- What to do in state STATE-NUM.  If
    // positive, shift that token.  If negative, reduce the rule whose
    // number is the opposite.  If YYTABLE_NINF, syntax error.
    static const short yytable_[];

    static const short yycheck_[];

    // YYSTOS[STATE-NUM] -- The (internal number of the) accessing
    // symbol of state STATE-NUM.
    static const short yystos_[];

    // YYR1[YYN] -- Symbol number of symbol that rule YYN derives.
    static const short yyr1_[];

    // YYR2[YYN] -- Number of symbols on the right hand side of rule YYN.
    static const signed char yyr2_[];


    /// Convert the symbol name \a n to a form suitable for a diagnostic.
    static std::string yytnamerr_(const char* n);


    /// For a symbol, its name in clear.
    static const char* const yytname_[];
#if YYDEBUG
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
        yylast_ = 4831,   ///< Last index in yytable_.
        yynnts_ = 247,    ///< Number of nonterminal symbols.
        yyfinal_ = 14,    ///< Termination state number.
        yyntokens_ = 203  ///< Number of tokens.
    };


    // User arguments.
    BSONLexer& lexer;
    CNode* cst;
};

inline ParserGen::token_number_type ParserGen::yytranslate_(int t) {
    return static_cast<token_number_type>(t);
}

// basic_symbol.
#if 201103L <= YY_CPLUSPLUS
template <typename Base>
ParserGen::basic_symbol<Base>::basic_symbol(basic_symbol&& that)
    : Base(std::move(that)), value(), location(std::move(that.location)) {
    switch (this->type_get()) {
        case 182:  // "BinData"
            value.move<BSONBinData>(std::move(that.value));
            break;

        case 189:  // "Code"
            value.move<BSONCode>(std::move(that.value));
            break;

        case 191:  // "CodeWScope"
            value.move<BSONCodeWScope>(std::move(that.value));
            break;

        case 188:  // "dbPointer"
            value.move<BSONDBRef>(std::move(that.value));
            break;

        case 187:  // "regex"
            value.move<BSONRegEx>(std::move(that.value));
            break;

        case 190:  // "Symbol"
            value.move<BSONSymbol>(std::move(that.value));
            break;

        case 223:  // dbPointer
        case 224:  // javascript
        case 225:  // symbol
        case 226:  // javascriptWScope
        case 227:  // int
        case 228:  // timestamp
        case 229:  // long
        case 230:  // double
        case 231:  // decimal
        case 232:  // minKey
        case 233:  // maxKey
        case 234:  // value
        case 235:  // string
        case 236:  // aggregationFieldPath
        case 237:  // binary
        case 238:  // undefined
        case 239:  // objectId
        case 240:  // bool
        case 241:  // date
        case 242:  // null
        case 243:  // regex
        case 244:  // simpleValue
        case 245:  // compoundValue
        case 246:  // valueArray
        case 247:  // valueObject
        case 248:  // valueFields
        case 249:  // variable
        case 250:  // typeArray
        case 251:  // typeValue
        case 252:  // pipeline
        case 253:  // stageList
        case 254:  // stage
        case 255:  // inhibitOptimization
        case 256:  // unionWith
        case 257:  // skip
        case 258:  // limit
        case 259:  // matchStage
        case 260:  // project
        case 261:  // sample
        case 262:  // aggregationProjectFields
        case 263:  // aggregationProjectionObjectFields
        case 264:  // topLevelAggregationProjection
        case 265:  // aggregationProjection
        case 266:  // projectionCommon
        case 267:  // aggregationProjectionObject
        case 268:  // num
        case 269:  // expression
        case 270:  // exprFixedTwoArg
        case 271:  // exprFixedThreeArg
        case 272:  // slice
        case 273:  // expressionArray
        case 274:  // expressionObject
        case 275:  // expressionFields
        case 276:  // maths
        case 277:  // meta
        case 278:  // add
        case 279:  // boolExprs
        case 280:  // and
        case 281:  // or
        case 282:  // not
        case 283:  // literalEscapes
        case 284:  // const
        case 285:  // literal
        case 286:  // stringExps
        case 287:  // concat
        case 288:  // dateFromString
        case 289:  // dateToString
        case 290:  // indexOfBytes
        case 291:  // indexOfCP
        case 292:  // ltrim
        case 293:  // regexFind
        case 294:  // regexFindAll
        case 295:  // regexMatch
        case 296:  // regexArgs
        case 297:  // replaceOne
        case 298:  // replaceAll
        case 299:  // rtrim
        case 300:  // split
        case 301:  // strLenBytes
        case 302:  // strLenCP
        case 303:  // strcasecmp
        case 304:  // substr
        case 305:  // substrBytes
        case 306:  // substrCP
        case 307:  // toLower
        case 308:  // toUpper
        case 309:  // trim
        case 310:  // compExprs
        case 311:  // cmp
        case 312:  // eq
        case 313:  // gt
        case 314:  // gte
        case 315:  // lt
        case 316:  // lte
        case 317:  // ne
        case 318:  // dateExps
        case 319:  // dateFromParts
        case 320:  // dateToParts
        case 321:  // dayOfMonth
        case 322:  // dayOfWeek
        case 323:  // dayOfYear
        case 324:  // hour
        case 325:  // isoDayOfWeek
        case 326:  // isoWeek
        case 327:  // isoWeekYear
        case 328:  // millisecond
        case 329:  // minute
        case 330:  // month
        case 331:  // second
        case 332:  // week
        case 333:  // year
        case 334:  // typeExpression
        case 335:  // convert
        case 336:  // toBool
        case 337:  // toDate
        case 338:  // toDecimal
        case 339:  // toDouble
        case 340:  // toInt
        case 341:  // toLong
        case 342:  // toObjectId
        case 343:  // toString
        case 344:  // type
        case 345:  // abs
        case 346:  // ceil
        case 347:  // divide
        case 348:  // exponent
        case 349:  // floor
        case 350:  // ln
        case 351:  // log
        case 352:  // logten
        case 353:  // mod
        case 354:  // multiply
        case 355:  // pow
        case 356:  // round
        case 357:  // sqrt
        case 358:  // subtract
        case 359:  // trunc
        case 378:  // setExpression
        case 379:  // allElementsTrue
        case 380:  // anyElementTrue
        case 381:  // setDifference
        case 382:  // setEquals
        case 383:  // setIntersection
        case 384:  // setIsSubset
        case 385:  // setUnion
        case 386:  // trig
        case 387:  // sin
        case 388:  // cos
        case 389:  // tan
        case 390:  // sinh
        case 391:  // cosh
        case 392:  // tanh
        case 393:  // asin
        case 394:  // acos
        case 395:  // atan
        case 396:  // asinh
        case 397:  // acosh
        case 398:  // atanh
        case 399:  // atan2
        case 400:  // degreesToRadians
        case 401:  // radiansToDegrees
        case 402:  // nonArrayExpression
        case 403:  // nonArrayCompoundExpression
        case 404:  // aggregationOperator
        case 405:  // aggregationOperatorWithoutSlice
        case 406:  // expressionSingletonArray
        case 407:  // singleArgExpression
        case 408:  // nonArrayNonObjExpression
        case 409:  // matchExpression
        case 410:  // predicates
        case 411:  // compoundMatchExprs
        case 412:  // predValue
        case 413:  // additionalExprs
        case 428:  // textArgCaseSensitive
        case 429:  // textArgDiacriticSensitive
        case 430:  // textArgLanguage
        case 431:  // textArgSearch
        case 432:  // findProject
        case 433:  // findProjectFields
        case 434:  // topLevelFindProjection
        case 435:  // findProjection
        case 436:  // findProjectionSlice
        case 437:  // elemMatch
        case 438:  // findProjectionObject
        case 439:  // findProjectionObjectFields
        case 442:  // sortSpecs
        case 443:  // specList
        case 444:  // metaSort
        case 445:  // oneOrNegOne
        case 446:  // metaSortKeyword
            value.move<CNode>(std::move(that.value));
            break;

        case 204:  // aggregationProjectionFieldname
        case 205:  // projectionFieldname
        case 206:  // expressionFieldname
        case 207:  // stageAsUserFieldname
        case 208:  // argAsUserFieldname
        case 209:  // argAsProjectionPath
        case 210:  // aggExprAsUserFieldname
        case 211:  // invariableUserFieldname
        case 212:  // sortFieldname
        case 213:  // idAsUserFieldname
        case 214:  // elemMatchAsUserFieldname
        case 215:  // idAsProjectionPath
        case 216:  // valueFieldname
        case 217:  // predFieldname
        case 423:  // logicalExprField
            value.move<CNode::Fieldname>(std::move(that.value));
            break;

        case 185:  // "Date"
            value.move<Date_t>(std::move(that.value));
            break;

        case 195:  // "arbitrary decimal"
            value.move<Decimal128>(std::move(that.value));
            break;

        case 184:  // "ObjectID"
            value.move<OID>(std::move(that.value));
            break;

        case 196:  // "Timestamp"
            value.move<Timestamp>(std::move(that.value));
            break;

        case 198:  // "maxKey"
            value.move<UserMaxKey>(std::move(that.value));
            break;

        case 197:  // "minKey"
            value.move<UserMinKey>(std::move(that.value));
            break;

        case 186:  // "null"
            value.move<UserNull>(std::move(that.value));
            break;

        case 183:  // "undefined"
            value.move<UserUndefined>(std::move(that.value));
            break;

        case 194:  // "arbitrary double"
            value.move<double>(std::move(that.value));
            break;

        case 192:  // "arbitrary integer"
            value.move<int>(std::move(that.value));
            break;

        case 193:  // "arbitrary long"
            value.move<long long>(std::move(that.value));
            break;

        case 218:  // aggregationProjectField
        case 219:  // aggregationProjectionObjectField
        case 220:  // expressionField
        case 221:  // valueField
        case 360:  // onErrorArg
        case 361:  // onNullArg
        case 362:  // formatArg
        case 363:  // timezoneArg
        case 364:  // charsArg
        case 365:  // optionsArg
        case 366:  // hourArg
        case 367:  // minuteArg
        case 368:  // secondArg
        case 369:  // millisecondArg
        case 370:  // dayArg
        case 371:  // isoWeekArg
        case 372:  // iso8601Arg
        case 373:  // monthArg
        case 374:  // isoDayOfWeekArg
        case 414:  // predicate
        case 415:  // fieldPredicate
        case 416:  // logicalExpr
        case 417:  // operatorExpression
        case 418:  // notExpr
        case 419:  // matchMod
        case 420:  // existsExpr
        case 421:  // typeExpr
        case 422:  // commentExpr
        case 425:  // matchExpr
        case 426:  // matchText
        case 427:  // matchWhere
        case 440:  // findProjectField
        case 441:  // findProjectionObjectField
        case 447:  // sortSpec
            value.move<std::pair<CNode::Fieldname, CNode>>(std::move(that.value));
            break;

        case 176:  // "fieldname"
        case 178:  // "$-prefixed fieldname"
        case 179:  // "string"
        case 180:  // "$-prefixed string"
        case 181:  // "$$-prefixed string"
        case 222:  // arg
            value.move<std::string>(std::move(that.value));
            break;

        case 375:  // expressions
        case 376:  // values
        case 377:  // exprZeroToTwo
        case 424:  // typeValues
            value.move<std::vector<CNode>>(std::move(that.value));
            break;

        case 177:  // "fieldname containing dotted path"
            value.move<std::vector<std::string>>(std::move(that.value));
            break;

        default:
            break;
    }
}
#endif

template <typename Base>
ParserGen::basic_symbol<Base>::basic_symbol(const basic_symbol& that)
    : Base(that), value(), location(that.location) {
    switch (this->type_get()) {
        case 182:  // "BinData"
            value.copy<BSONBinData>(YY_MOVE(that.value));
            break;

        case 189:  // "Code"
            value.copy<BSONCode>(YY_MOVE(that.value));
            break;

        case 191:  // "CodeWScope"
            value.copy<BSONCodeWScope>(YY_MOVE(that.value));
            break;

        case 188:  // "dbPointer"
            value.copy<BSONDBRef>(YY_MOVE(that.value));
            break;

        case 187:  // "regex"
            value.copy<BSONRegEx>(YY_MOVE(that.value));
            break;

        case 190:  // "Symbol"
            value.copy<BSONSymbol>(YY_MOVE(that.value));
            break;

        case 223:  // dbPointer
        case 224:  // javascript
        case 225:  // symbol
        case 226:  // javascriptWScope
        case 227:  // int
        case 228:  // timestamp
        case 229:  // long
        case 230:  // double
        case 231:  // decimal
        case 232:  // minKey
        case 233:  // maxKey
        case 234:  // value
        case 235:  // string
        case 236:  // aggregationFieldPath
        case 237:  // binary
        case 238:  // undefined
        case 239:  // objectId
        case 240:  // bool
        case 241:  // date
        case 242:  // null
        case 243:  // regex
        case 244:  // simpleValue
        case 245:  // compoundValue
        case 246:  // valueArray
        case 247:  // valueObject
        case 248:  // valueFields
        case 249:  // variable
        case 250:  // typeArray
        case 251:  // typeValue
        case 252:  // pipeline
        case 253:  // stageList
        case 254:  // stage
        case 255:  // inhibitOptimization
        case 256:  // unionWith
        case 257:  // skip
        case 258:  // limit
        case 259:  // matchStage
        case 260:  // project
        case 261:  // sample
        case 262:  // aggregationProjectFields
        case 263:  // aggregationProjectionObjectFields
        case 264:  // topLevelAggregationProjection
        case 265:  // aggregationProjection
        case 266:  // projectionCommon
        case 267:  // aggregationProjectionObject
        case 268:  // num
        case 269:  // expression
        case 270:  // exprFixedTwoArg
        case 271:  // exprFixedThreeArg
        case 272:  // slice
        case 273:  // expressionArray
        case 274:  // expressionObject
        case 275:  // expressionFields
        case 276:  // maths
        case 277:  // meta
        case 278:  // add
        case 279:  // boolExprs
        case 280:  // and
        case 281:  // or
        case 282:  // not
        case 283:  // literalEscapes
        case 284:  // const
        case 285:  // literal
        case 286:  // stringExps
        case 287:  // concat
        case 288:  // dateFromString
        case 289:  // dateToString
        case 290:  // indexOfBytes
        case 291:  // indexOfCP
        case 292:  // ltrim
        case 293:  // regexFind
        case 294:  // regexFindAll
        case 295:  // regexMatch
        case 296:  // regexArgs
        case 297:  // replaceOne
        case 298:  // replaceAll
        case 299:  // rtrim
        case 300:  // split
        case 301:  // strLenBytes
        case 302:  // strLenCP
        case 303:  // strcasecmp
        case 304:  // substr
        case 305:  // substrBytes
        case 306:  // substrCP
        case 307:  // toLower
        case 308:  // toUpper
        case 309:  // trim
        case 310:  // compExprs
        case 311:  // cmp
        case 312:  // eq
        case 313:  // gt
        case 314:  // gte
        case 315:  // lt
        case 316:  // lte
        case 317:  // ne
        case 318:  // dateExps
        case 319:  // dateFromParts
        case 320:  // dateToParts
        case 321:  // dayOfMonth
        case 322:  // dayOfWeek
        case 323:  // dayOfYear
        case 324:  // hour
        case 325:  // isoDayOfWeek
        case 326:  // isoWeek
        case 327:  // isoWeekYear
        case 328:  // millisecond
        case 329:  // minute
        case 330:  // month
        case 331:  // second
        case 332:  // week
        case 333:  // year
        case 334:  // typeExpression
        case 335:  // convert
        case 336:  // toBool
        case 337:  // toDate
        case 338:  // toDecimal
        case 339:  // toDouble
        case 340:  // toInt
        case 341:  // toLong
        case 342:  // toObjectId
        case 343:  // toString
        case 344:  // type
        case 345:  // abs
        case 346:  // ceil
        case 347:  // divide
        case 348:  // exponent
        case 349:  // floor
        case 350:  // ln
        case 351:  // log
        case 352:  // logten
        case 353:  // mod
        case 354:  // multiply
        case 355:  // pow
        case 356:  // round
        case 357:  // sqrt
        case 358:  // subtract
        case 359:  // trunc
        case 378:  // setExpression
        case 379:  // allElementsTrue
        case 380:  // anyElementTrue
        case 381:  // setDifference
        case 382:  // setEquals
        case 383:  // setIntersection
        case 384:  // setIsSubset
        case 385:  // setUnion
        case 386:  // trig
        case 387:  // sin
        case 388:  // cos
        case 389:  // tan
        case 390:  // sinh
        case 391:  // cosh
        case 392:  // tanh
        case 393:  // asin
        case 394:  // acos
        case 395:  // atan
        case 396:  // asinh
        case 397:  // acosh
        case 398:  // atanh
        case 399:  // atan2
        case 400:  // degreesToRadians
        case 401:  // radiansToDegrees
        case 402:  // nonArrayExpression
        case 403:  // nonArrayCompoundExpression
        case 404:  // aggregationOperator
        case 405:  // aggregationOperatorWithoutSlice
        case 406:  // expressionSingletonArray
        case 407:  // singleArgExpression
        case 408:  // nonArrayNonObjExpression
        case 409:  // matchExpression
        case 410:  // predicates
        case 411:  // compoundMatchExprs
        case 412:  // predValue
        case 413:  // additionalExprs
        case 428:  // textArgCaseSensitive
        case 429:  // textArgDiacriticSensitive
        case 430:  // textArgLanguage
        case 431:  // textArgSearch
        case 432:  // findProject
        case 433:  // findProjectFields
        case 434:  // topLevelFindProjection
        case 435:  // findProjection
        case 436:  // findProjectionSlice
        case 437:  // elemMatch
        case 438:  // findProjectionObject
        case 439:  // findProjectionObjectFields
        case 442:  // sortSpecs
        case 443:  // specList
        case 444:  // metaSort
        case 445:  // oneOrNegOne
        case 446:  // metaSortKeyword
            value.copy<CNode>(YY_MOVE(that.value));
            break;

        case 204:  // aggregationProjectionFieldname
        case 205:  // projectionFieldname
        case 206:  // expressionFieldname
        case 207:  // stageAsUserFieldname
        case 208:  // argAsUserFieldname
        case 209:  // argAsProjectionPath
        case 210:  // aggExprAsUserFieldname
        case 211:  // invariableUserFieldname
        case 212:  // sortFieldname
        case 213:  // idAsUserFieldname
        case 214:  // elemMatchAsUserFieldname
        case 215:  // idAsProjectionPath
        case 216:  // valueFieldname
        case 217:  // predFieldname
        case 423:  // logicalExprField
            value.copy<CNode::Fieldname>(YY_MOVE(that.value));
            break;

        case 185:  // "Date"
            value.copy<Date_t>(YY_MOVE(that.value));
            break;

        case 195:  // "arbitrary decimal"
            value.copy<Decimal128>(YY_MOVE(that.value));
            break;

        case 184:  // "ObjectID"
            value.copy<OID>(YY_MOVE(that.value));
            break;

        case 196:  // "Timestamp"
            value.copy<Timestamp>(YY_MOVE(that.value));
            break;

        case 198:  // "maxKey"
            value.copy<UserMaxKey>(YY_MOVE(that.value));
            break;

        case 197:  // "minKey"
            value.copy<UserMinKey>(YY_MOVE(that.value));
            break;

        case 186:  // "null"
            value.copy<UserNull>(YY_MOVE(that.value));
            break;

        case 183:  // "undefined"
            value.copy<UserUndefined>(YY_MOVE(that.value));
            break;

        case 194:  // "arbitrary double"
            value.copy<double>(YY_MOVE(that.value));
            break;

        case 192:  // "arbitrary integer"
            value.copy<int>(YY_MOVE(that.value));
            break;

        case 193:  // "arbitrary long"
            value.copy<long long>(YY_MOVE(that.value));
            break;

        case 218:  // aggregationProjectField
        case 219:  // aggregationProjectionObjectField
        case 220:  // expressionField
        case 221:  // valueField
        case 360:  // onErrorArg
        case 361:  // onNullArg
        case 362:  // formatArg
        case 363:  // timezoneArg
        case 364:  // charsArg
        case 365:  // optionsArg
        case 366:  // hourArg
        case 367:  // minuteArg
        case 368:  // secondArg
        case 369:  // millisecondArg
        case 370:  // dayArg
        case 371:  // isoWeekArg
        case 372:  // iso8601Arg
        case 373:  // monthArg
        case 374:  // isoDayOfWeekArg
        case 414:  // predicate
        case 415:  // fieldPredicate
        case 416:  // logicalExpr
        case 417:  // operatorExpression
        case 418:  // notExpr
        case 419:  // matchMod
        case 420:  // existsExpr
        case 421:  // typeExpr
        case 422:  // commentExpr
        case 425:  // matchExpr
        case 426:  // matchText
        case 427:  // matchWhere
        case 440:  // findProjectField
        case 441:  // findProjectionObjectField
        case 447:  // sortSpec
            value.copy<std::pair<CNode::Fieldname, CNode>>(YY_MOVE(that.value));
            break;

        case 176:  // "fieldname"
        case 178:  // "$-prefixed fieldname"
        case 179:  // "string"
        case 180:  // "$-prefixed string"
        case 181:  // "$$-prefixed string"
        case 222:  // arg
            value.copy<std::string>(YY_MOVE(that.value));
            break;

        case 375:  // expressions
        case 376:  // values
        case 377:  // exprZeroToTwo
        case 424:  // typeValues
            value.copy<std::vector<CNode>>(YY_MOVE(that.value));
            break;

        case 177:  // "fieldname containing dotted path"
            value.copy<std::vector<std::string>>(YY_MOVE(that.value));
            break;

        default:
            break;
    }
}


template <typename Base>
bool ParserGen::basic_symbol<Base>::empty() const YY_NOEXCEPT {
    return Base::type_get() == empty_symbol;
}

template <typename Base>
void ParserGen::basic_symbol<Base>::move(basic_symbol& s) {
    super_type::move(s);
    switch (this->type_get()) {
        case 182:  // "BinData"
            value.move<BSONBinData>(YY_MOVE(s.value));
            break;

        case 189:  // "Code"
            value.move<BSONCode>(YY_MOVE(s.value));
            break;

        case 191:  // "CodeWScope"
            value.move<BSONCodeWScope>(YY_MOVE(s.value));
            break;

        case 188:  // "dbPointer"
            value.move<BSONDBRef>(YY_MOVE(s.value));
            break;

        case 187:  // "regex"
            value.move<BSONRegEx>(YY_MOVE(s.value));
            break;

        case 190:  // "Symbol"
            value.move<BSONSymbol>(YY_MOVE(s.value));
            break;

        case 223:  // dbPointer
        case 224:  // javascript
        case 225:  // symbol
        case 226:  // javascriptWScope
        case 227:  // int
        case 228:  // timestamp
        case 229:  // long
        case 230:  // double
        case 231:  // decimal
        case 232:  // minKey
        case 233:  // maxKey
        case 234:  // value
        case 235:  // string
        case 236:  // aggregationFieldPath
        case 237:  // binary
        case 238:  // undefined
        case 239:  // objectId
        case 240:  // bool
        case 241:  // date
        case 242:  // null
        case 243:  // regex
        case 244:  // simpleValue
        case 245:  // compoundValue
        case 246:  // valueArray
        case 247:  // valueObject
        case 248:  // valueFields
        case 249:  // variable
        case 250:  // typeArray
        case 251:  // typeValue
        case 252:  // pipeline
        case 253:  // stageList
        case 254:  // stage
        case 255:  // inhibitOptimization
        case 256:  // unionWith
        case 257:  // skip
        case 258:  // limit
        case 259:  // matchStage
        case 260:  // project
        case 261:  // sample
        case 262:  // aggregationProjectFields
        case 263:  // aggregationProjectionObjectFields
        case 264:  // topLevelAggregationProjection
        case 265:  // aggregationProjection
        case 266:  // projectionCommon
        case 267:  // aggregationProjectionObject
        case 268:  // num
        case 269:  // expression
        case 270:  // exprFixedTwoArg
        case 271:  // exprFixedThreeArg
        case 272:  // slice
        case 273:  // expressionArray
        case 274:  // expressionObject
        case 275:  // expressionFields
        case 276:  // maths
        case 277:  // meta
        case 278:  // add
        case 279:  // boolExprs
        case 280:  // and
        case 281:  // or
        case 282:  // not
        case 283:  // literalEscapes
        case 284:  // const
        case 285:  // literal
        case 286:  // stringExps
        case 287:  // concat
        case 288:  // dateFromString
        case 289:  // dateToString
        case 290:  // indexOfBytes
        case 291:  // indexOfCP
        case 292:  // ltrim
        case 293:  // regexFind
        case 294:  // regexFindAll
        case 295:  // regexMatch
        case 296:  // regexArgs
        case 297:  // replaceOne
        case 298:  // replaceAll
        case 299:  // rtrim
        case 300:  // split
        case 301:  // strLenBytes
        case 302:  // strLenCP
        case 303:  // strcasecmp
        case 304:  // substr
        case 305:  // substrBytes
        case 306:  // substrCP
        case 307:  // toLower
        case 308:  // toUpper
        case 309:  // trim
        case 310:  // compExprs
        case 311:  // cmp
        case 312:  // eq
        case 313:  // gt
        case 314:  // gte
        case 315:  // lt
        case 316:  // lte
        case 317:  // ne
        case 318:  // dateExps
        case 319:  // dateFromParts
        case 320:  // dateToParts
        case 321:  // dayOfMonth
        case 322:  // dayOfWeek
        case 323:  // dayOfYear
        case 324:  // hour
        case 325:  // isoDayOfWeek
        case 326:  // isoWeek
        case 327:  // isoWeekYear
        case 328:  // millisecond
        case 329:  // minute
        case 330:  // month
        case 331:  // second
        case 332:  // week
        case 333:  // year
        case 334:  // typeExpression
        case 335:  // convert
        case 336:  // toBool
        case 337:  // toDate
        case 338:  // toDecimal
        case 339:  // toDouble
        case 340:  // toInt
        case 341:  // toLong
        case 342:  // toObjectId
        case 343:  // toString
        case 344:  // type
        case 345:  // abs
        case 346:  // ceil
        case 347:  // divide
        case 348:  // exponent
        case 349:  // floor
        case 350:  // ln
        case 351:  // log
        case 352:  // logten
        case 353:  // mod
        case 354:  // multiply
        case 355:  // pow
        case 356:  // round
        case 357:  // sqrt
        case 358:  // subtract
        case 359:  // trunc
        case 378:  // setExpression
        case 379:  // allElementsTrue
        case 380:  // anyElementTrue
        case 381:  // setDifference
        case 382:  // setEquals
        case 383:  // setIntersection
        case 384:  // setIsSubset
        case 385:  // setUnion
        case 386:  // trig
        case 387:  // sin
        case 388:  // cos
        case 389:  // tan
        case 390:  // sinh
        case 391:  // cosh
        case 392:  // tanh
        case 393:  // asin
        case 394:  // acos
        case 395:  // atan
        case 396:  // asinh
        case 397:  // acosh
        case 398:  // atanh
        case 399:  // atan2
        case 400:  // degreesToRadians
        case 401:  // radiansToDegrees
        case 402:  // nonArrayExpression
        case 403:  // nonArrayCompoundExpression
        case 404:  // aggregationOperator
        case 405:  // aggregationOperatorWithoutSlice
        case 406:  // expressionSingletonArray
        case 407:  // singleArgExpression
        case 408:  // nonArrayNonObjExpression
        case 409:  // matchExpression
        case 410:  // predicates
        case 411:  // compoundMatchExprs
        case 412:  // predValue
        case 413:  // additionalExprs
        case 428:  // textArgCaseSensitive
        case 429:  // textArgDiacriticSensitive
        case 430:  // textArgLanguage
        case 431:  // textArgSearch
        case 432:  // findProject
        case 433:  // findProjectFields
        case 434:  // topLevelFindProjection
        case 435:  // findProjection
        case 436:  // findProjectionSlice
        case 437:  // elemMatch
        case 438:  // findProjectionObject
        case 439:  // findProjectionObjectFields
        case 442:  // sortSpecs
        case 443:  // specList
        case 444:  // metaSort
        case 445:  // oneOrNegOne
        case 446:  // metaSortKeyword
            value.move<CNode>(YY_MOVE(s.value));
            break;

        case 204:  // aggregationProjectionFieldname
        case 205:  // projectionFieldname
        case 206:  // expressionFieldname
        case 207:  // stageAsUserFieldname
        case 208:  // argAsUserFieldname
        case 209:  // argAsProjectionPath
        case 210:  // aggExprAsUserFieldname
        case 211:  // invariableUserFieldname
        case 212:  // sortFieldname
        case 213:  // idAsUserFieldname
        case 214:  // elemMatchAsUserFieldname
        case 215:  // idAsProjectionPath
        case 216:  // valueFieldname
        case 217:  // predFieldname
        case 423:  // logicalExprField
            value.move<CNode::Fieldname>(YY_MOVE(s.value));
            break;

        case 185:  // "Date"
            value.move<Date_t>(YY_MOVE(s.value));
            break;

        case 195:  // "arbitrary decimal"
            value.move<Decimal128>(YY_MOVE(s.value));
            break;

        case 184:  // "ObjectID"
            value.move<OID>(YY_MOVE(s.value));
            break;

        case 196:  // "Timestamp"
            value.move<Timestamp>(YY_MOVE(s.value));
            break;

        case 198:  // "maxKey"
            value.move<UserMaxKey>(YY_MOVE(s.value));
            break;

        case 197:  // "minKey"
            value.move<UserMinKey>(YY_MOVE(s.value));
            break;

        case 186:  // "null"
            value.move<UserNull>(YY_MOVE(s.value));
            break;

        case 183:  // "undefined"
            value.move<UserUndefined>(YY_MOVE(s.value));
            break;

        case 194:  // "arbitrary double"
            value.move<double>(YY_MOVE(s.value));
            break;

        case 192:  // "arbitrary integer"
            value.move<int>(YY_MOVE(s.value));
            break;

        case 193:  // "arbitrary long"
            value.move<long long>(YY_MOVE(s.value));
            break;

        case 218:  // aggregationProjectField
        case 219:  // aggregationProjectionObjectField
        case 220:  // expressionField
        case 221:  // valueField
        case 360:  // onErrorArg
        case 361:  // onNullArg
        case 362:  // formatArg
        case 363:  // timezoneArg
        case 364:  // charsArg
        case 365:  // optionsArg
        case 366:  // hourArg
        case 367:  // minuteArg
        case 368:  // secondArg
        case 369:  // millisecondArg
        case 370:  // dayArg
        case 371:  // isoWeekArg
        case 372:  // iso8601Arg
        case 373:  // monthArg
        case 374:  // isoDayOfWeekArg
        case 414:  // predicate
        case 415:  // fieldPredicate
        case 416:  // logicalExpr
        case 417:  // operatorExpression
        case 418:  // notExpr
        case 419:  // matchMod
        case 420:  // existsExpr
        case 421:  // typeExpr
        case 422:  // commentExpr
        case 425:  // matchExpr
        case 426:  // matchText
        case 427:  // matchWhere
        case 440:  // findProjectField
        case 441:  // findProjectionObjectField
        case 447:  // sortSpec
            value.move<std::pair<CNode::Fieldname, CNode>>(YY_MOVE(s.value));
            break;

        case 176:  // "fieldname"
        case 178:  // "$-prefixed fieldname"
        case 179:  // "string"
        case 180:  // "$-prefixed string"
        case 181:  // "$$-prefixed string"
        case 222:  // arg
            value.move<std::string>(YY_MOVE(s.value));
            break;

        case 375:  // expressions
        case 376:  // values
        case 377:  // exprZeroToTwo
        case 424:  // typeValues
            value.move<std::vector<CNode>>(YY_MOVE(s.value));
            break;

        case 177:  // "fieldname containing dotted path"
            value.move<std::vector<std::string>>(YY_MOVE(s.value));
            break;

        default:
            break;
    }

    location = YY_MOVE(s.location);
}

// by_type.
inline ParserGen::by_type::by_type() : type(empty_symbol) {}

#if 201103L <= YY_CPLUSPLUS
inline ParserGen::by_type::by_type(by_type&& that) : type(that.type) {
    that.clear();
}
#endif

inline ParserGen::by_type::by_type(const by_type& that) : type(that.type) {}

inline ParserGen::by_type::by_type(token_type t) : type(yytranslate_(t)) {}

inline void ParserGen::by_type::clear() {
    type = empty_symbol;
}

inline void ParserGen::by_type::move(by_type& that) {
    type = that.type;
    that.clear();
}

inline int ParserGen::by_type::type_get() const YY_NOEXCEPT {
    return type;
}

#line 57 "src/mongo/db/cst/grammar.yy"
}  // namespace mongo
#line 6493 "src/mongo/db/cst/parser_gen.hpp"


#endif  // !YY_YY_SRC_MONGO_DB_CST_PARSER_GEN_HPP_INCLUDED
