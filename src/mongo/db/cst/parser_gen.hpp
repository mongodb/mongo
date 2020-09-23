// A Bison parser, made by GNU Bison 3.7.1.

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
 ** \file parser_gen.hpp
 ** Define the mongo::parser class.
 */

// C++ LALR(1) parser skeleton written by Akim Demaille.

// DO NOT RELY ON FEATURES THAT ARE NOT DOCUMENTED in the manual,
// especially those whose name start with YY_ or yy_.  They are
// private implementation details that can be changed or removed.

#ifndef YY_YY_PARSER_GEN_HPP_INCLUDED
#define YY_YY_PARSER_GEN_HPP_INCLUDED
// "%code requires" blocks.
#line 66 "grammar.yy"

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

#line 64 "parser_gen.hpp"

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

#line 57 "grammar.yy"
namespace mongo {
#line 199 "parser_gen.hpp"


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

#if 201103L <= YY_CPLUSPLUS
        /// Non copyable.
        semantic_type(const self_type&) = delete;
        /// Non copyable.
        self_type& operator=(const self_type&) = delete;
#endif

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
#if YY_CPLUSPLUS < 201103L
        /// Non copyable.
        semantic_type(const self_type&);
        /// Non copyable.
        self_type& operator=(const self_type&);
#endif

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
            // arrayExps
            // arrayElemAt
            // arrayToObject
            // concatArrays
            // filter
            // first
            // in
            // indexOfArray
            // isArray
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
            // asArg
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

    /// Token kinds.
    struct token {
        enum token_kind_type {
            YYEMPTY = -2,
            END_OF_FILE = 0,                   // "EOF"
            YYerror = 1,                       // error
            YYUNDEF = 2,                       // "invalid token"
            ABS = 3,                           // ABS
            ACOS = 4,                          // ACOS
            ACOSH = 5,                         // ACOSH
            ADD = 6,                           // ADD
            ALL_ELEMENTS_TRUE = 7,             // "allElementsTrue"
            AND = 8,                           // AND
            ANY_ELEMENT_TRUE = 9,              // "anyElementTrue"
            ARG_CASE_SENSITIVE = 10,           // "$caseSensitive argument"
            ARRAY_ELEM_AT = 11,                // ARRAY_ELEM_AT
            ARRAY_TO_OBJECT = 12,              // ARRAY_TO_OBJECT
            ARG_AS = 13,                       // "as argument"
            ARG_CHARS = 14,                    // "chars argument"
            ARG_COLL = 15,                     // "coll argument"
            ARG_COND = 16,                     // "cond argument"
            ARG_DATE = 17,                     // "date argument"
            ARG_DATE_STRING = 18,              // "dateString argument"
            ARG_DAY = 19,                      // "day argument"
            ARG_DIACRITIC_SENSITIVE = 20,      // "$diacriticSensitive argument"
            ARG_FILTER = 21,                   // "filter"
            ARG_FIND = 22,                     // "find argument"
            ARG_FORMAT = 23,                   // "format argument"
            ARG_HOUR = 24,                     // "hour argument"
            ARG_INPUT = 25,                    // "input argument"
            ARG_ISO_8601 = 26,                 // "ISO 8601 argument"
            ARG_ISO_DAY_OF_WEEK = 27,          // "ISO day of week argument"
            ARG_ISO_WEEK = 28,                 // "ISO week argument"
            ARG_ISO_WEEK_YEAR = 29,            // "ISO week year argument"
            ARG_LANGUAGE = 30,                 // "$language argument"
            ARG_MILLISECOND = 31,              // "millisecond argument"
            ARG_MINUTE = 32,                   // "minute argument"
            ARG_MONTH = 33,                    // "month argument"
            ARG_ON_ERROR = 34,                 // "onError argument"
            ARG_ON_NULL = 35,                  // "onNull argument"
            ARG_OPTIONS = 36,                  // "options argument"
            ARG_PIPELINE = 37,                 // "pipeline argument"
            ARG_REGEX = 38,                    // "regex argument"
            ARG_REPLACEMENT = 39,              // "replacement argument"
            ARG_SEARCH = 40,                   // "$search argument"
            ARG_SECOND = 41,                   // "second argument"
            ARG_SIZE = 42,                     // "size argument"
            ARG_TIMEZONE = 43,                 // "timezone argument"
            ARG_TO = 44,                       // "to argument"
            ARG_YEAR = 45,                     // "year argument"
            ASIN = 46,                         // ASIN
            ASINH = 47,                        // ASINH
            ATAN = 48,                         // ATAN
            ATAN2 = 49,                        // ATAN2
            ATANH = 50,                        // ATANH
            BOOL_FALSE = 51,                   // "false"
            BOOL_TRUE = 52,                    // "true"
            CEIL = 53,                         // CEIL
            CMP = 54,                          // CMP
            COMMENT = 55,                      // COMMENT
            CONCAT = 56,                       // CONCAT
            CONCAT_ARRAYS = 57,                // CONCAT_ARRAYS
            CONST_EXPR = 58,                   // CONST_EXPR
            CONVERT = 59,                      // CONVERT
            COS = 60,                          // COS
            COSH = 61,                         // COSH
            DATE_FROM_PARTS = 62,              // DATE_FROM_PARTS
            DATE_FROM_STRING = 63,             // DATE_FROM_STRING
            DATE_TO_PARTS = 64,                // DATE_TO_PARTS
            DATE_TO_STRING = 65,               // DATE_TO_STRING
            DAY_OF_MONTH = 66,                 // DAY_OF_MONTH
            DAY_OF_WEEK = 67,                  // DAY_OF_WEEK
            DAY_OF_YEAR = 68,                  // DAY_OF_YEAR
            DECIMAL_NEGATIVE_ONE = 69,         // "-1 (decimal)"
            DECIMAL_ONE = 70,                  // "1 (decimal)"
            DECIMAL_ZERO = 71,                 // "zero (decimal)"
            DEGREES_TO_RADIANS = 72,           // DEGREES_TO_RADIANS
            DIVIDE = 73,                       // DIVIDE
            DOUBLE_NEGATIVE_ONE = 74,          // "-1 (double)"
            DOUBLE_ONE = 75,                   // "1 (double)"
            DOUBLE_ZERO = 76,                  // "zero (double)"
            ELEM_MATCH = 77,                   // "elemMatch operator"
            END_ARRAY = 78,                    // "end of array"
            END_OBJECT = 79,                   // "end of object"
            EQ = 80,                           // EQ
            EXISTS = 81,                       // EXISTS
            EXPONENT = 82,                     // EXPONENT
            EXPR = 83,                         // EXPR
            FILTER = 84,                       // FILTER
            FIRST = 85,                        // FIRST
            FLOOR = 86,                        // FLOOR
            GEO_NEAR_DISTANCE = 87,            // "geoNearDistance"
            GEO_NEAR_POINT = 88,               // "geoNearPoint"
            GT = 89,                           // GT
            GTE = 90,                          // GTE
            HOUR = 91,                         // HOUR
            ID = 92,                           // ID
            IN = 93,                           // IN
            INDEX_KEY = 94,                    // "indexKey"
            INDEX_OF_ARRAY = 95,               // INDEX_OF_ARRAY
            INDEX_OF_BYTES = 96,               // INDEX_OF_BYTES
            INDEX_OF_CP = 97,                  // INDEX_OF_CP
            INT_NEGATIVE_ONE = 98,             // "-1 (int)"
            INT_ONE = 99,                      // "1 (int)"
            INT_ZERO = 100,                    // "zero (int)"
            IS_ARRAY = 101,                    // IS_ARRAY
            ISO_DAY_OF_WEEK = 102,             // ISO_DAY_OF_WEEK
            ISO_WEEK = 103,                    // ISO_WEEK
            ISO_WEEK_YEAR = 104,               // ISO_WEEK_YEAR
            LITERAL = 105,                     // LITERAL
            LN = 106,                          // LN
            LOG = 107,                         // LOG
            LOGTEN = 108,                      // LOGTEN
            LONG_NEGATIVE_ONE = 109,           // "-1 (long)"
            LONG_ONE = 110,                    // "1 (long)"
            LONG_ZERO = 111,                   // "zero (long)"
            LT = 112,                          // LT
            LTE = 113,                         // LTE
            LTRIM = 114,                       // LTRIM
            META = 115,                        // META
            MILLISECOND = 116,                 // MILLISECOND
            MINUTE = 117,                      // MINUTE
            MOD = 118,                         // MOD
            MONTH = 119,                       // MONTH
            MULTIPLY = 120,                    // MULTIPLY
            NE = 121,                          // NE
            NOR = 122,                         // NOR
            NOT = 123,                         // NOT
            OR = 124,                          // OR
            POW = 125,                         // POW
            RADIANS_TO_DEGREES = 126,          // RADIANS_TO_DEGREES
            RAND_VAL = 127,                    // "randVal"
            RECORD_ID = 128,                   // "recordId"
            REGEX_FIND = 129,                  // REGEX_FIND
            REGEX_FIND_ALL = 130,              // REGEX_FIND_ALL
            REGEX_MATCH = 131,                 // REGEX_MATCH
            REPLACE_ALL = 132,                 // REPLACE_ALL
            REPLACE_ONE = 133,                 // REPLACE_ONE
            ROUND = 134,                       // ROUND
            RTRIM = 135,                       // RTRIM
            SEARCH_HIGHLIGHTS = 136,           // "searchHighlights"
            SEARCH_SCORE = 137,                // "searchScore"
            SECOND = 138,                      // SECOND
            SET_DIFFERENCE = 139,              // "setDifference"
            SET_EQUALS = 140,                  // "setEquals"
            SET_INTERSECTION = 141,            // "setIntersection"
            SET_IS_SUBSET = 142,               // "setIsSubset"
            SET_UNION = 143,                   // "setUnion"
            SIN = 144,                         // SIN
            SINH = 145,                        // SINH
            SLICE = 146,                       // "slice"
            SORT_KEY = 147,                    // "sortKey"
            SPLIT = 148,                       // SPLIT
            SQRT = 149,                        // SQRT
            STAGE_INHIBIT_OPTIMIZATION = 150,  // STAGE_INHIBIT_OPTIMIZATION
            STAGE_LIMIT = 151,                 // STAGE_LIMIT
            STAGE_MATCH = 152,                 // STAGE_MATCH
            STAGE_PROJECT = 153,               // STAGE_PROJECT
            STAGE_SAMPLE = 154,                // STAGE_SAMPLE
            STAGE_SKIP = 155,                  // STAGE_SKIP
            STAGE_UNION_WITH = 156,            // STAGE_UNION_WITH
            START_ARRAY = 157,                 // "array"
            START_OBJECT = 158,                // "object"
            STR_CASE_CMP = 159,                // STR_CASE_CMP
            STR_LEN_BYTES = 160,               // STR_LEN_BYTES
            STR_LEN_CP = 161,                  // STR_LEN_CP
            SUBSTR = 162,                      // SUBSTR
            SUBSTR_BYTES = 163,                // SUBSTR_BYTES
            SUBSTR_CP = 164,                   // SUBSTR_CP
            SUBTRACT = 165,                    // SUBTRACT
            TAN = 166,                         // TAN
            TANH = 167,                        // TANH
            TEXT = 168,                        // TEXT
            TEXT_SCORE = 169,                  // "textScore"
            TO_BOOL = 170,                     // TO_BOOL
            TO_DATE = 171,                     // TO_DATE
            TO_DECIMAL = 172,                  // TO_DECIMAL
            TO_DOUBLE = 173,                   // TO_DOUBLE
            TO_INT = 174,                      // TO_INT
            TO_LONG = 175,                     // TO_LONG
            TO_LOWER = 176,                    // TO_LOWER
            TO_OBJECT_ID = 177,                // TO_OBJECT_ID
            TO_STRING = 178,                   // TO_STRING
            TO_UPPER = 179,                    // TO_UPPER
            TRIM = 180,                        // TRIM
            TRUNC = 181,                       // TRUNC
            TYPE = 182,                        // TYPE
            WEEK = 183,                        // WEEK
            WHERE = 184,                       // WHERE
            YEAR = 185,                        // YEAR
            FIELDNAME = 186,                   // "fieldname"
            DOTTED_FIELDNAME = 187,            // "fieldname containing dotted path"
            DOLLAR_PREF_FIELDNAME = 188,       // "$-prefixed fieldname"
            STRING = 189,                      // "string"
            DOLLAR_STRING = 190,               // "$-prefixed string"
            DOLLAR_DOLLAR_STRING = 191,        // "$$-prefixed string"
            BINARY = 192,                      // "BinData"
            UNDEFINED = 193,                   // "undefined"
            OBJECT_ID = 194,                   // "ObjectID"
            DATE_LITERAL = 195,                // "Date"
            JSNULL = 196,                      // "null"
            REGEX = 197,                       // "regex"
            DB_POINTER = 198,                  // "dbPointer"
            JAVASCRIPT = 199,                  // "Code"
            SYMBOL = 200,                      // "Symbol"
            JAVASCRIPT_W_SCOPE = 201,          // "CodeWScope"
            INT_OTHER = 202,                   // "arbitrary integer"
            LONG_OTHER = 203,                  // "arbitrary long"
            DOUBLE_OTHER = 204,                // "arbitrary double"
            DECIMAL_OTHER = 205,               // "arbitrary decimal"
            TIMESTAMP = 206,                   // "Timestamp"
            MIN_KEY = 207,                     // "minKey"
            MAX_KEY = 208,                     // "maxKey"
            START_PIPELINE = 209,              // START_PIPELINE
            START_MATCH = 210,                 // START_MATCH
            START_PROJECT = 211,               // START_PROJECT
            START_SORT = 212                   // START_SORT
        };
        /// Backward compatibility alias (Bison 3.6).
        typedef token_kind_type yytokentype;
    };

    /// Token kind, as returned by yylex.
    typedef token::yytokentype token_kind_type;

    /// Backward compatibility alias (Bison 3.6).
    typedef token_kind_type token_type;

    /// Symbol kinds.
    struct symbol_kind {
        enum symbol_kind_type {
            YYNTOKENS = 213,  ///< Number of tokens.
            S_YYEMPTY = -2,
            S_YYEOF = 0,                                // "EOF"
            S_YYerror = 1,                              // error
            S_YYUNDEF = 2,                              // "invalid token"
            S_ABS = 3,                                  // ABS
            S_ACOS = 4,                                 // ACOS
            S_ACOSH = 5,                                // ACOSH
            S_ADD = 6,                                  // ADD
            S_ALL_ELEMENTS_TRUE = 7,                    // "allElementsTrue"
            S_AND = 8,                                  // AND
            S_ANY_ELEMENT_TRUE = 9,                     // "anyElementTrue"
            S_ARG_CASE_SENSITIVE = 10,                  // "$caseSensitive argument"
            S_ARRAY_ELEM_AT = 11,                       // ARRAY_ELEM_AT
            S_ARRAY_TO_OBJECT = 12,                     // ARRAY_TO_OBJECT
            S_ARG_AS = 13,                              // "as argument"
            S_ARG_CHARS = 14,                           // "chars argument"
            S_ARG_COLL = 15,                            // "coll argument"
            S_ARG_COND = 16,                            // "cond argument"
            S_ARG_DATE = 17,                            // "date argument"
            S_ARG_DATE_STRING = 18,                     // "dateString argument"
            S_ARG_DAY = 19,                             // "day argument"
            S_ARG_DIACRITIC_SENSITIVE = 20,             // "$diacriticSensitive argument"
            S_ARG_FILTER = 21,                          // "filter"
            S_ARG_FIND = 22,                            // "find argument"
            S_ARG_FORMAT = 23,                          // "format argument"
            S_ARG_HOUR = 24,                            // "hour argument"
            S_ARG_INPUT = 25,                           // "input argument"
            S_ARG_ISO_8601 = 26,                        // "ISO 8601 argument"
            S_ARG_ISO_DAY_OF_WEEK = 27,                 // "ISO day of week argument"
            S_ARG_ISO_WEEK = 28,                        // "ISO week argument"
            S_ARG_ISO_WEEK_YEAR = 29,                   // "ISO week year argument"
            S_ARG_LANGUAGE = 30,                        // "$language argument"
            S_ARG_MILLISECOND = 31,                     // "millisecond argument"
            S_ARG_MINUTE = 32,                          // "minute argument"
            S_ARG_MONTH = 33,                           // "month argument"
            S_ARG_ON_ERROR = 34,                        // "onError argument"
            S_ARG_ON_NULL = 35,                         // "onNull argument"
            S_ARG_OPTIONS = 36,                         // "options argument"
            S_ARG_PIPELINE = 37,                        // "pipeline argument"
            S_ARG_REGEX = 38,                           // "regex argument"
            S_ARG_REPLACEMENT = 39,                     // "replacement argument"
            S_ARG_SEARCH = 40,                          // "$search argument"
            S_ARG_SECOND = 41,                          // "second argument"
            S_ARG_SIZE = 42,                            // "size argument"
            S_ARG_TIMEZONE = 43,                        // "timezone argument"
            S_ARG_TO = 44,                              // "to argument"
            S_ARG_YEAR = 45,                            // "year argument"
            S_ASIN = 46,                                // ASIN
            S_ASINH = 47,                               // ASINH
            S_ATAN = 48,                                // ATAN
            S_ATAN2 = 49,                               // ATAN2
            S_ATANH = 50,                               // ATANH
            S_BOOL_FALSE = 51,                          // "false"
            S_BOOL_TRUE = 52,                           // "true"
            S_CEIL = 53,                                // CEIL
            S_CMP = 54,                                 // CMP
            S_COMMENT = 55,                             // COMMENT
            S_CONCAT = 56,                              // CONCAT
            S_CONCAT_ARRAYS = 57,                       // CONCAT_ARRAYS
            S_CONST_EXPR = 58,                          // CONST_EXPR
            S_CONVERT = 59,                             // CONVERT
            S_COS = 60,                                 // COS
            S_COSH = 61,                                // COSH
            S_DATE_FROM_PARTS = 62,                     // DATE_FROM_PARTS
            S_DATE_FROM_STRING = 63,                    // DATE_FROM_STRING
            S_DATE_TO_PARTS = 64,                       // DATE_TO_PARTS
            S_DATE_TO_STRING = 65,                      // DATE_TO_STRING
            S_DAY_OF_MONTH = 66,                        // DAY_OF_MONTH
            S_DAY_OF_WEEK = 67,                         // DAY_OF_WEEK
            S_DAY_OF_YEAR = 68,                         // DAY_OF_YEAR
            S_DECIMAL_NEGATIVE_ONE = 69,                // "-1 (decimal)"
            S_DECIMAL_ONE = 70,                         // "1 (decimal)"
            S_DECIMAL_ZERO = 71,                        // "zero (decimal)"
            S_DEGREES_TO_RADIANS = 72,                  // DEGREES_TO_RADIANS
            S_DIVIDE = 73,                              // DIVIDE
            S_DOUBLE_NEGATIVE_ONE = 74,                 // "-1 (double)"
            S_DOUBLE_ONE = 75,                          // "1 (double)"
            S_DOUBLE_ZERO = 76,                         // "zero (double)"
            S_ELEM_MATCH = 77,                          // "elemMatch operator"
            S_END_ARRAY = 78,                           // "end of array"
            S_END_OBJECT = 79,                          // "end of object"
            S_EQ = 80,                                  // EQ
            S_EXISTS = 81,                              // EXISTS
            S_EXPONENT = 82,                            // EXPONENT
            S_EXPR = 83,                                // EXPR
            S_FILTER = 84,                              // FILTER
            S_FIRST = 85,                               // FIRST
            S_FLOOR = 86,                               // FLOOR
            S_GEO_NEAR_DISTANCE = 87,                   // "geoNearDistance"
            S_GEO_NEAR_POINT = 88,                      // "geoNearPoint"
            S_GT = 89,                                  // GT
            S_GTE = 90,                                 // GTE
            S_HOUR = 91,                                // HOUR
            S_ID = 92,                                  // ID
            S_IN = 93,                                  // IN
            S_INDEX_KEY = 94,                           // "indexKey"
            S_INDEX_OF_ARRAY = 95,                      // INDEX_OF_ARRAY
            S_INDEX_OF_BYTES = 96,                      // INDEX_OF_BYTES
            S_INDEX_OF_CP = 97,                         // INDEX_OF_CP
            S_INT_NEGATIVE_ONE = 98,                    // "-1 (int)"
            S_INT_ONE = 99,                             // "1 (int)"
            S_INT_ZERO = 100,                           // "zero (int)"
            S_IS_ARRAY = 101,                           // IS_ARRAY
            S_ISO_DAY_OF_WEEK = 102,                    // ISO_DAY_OF_WEEK
            S_ISO_WEEK = 103,                           // ISO_WEEK
            S_ISO_WEEK_YEAR = 104,                      // ISO_WEEK_YEAR
            S_LITERAL = 105,                            // LITERAL
            S_LN = 106,                                 // LN
            S_LOG = 107,                                // LOG
            S_LOGTEN = 108,                             // LOGTEN
            S_LONG_NEGATIVE_ONE = 109,                  // "-1 (long)"
            S_LONG_ONE = 110,                           // "1 (long)"
            S_LONG_ZERO = 111,                          // "zero (long)"
            S_LT = 112,                                 // LT
            S_LTE = 113,                                // LTE
            S_LTRIM = 114,                              // LTRIM
            S_META = 115,                               // META
            S_MILLISECOND = 116,                        // MILLISECOND
            S_MINUTE = 117,                             // MINUTE
            S_MOD = 118,                                // MOD
            S_MONTH = 119,                              // MONTH
            S_MULTIPLY = 120,                           // MULTIPLY
            S_NE = 121,                                 // NE
            S_NOR = 122,                                // NOR
            S_NOT = 123,                                // NOT
            S_OR = 124,                                 // OR
            S_POW = 125,                                // POW
            S_RADIANS_TO_DEGREES = 126,                 // RADIANS_TO_DEGREES
            S_RAND_VAL = 127,                           // "randVal"
            S_RECORD_ID = 128,                          // "recordId"
            S_REGEX_FIND = 129,                         // REGEX_FIND
            S_REGEX_FIND_ALL = 130,                     // REGEX_FIND_ALL
            S_REGEX_MATCH = 131,                        // REGEX_MATCH
            S_REPLACE_ALL = 132,                        // REPLACE_ALL
            S_REPLACE_ONE = 133,                        // REPLACE_ONE
            S_ROUND = 134,                              // ROUND
            S_RTRIM = 135,                              // RTRIM
            S_SEARCH_HIGHLIGHTS = 136,                  // "searchHighlights"
            S_SEARCH_SCORE = 137,                       // "searchScore"
            S_SECOND = 138,                             // SECOND
            S_SET_DIFFERENCE = 139,                     // "setDifference"
            S_SET_EQUALS = 140,                         // "setEquals"
            S_SET_INTERSECTION = 141,                   // "setIntersection"
            S_SET_IS_SUBSET = 142,                      // "setIsSubset"
            S_SET_UNION = 143,                          // "setUnion"
            S_SIN = 144,                                // SIN
            S_SINH = 145,                               // SINH
            S_SLICE = 146,                              // "slice"
            S_SORT_KEY = 147,                           // "sortKey"
            S_SPLIT = 148,                              // SPLIT
            S_SQRT = 149,                               // SQRT
            S_STAGE_INHIBIT_OPTIMIZATION = 150,         // STAGE_INHIBIT_OPTIMIZATION
            S_STAGE_LIMIT = 151,                        // STAGE_LIMIT
            S_STAGE_MATCH = 152,                        // STAGE_MATCH
            S_STAGE_PROJECT = 153,                      // STAGE_PROJECT
            S_STAGE_SAMPLE = 154,                       // STAGE_SAMPLE
            S_STAGE_SKIP = 155,                         // STAGE_SKIP
            S_STAGE_UNION_WITH = 156,                   // STAGE_UNION_WITH
            S_START_ARRAY = 157,                        // "array"
            S_START_OBJECT = 158,                       // "object"
            S_STR_CASE_CMP = 159,                       // STR_CASE_CMP
            S_STR_LEN_BYTES = 160,                      // STR_LEN_BYTES
            S_STR_LEN_CP = 161,                         // STR_LEN_CP
            S_SUBSTR = 162,                             // SUBSTR
            S_SUBSTR_BYTES = 163,                       // SUBSTR_BYTES
            S_SUBSTR_CP = 164,                          // SUBSTR_CP
            S_SUBTRACT = 165,                           // SUBTRACT
            S_TAN = 166,                                // TAN
            S_TANH = 167,                               // TANH
            S_TEXT = 168,                               // TEXT
            S_TEXT_SCORE = 169,                         // "textScore"
            S_TO_BOOL = 170,                            // TO_BOOL
            S_TO_DATE = 171,                            // TO_DATE
            S_TO_DECIMAL = 172,                         // TO_DECIMAL
            S_TO_DOUBLE = 173,                          // TO_DOUBLE
            S_TO_INT = 174,                             // TO_INT
            S_TO_LONG = 175,                            // TO_LONG
            S_TO_LOWER = 176,                           // TO_LOWER
            S_TO_OBJECT_ID = 177,                       // TO_OBJECT_ID
            S_TO_STRING = 178,                          // TO_STRING
            S_TO_UPPER = 179,                           // TO_UPPER
            S_TRIM = 180,                               // TRIM
            S_TRUNC = 181,                              // TRUNC
            S_TYPE = 182,                               // TYPE
            S_WEEK = 183,                               // WEEK
            S_WHERE = 184,                              // WHERE
            S_YEAR = 185,                               // YEAR
            S_FIELDNAME = 186,                          // "fieldname"
            S_DOTTED_FIELDNAME = 187,                   // "fieldname containing dotted path"
            S_DOLLAR_PREF_FIELDNAME = 188,              // "$-prefixed fieldname"
            S_STRING = 189,                             // "string"
            S_DOLLAR_STRING = 190,                      // "$-prefixed string"
            S_DOLLAR_DOLLAR_STRING = 191,               // "$$-prefixed string"
            S_BINARY = 192,                             // "BinData"
            S_UNDEFINED = 193,                          // "undefined"
            S_OBJECT_ID = 194,                          // "ObjectID"
            S_DATE_LITERAL = 195,                       // "Date"
            S_JSNULL = 196,                             // "null"
            S_REGEX = 197,                              // "regex"
            S_DB_POINTER = 198,                         // "dbPointer"
            S_JAVASCRIPT = 199,                         // "Code"
            S_SYMBOL = 200,                             // "Symbol"
            S_JAVASCRIPT_W_SCOPE = 201,                 // "CodeWScope"
            S_INT_OTHER = 202,                          // "arbitrary integer"
            S_LONG_OTHER = 203,                         // "arbitrary long"
            S_DOUBLE_OTHER = 204,                       // "arbitrary double"
            S_DECIMAL_OTHER = 205,                      // "arbitrary decimal"
            S_TIMESTAMP = 206,                          // "Timestamp"
            S_MIN_KEY = 207,                            // "minKey"
            S_MAX_KEY = 208,                            // "maxKey"
            S_START_PIPELINE = 209,                     // START_PIPELINE
            S_START_MATCH = 210,                        // START_MATCH
            S_START_PROJECT = 211,                      // START_PROJECT
            S_START_SORT = 212,                         // START_SORT
            S_YYACCEPT = 213,                           // $accept
            S_aggregationProjectionFieldname = 214,     // aggregationProjectionFieldname
            S_projectionFieldname = 215,                // projectionFieldname
            S_expressionFieldname = 216,                // expressionFieldname
            S_stageAsUserFieldname = 217,               // stageAsUserFieldname
            S_argAsUserFieldname = 218,                 // argAsUserFieldname
            S_argAsProjectionPath = 219,                // argAsProjectionPath
            S_aggExprAsUserFieldname = 220,             // aggExprAsUserFieldname
            S_invariableUserFieldname = 221,            // invariableUserFieldname
            S_sortFieldname = 222,                      // sortFieldname
            S_idAsUserFieldname = 223,                  // idAsUserFieldname
            S_elemMatchAsUserFieldname = 224,           // elemMatchAsUserFieldname
            S_idAsProjectionPath = 225,                 // idAsProjectionPath
            S_valueFieldname = 226,                     // valueFieldname
            S_predFieldname = 227,                      // predFieldname
            S_aggregationProjectField = 228,            // aggregationProjectField
            S_aggregationProjectionObjectField = 229,   // aggregationProjectionObjectField
            S_expressionField = 230,                    // expressionField
            S_valueField = 231,                         // valueField
            S_arg = 232,                                // arg
            S_dbPointer = 233,                          // dbPointer
            S_javascript = 234,                         // javascript
            S_symbol = 235,                             // symbol
            S_javascriptWScope = 236,                   // javascriptWScope
            S_int = 237,                                // int
            S_timestamp = 238,                          // timestamp
            S_long = 239,                               // long
            S_double = 240,                             // double
            S_decimal = 241,                            // decimal
            S_minKey = 242,                             // minKey
            S_maxKey = 243,                             // maxKey
            S_value = 244,                              // value
            S_string = 245,                             // string
            S_aggregationFieldPath = 246,               // aggregationFieldPath
            S_binary = 247,                             // binary
            S_undefined = 248,                          // undefined
            S_objectId = 249,                           // objectId
            S_bool = 250,                               // bool
            S_date = 251,                               // date
            S_null = 252,                               // null
            S_regex = 253,                              // regex
            S_simpleValue = 254,                        // simpleValue
            S_compoundValue = 255,                      // compoundValue
            S_valueArray = 256,                         // valueArray
            S_valueObject = 257,                        // valueObject
            S_valueFields = 258,                        // valueFields
            S_variable = 259,                           // variable
            S_typeArray = 260,                          // typeArray
            S_typeValue = 261,                          // typeValue
            S_pipeline = 262,                           // pipeline
            S_stageList = 263,                          // stageList
            S_stage = 264,                              // stage
            S_inhibitOptimization = 265,                // inhibitOptimization
            S_unionWith = 266,                          // unionWith
            S_skip = 267,                               // skip
            S_limit = 268,                              // limit
            S_matchStage = 269,                         // matchStage
            S_project = 270,                            // project
            S_sample = 271,                             // sample
            S_aggregationProjectFields = 272,           // aggregationProjectFields
            S_aggregationProjectionObjectFields = 273,  // aggregationProjectionObjectFields
            S_topLevelAggregationProjection = 274,      // topLevelAggregationProjection
            S_aggregationProjection = 275,              // aggregationProjection
            S_projectionCommon = 276,                   // projectionCommon
            S_aggregationProjectionObject = 277,        // aggregationProjectionObject
            S_num = 278,                                // num
            S_expression = 279,                         // expression
            S_exprFixedTwoArg = 280,                    // exprFixedTwoArg
            S_exprFixedThreeArg = 281,                  // exprFixedThreeArg
            S_slice = 282,                              // slice
            S_expressionArray = 283,                    // expressionArray
            S_expressionObject = 284,                   // expressionObject
            S_expressionFields = 285,                   // expressionFields
            S_maths = 286,                              // maths
            S_meta = 287,                               // meta
            S_add = 288,                                // add
            S_boolExprs = 289,                          // boolExprs
            S_and = 290,                                // and
            S_or = 291,                                 // or
            S_not = 292,                                // not
            S_literalEscapes = 293,                     // literalEscapes
            S_const = 294,                              // const
            S_literal = 295,                            // literal
            S_stringExps = 296,                         // stringExps
            S_concat = 297,                             // concat
            S_dateFromString = 298,                     // dateFromString
            S_dateToString = 299,                       // dateToString
            S_indexOfBytes = 300,                       // indexOfBytes
            S_indexOfCP = 301,                          // indexOfCP
            S_ltrim = 302,                              // ltrim
            S_regexFind = 303,                          // regexFind
            S_regexFindAll = 304,                       // regexFindAll
            S_regexMatch = 305,                         // regexMatch
            S_regexArgs = 306,                          // regexArgs
            S_replaceOne = 307,                         // replaceOne
            S_replaceAll = 308,                         // replaceAll
            S_rtrim = 309,                              // rtrim
            S_split = 310,                              // split
            S_strLenBytes = 311,                        // strLenBytes
            S_strLenCP = 312,                           // strLenCP
            S_strcasecmp = 313,                         // strcasecmp
            S_substr = 314,                             // substr
            S_substrBytes = 315,                        // substrBytes
            S_substrCP = 316,                           // substrCP
            S_toLower = 317,                            // toLower
            S_toUpper = 318,                            // toUpper
            S_trim = 319,                               // trim
            S_compExprs = 320,                          // compExprs
            S_cmp = 321,                                // cmp
            S_eq = 322,                                 // eq
            S_gt = 323,                                 // gt
            S_gte = 324,                                // gte
            S_lt = 325,                                 // lt
            S_lte = 326,                                // lte
            S_ne = 327,                                 // ne
            S_dateExps = 328,                           // dateExps
            S_dateFromParts = 329,                      // dateFromParts
            S_dateToParts = 330,                        // dateToParts
            S_dayOfMonth = 331,                         // dayOfMonth
            S_dayOfWeek = 332,                          // dayOfWeek
            S_dayOfYear = 333,                          // dayOfYear
            S_hour = 334,                               // hour
            S_isoDayOfWeek = 335,                       // isoDayOfWeek
            S_isoWeek = 336,                            // isoWeek
            S_isoWeekYear = 337,                        // isoWeekYear
            S_millisecond = 338,                        // millisecond
            S_minute = 339,                             // minute
            S_month = 340,                              // month
            S_second = 341,                             // second
            S_week = 342,                               // week
            S_year = 343,                               // year
            S_typeExpression = 344,                     // typeExpression
            S_convert = 345,                            // convert
            S_toBool = 346,                             // toBool
            S_toDate = 347,                             // toDate
            S_toDecimal = 348,                          // toDecimal
            S_toDouble = 349,                           // toDouble
            S_toInt = 350,                              // toInt
            S_toLong = 351,                             // toLong
            S_toObjectId = 352,                         // toObjectId
            S_toString = 353,                           // toString
            S_type = 354,                               // type
            S_abs = 355,                                // abs
            S_ceil = 356,                               // ceil
            S_divide = 357,                             // divide
            S_exponent = 358,                           // exponent
            S_floor = 359,                              // floor
            S_ln = 360,                                 // ln
            S_log = 361,                                // log
            S_logten = 362,                             // logten
            S_mod = 363,                                // mod
            S_multiply = 364,                           // multiply
            S_pow = 365,                                // pow
            S_round = 366,                              // round
            S_sqrt = 367,                               // sqrt
            S_subtract = 368,                           // subtract
            S_trunc = 369,                              // trunc
            S_arrayExps = 370,                          // arrayExps
            S_arrayElemAt = 371,                        // arrayElemAt
            S_arrayToObject = 372,                      // arrayToObject
            S_concatArrays = 373,                       // concatArrays
            S_filter = 374,                             // filter
            S_first = 375,                              // first
            S_in = 376,                                 // in
            S_indexOfArray = 377,                       // indexOfArray
            S_isArray = 378,                            // isArray
            S_onErrorArg = 379,                         // onErrorArg
            S_onNullArg = 380,                          // onNullArg
            S_asArg = 381,                              // asArg
            S_formatArg = 382,                          // formatArg
            S_timezoneArg = 383,                        // timezoneArg
            S_charsArg = 384,                           // charsArg
            S_optionsArg = 385,                         // optionsArg
            S_hourArg = 386,                            // hourArg
            S_minuteArg = 387,                          // minuteArg
            S_secondArg = 388,                          // secondArg
            S_millisecondArg = 389,                     // millisecondArg
            S_dayArg = 390,                             // dayArg
            S_isoWeekArg = 391,                         // isoWeekArg
            S_iso8601Arg = 392,                         // iso8601Arg
            S_monthArg = 393,                           // monthArg
            S_isoDayOfWeekArg = 394,                    // isoDayOfWeekArg
            S_expressions = 395,                        // expressions
            S_values = 396,                             // values
            S_exprZeroToTwo = 397,                      // exprZeroToTwo
            S_setExpression = 398,                      // setExpression
            S_allElementsTrue = 399,                    // allElementsTrue
            S_anyElementTrue = 400,                     // anyElementTrue
            S_setDifference = 401,                      // setDifference
            S_setEquals = 402,                          // setEquals
            S_setIntersection = 403,                    // setIntersection
            S_setIsSubset = 404,                        // setIsSubset
            S_setUnion = 405,                           // setUnion
            S_trig = 406,                               // trig
            S_sin = 407,                                // sin
            S_cos = 408,                                // cos
            S_tan = 409,                                // tan
            S_sinh = 410,                               // sinh
            S_cosh = 411,                               // cosh
            S_tanh = 412,                               // tanh
            S_asin = 413,                               // asin
            S_acos = 414,                               // acos
            S_atan = 415,                               // atan
            S_asinh = 416,                              // asinh
            S_acosh = 417,                              // acosh
            S_atanh = 418,                              // atanh
            S_atan2 = 419,                              // atan2
            S_degreesToRadians = 420,                   // degreesToRadians
            S_radiansToDegrees = 421,                   // radiansToDegrees
            S_nonArrayExpression = 422,                 // nonArrayExpression
            S_nonArrayCompoundExpression = 423,         // nonArrayCompoundExpression
            S_aggregationOperator = 424,                // aggregationOperator
            S_aggregationOperatorWithoutSlice = 425,    // aggregationOperatorWithoutSlice
            S_expressionSingletonArray = 426,           // expressionSingletonArray
            S_singleArgExpression = 427,                // singleArgExpression
            S_nonArrayNonObjExpression = 428,           // nonArrayNonObjExpression
            S_matchExpression = 429,                    // matchExpression
            S_predicates = 430,                         // predicates
            S_compoundMatchExprs = 431,                 // compoundMatchExprs
            S_predValue = 432,                          // predValue
            S_additionalExprs = 433,                    // additionalExprs
            S_predicate = 434,                          // predicate
            S_fieldPredicate = 435,                     // fieldPredicate
            S_logicalExpr = 436,                        // logicalExpr
            S_operatorExpression = 437,                 // operatorExpression
            S_notExpr = 438,                            // notExpr
            S_matchMod = 439,                           // matchMod
            S_existsExpr = 440,                         // existsExpr
            S_typeExpr = 441,                           // typeExpr
            S_commentExpr = 442,                        // commentExpr
            S_logicalExprField = 443,                   // logicalExprField
            S_typeValues = 444,                         // typeValues
            S_matchExpr = 445,                          // matchExpr
            S_matchText = 446,                          // matchText
            S_matchWhere = 447,                         // matchWhere
            S_textArgCaseSensitive = 448,               // textArgCaseSensitive
            S_textArgDiacriticSensitive = 449,          // textArgDiacriticSensitive
            S_textArgLanguage = 450,                    // textArgLanguage
            S_textArgSearch = 451,                      // textArgSearch
            S_findProject = 452,                        // findProject
            S_findProjectFields = 453,                  // findProjectFields
            S_topLevelFindProjection = 454,             // topLevelFindProjection
            S_findProjection = 455,                     // findProjection
            S_findProjectionSlice = 456,                // findProjectionSlice
            S_elemMatch = 457,                          // elemMatch
            S_findProjectionObject = 458,               // findProjectionObject
            S_findProjectionObjectFields = 459,         // findProjectionObjectFields
            S_findProjectField = 460,                   // findProjectField
            S_findProjectionObjectField = 461,          // findProjectionObjectField
            S_sortSpecs = 462,                          // sortSpecs
            S_specList = 463,                           // specList
            S_metaSort = 464,                           // metaSort
            S_oneOrNegOne = 465,                        // oneOrNegOne
            S_metaSortKeyword = 466,                    // metaSortKeyword
            S_sortSpec = 467,                           // sortSpec
            S_start = 468,                              // start
            S_START_ORDERED_OBJECT = 469                // START_ORDERED_OBJECT
        };
    };

    /// (Internal) symbol kind.
    typedef symbol_kind::symbol_kind_type symbol_kind_type;

    /// The number of tokens.
    static const symbol_kind_type YYNTOKENS = symbol_kind::YYNTOKENS;

    /// A complete symbol.
    ///
    /// Expects its Base type to provide access to the symbol kind
    /// via kind ().
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
        basic_symbol(basic_symbol&& that)
            : Base(std::move(that)), value(), location(std::move(that.location)) {
            switch (this->kind()) {
                case symbol_kind::S_BINARY:  // "BinData"
                    value.move<BSONBinData>(std::move(that.value));
                    break;

                case symbol_kind::S_JAVASCRIPT:  // "Code"
                    value.move<BSONCode>(std::move(that.value));
                    break;

                case symbol_kind::S_JAVASCRIPT_W_SCOPE:  // "CodeWScope"
                    value.move<BSONCodeWScope>(std::move(that.value));
                    break;

                case symbol_kind::S_DB_POINTER:  // "dbPointer"
                    value.move<BSONDBRef>(std::move(that.value));
                    break;

                case symbol_kind::S_REGEX:  // "regex"
                    value.move<BSONRegEx>(std::move(that.value));
                    break;

                case symbol_kind::S_SYMBOL:  // "Symbol"
                    value.move<BSONSymbol>(std::move(that.value));
                    break;

                case symbol_kind::S_dbPointer:                 // dbPointer
                case symbol_kind::S_javascript:                // javascript
                case symbol_kind::S_symbol:                    // symbol
                case symbol_kind::S_javascriptWScope:          // javascriptWScope
                case symbol_kind::S_int:                       // int
                case symbol_kind::S_timestamp:                 // timestamp
                case symbol_kind::S_long:                      // long
                case symbol_kind::S_double:                    // double
                case symbol_kind::S_decimal:                   // decimal
                case symbol_kind::S_minKey:                    // minKey
                case symbol_kind::S_maxKey:                    // maxKey
                case symbol_kind::S_value:                     // value
                case symbol_kind::S_string:                    // string
                case symbol_kind::S_aggregationFieldPath:      // aggregationFieldPath
                case symbol_kind::S_binary:                    // binary
                case symbol_kind::S_undefined:                 // undefined
                case symbol_kind::S_objectId:                  // objectId
                case symbol_kind::S_bool:                      // bool
                case symbol_kind::S_date:                      // date
                case symbol_kind::S_null:                      // null
                case symbol_kind::S_regex:                     // regex
                case symbol_kind::S_simpleValue:               // simpleValue
                case symbol_kind::S_compoundValue:             // compoundValue
                case symbol_kind::S_valueArray:                // valueArray
                case symbol_kind::S_valueObject:               // valueObject
                case symbol_kind::S_valueFields:               // valueFields
                case symbol_kind::S_variable:                  // variable
                case symbol_kind::S_typeArray:                 // typeArray
                case symbol_kind::S_typeValue:                 // typeValue
                case symbol_kind::S_pipeline:                  // pipeline
                case symbol_kind::S_stageList:                 // stageList
                case symbol_kind::S_stage:                     // stage
                case symbol_kind::S_inhibitOptimization:       // inhibitOptimization
                case symbol_kind::S_unionWith:                 // unionWith
                case symbol_kind::S_skip:                      // skip
                case symbol_kind::S_limit:                     // limit
                case symbol_kind::S_matchStage:                // matchStage
                case symbol_kind::S_project:                   // project
                case symbol_kind::S_sample:                    // sample
                case symbol_kind::S_aggregationProjectFields:  // aggregationProjectFields
                case symbol_kind::
                    S_aggregationProjectionObjectFields:  // aggregationProjectionObjectFields
                case symbol_kind::S_topLevelAggregationProjection:  // topLevelAggregationProjection
                case symbol_kind::S_aggregationProjection:          // aggregationProjection
                case symbol_kind::S_projectionCommon:               // projectionCommon
                case symbol_kind::S_aggregationProjectionObject:    // aggregationProjectionObject
                case symbol_kind::S_num:                            // num
                case symbol_kind::S_expression:                     // expression
                case symbol_kind::S_exprFixedTwoArg:                // exprFixedTwoArg
                case symbol_kind::S_exprFixedThreeArg:              // exprFixedThreeArg
                case symbol_kind::S_slice:                          // slice
                case symbol_kind::S_expressionArray:                // expressionArray
                case symbol_kind::S_expressionObject:               // expressionObject
                case symbol_kind::S_expressionFields:               // expressionFields
                case symbol_kind::S_maths:                          // maths
                case symbol_kind::S_meta:                           // meta
                case symbol_kind::S_add:                            // add
                case symbol_kind::S_boolExprs:                      // boolExprs
                case symbol_kind::S_and:                            // and
                case symbol_kind::S_or:                             // or
                case symbol_kind::S_not:                            // not
                case symbol_kind::S_literalEscapes:                 // literalEscapes
                case symbol_kind::S_const:                          // const
                case symbol_kind::S_literal:                        // literal
                case symbol_kind::S_stringExps:                     // stringExps
                case symbol_kind::S_concat:                         // concat
                case symbol_kind::S_dateFromString:                 // dateFromString
                case symbol_kind::S_dateToString:                   // dateToString
                case symbol_kind::S_indexOfBytes:                   // indexOfBytes
                case symbol_kind::S_indexOfCP:                      // indexOfCP
                case symbol_kind::S_ltrim:                          // ltrim
                case symbol_kind::S_regexFind:                      // regexFind
                case symbol_kind::S_regexFindAll:                   // regexFindAll
                case symbol_kind::S_regexMatch:                     // regexMatch
                case symbol_kind::S_regexArgs:                      // regexArgs
                case symbol_kind::S_replaceOne:                     // replaceOne
                case symbol_kind::S_replaceAll:                     // replaceAll
                case symbol_kind::S_rtrim:                          // rtrim
                case symbol_kind::S_split:                          // split
                case symbol_kind::S_strLenBytes:                    // strLenBytes
                case symbol_kind::S_strLenCP:                       // strLenCP
                case symbol_kind::S_strcasecmp:                     // strcasecmp
                case symbol_kind::S_substr:                         // substr
                case symbol_kind::S_substrBytes:                    // substrBytes
                case symbol_kind::S_substrCP:                       // substrCP
                case symbol_kind::S_toLower:                        // toLower
                case symbol_kind::S_toUpper:                        // toUpper
                case symbol_kind::S_trim:                           // trim
                case symbol_kind::S_compExprs:                      // compExprs
                case symbol_kind::S_cmp:                            // cmp
                case symbol_kind::S_eq:                             // eq
                case symbol_kind::S_gt:                             // gt
                case symbol_kind::S_gte:                            // gte
                case symbol_kind::S_lt:                             // lt
                case symbol_kind::S_lte:                            // lte
                case symbol_kind::S_ne:                             // ne
                case symbol_kind::S_dateExps:                       // dateExps
                case symbol_kind::S_dateFromParts:                  // dateFromParts
                case symbol_kind::S_dateToParts:                    // dateToParts
                case symbol_kind::S_dayOfMonth:                     // dayOfMonth
                case symbol_kind::S_dayOfWeek:                      // dayOfWeek
                case symbol_kind::S_dayOfYear:                      // dayOfYear
                case symbol_kind::S_hour:                           // hour
                case symbol_kind::S_isoDayOfWeek:                   // isoDayOfWeek
                case symbol_kind::S_isoWeek:                        // isoWeek
                case symbol_kind::S_isoWeekYear:                    // isoWeekYear
                case symbol_kind::S_millisecond:                    // millisecond
                case symbol_kind::S_minute:                         // minute
                case symbol_kind::S_month:                          // month
                case symbol_kind::S_second:                         // second
                case symbol_kind::S_week:                           // week
                case symbol_kind::S_year:                           // year
                case symbol_kind::S_typeExpression:                 // typeExpression
                case symbol_kind::S_convert:                        // convert
                case symbol_kind::S_toBool:                         // toBool
                case symbol_kind::S_toDate:                         // toDate
                case symbol_kind::S_toDecimal:                      // toDecimal
                case symbol_kind::S_toDouble:                       // toDouble
                case symbol_kind::S_toInt:                          // toInt
                case symbol_kind::S_toLong:                         // toLong
                case symbol_kind::S_toObjectId:                     // toObjectId
                case symbol_kind::S_toString:                       // toString
                case symbol_kind::S_type:                           // type
                case symbol_kind::S_abs:                            // abs
                case symbol_kind::S_ceil:                           // ceil
                case symbol_kind::S_divide:                         // divide
                case symbol_kind::S_exponent:                       // exponent
                case symbol_kind::S_floor:                          // floor
                case symbol_kind::S_ln:                             // ln
                case symbol_kind::S_log:                            // log
                case symbol_kind::S_logten:                         // logten
                case symbol_kind::S_mod:                            // mod
                case symbol_kind::S_multiply:                       // multiply
                case symbol_kind::S_pow:                            // pow
                case symbol_kind::S_round:                          // round
                case symbol_kind::S_sqrt:                           // sqrt
                case symbol_kind::S_subtract:                       // subtract
                case symbol_kind::S_trunc:                          // trunc
                case symbol_kind::S_arrayExps:                      // arrayExps
                case symbol_kind::S_arrayElemAt:                    // arrayElemAt
                case symbol_kind::S_arrayToObject:                  // arrayToObject
                case symbol_kind::S_concatArrays:                   // concatArrays
                case symbol_kind::S_filter:                         // filter
                case symbol_kind::S_first:                          // first
                case symbol_kind::S_in:                             // in
                case symbol_kind::S_indexOfArray:                   // indexOfArray
                case symbol_kind::S_isArray:                        // isArray
                case symbol_kind::S_setExpression:                  // setExpression
                case symbol_kind::S_allElementsTrue:                // allElementsTrue
                case symbol_kind::S_anyElementTrue:                 // anyElementTrue
                case symbol_kind::S_setDifference:                  // setDifference
                case symbol_kind::S_setEquals:                      // setEquals
                case symbol_kind::S_setIntersection:                // setIntersection
                case symbol_kind::S_setIsSubset:                    // setIsSubset
                case symbol_kind::S_setUnion:                       // setUnion
                case symbol_kind::S_trig:                           // trig
                case symbol_kind::S_sin:                            // sin
                case symbol_kind::S_cos:                            // cos
                case symbol_kind::S_tan:                            // tan
                case symbol_kind::S_sinh:                           // sinh
                case symbol_kind::S_cosh:                           // cosh
                case symbol_kind::S_tanh:                           // tanh
                case symbol_kind::S_asin:                           // asin
                case symbol_kind::S_acos:                           // acos
                case symbol_kind::S_atan:                           // atan
                case symbol_kind::S_asinh:                          // asinh
                case symbol_kind::S_acosh:                          // acosh
                case symbol_kind::S_atanh:                          // atanh
                case symbol_kind::S_atan2:                          // atan2
                case symbol_kind::S_degreesToRadians:               // degreesToRadians
                case symbol_kind::S_radiansToDegrees:               // radiansToDegrees
                case symbol_kind::S_nonArrayExpression:             // nonArrayExpression
                case symbol_kind::S_nonArrayCompoundExpression:     // nonArrayCompoundExpression
                case symbol_kind::S_aggregationOperator:            // aggregationOperator
                case symbol_kind::
                    S_aggregationOperatorWithoutSlice:           // aggregationOperatorWithoutSlice
                case symbol_kind::S_expressionSingletonArray:    // expressionSingletonArray
                case symbol_kind::S_singleArgExpression:         // singleArgExpression
                case symbol_kind::S_nonArrayNonObjExpression:    // nonArrayNonObjExpression
                case symbol_kind::S_matchExpression:             // matchExpression
                case symbol_kind::S_predicates:                  // predicates
                case symbol_kind::S_compoundMatchExprs:          // compoundMatchExprs
                case symbol_kind::S_predValue:                   // predValue
                case symbol_kind::S_additionalExprs:             // additionalExprs
                case symbol_kind::S_textArgCaseSensitive:        // textArgCaseSensitive
                case symbol_kind::S_textArgDiacriticSensitive:   // textArgDiacriticSensitive
                case symbol_kind::S_textArgLanguage:             // textArgLanguage
                case symbol_kind::S_textArgSearch:               // textArgSearch
                case symbol_kind::S_findProject:                 // findProject
                case symbol_kind::S_findProjectFields:           // findProjectFields
                case symbol_kind::S_topLevelFindProjection:      // topLevelFindProjection
                case symbol_kind::S_findProjection:              // findProjection
                case symbol_kind::S_findProjectionSlice:         // findProjectionSlice
                case symbol_kind::S_elemMatch:                   // elemMatch
                case symbol_kind::S_findProjectionObject:        // findProjectionObject
                case symbol_kind::S_findProjectionObjectFields:  // findProjectionObjectFields
                case symbol_kind::S_sortSpecs:                   // sortSpecs
                case symbol_kind::S_specList:                    // specList
                case symbol_kind::S_metaSort:                    // metaSort
                case symbol_kind::S_oneOrNegOne:                 // oneOrNegOne
                case symbol_kind::S_metaSortKeyword:             // metaSortKeyword
                    value.move<CNode>(std::move(that.value));
                    break;

                case symbol_kind::
                    S_aggregationProjectionFieldname:          // aggregationProjectionFieldname
                case symbol_kind::S_projectionFieldname:       // projectionFieldname
                case symbol_kind::S_expressionFieldname:       // expressionFieldname
                case symbol_kind::S_stageAsUserFieldname:      // stageAsUserFieldname
                case symbol_kind::S_argAsUserFieldname:        // argAsUserFieldname
                case symbol_kind::S_argAsProjectionPath:       // argAsProjectionPath
                case symbol_kind::S_aggExprAsUserFieldname:    // aggExprAsUserFieldname
                case symbol_kind::S_invariableUserFieldname:   // invariableUserFieldname
                case symbol_kind::S_sortFieldname:             // sortFieldname
                case symbol_kind::S_idAsUserFieldname:         // idAsUserFieldname
                case symbol_kind::S_elemMatchAsUserFieldname:  // elemMatchAsUserFieldname
                case symbol_kind::S_idAsProjectionPath:        // idAsProjectionPath
                case symbol_kind::S_valueFieldname:            // valueFieldname
                case symbol_kind::S_predFieldname:             // predFieldname
                case symbol_kind::S_logicalExprField:          // logicalExprField
                    value.move<CNode::Fieldname>(std::move(that.value));
                    break;

                case symbol_kind::S_DATE_LITERAL:  // "Date"
                    value.move<Date_t>(std::move(that.value));
                    break;

                case symbol_kind::S_DECIMAL_OTHER:  // "arbitrary decimal"
                    value.move<Decimal128>(std::move(that.value));
                    break;

                case symbol_kind::S_OBJECT_ID:  // "ObjectID"
                    value.move<OID>(std::move(that.value));
                    break;

                case symbol_kind::S_TIMESTAMP:  // "Timestamp"
                    value.move<Timestamp>(std::move(that.value));
                    break;

                case symbol_kind::S_MAX_KEY:  // "maxKey"
                    value.move<UserMaxKey>(std::move(that.value));
                    break;

                case symbol_kind::S_MIN_KEY:  // "minKey"
                    value.move<UserMinKey>(std::move(that.value));
                    break;

                case symbol_kind::S_JSNULL:  // "null"
                    value.move<UserNull>(std::move(that.value));
                    break;

                case symbol_kind::S_UNDEFINED:  // "undefined"
                    value.move<UserUndefined>(std::move(that.value));
                    break;

                case symbol_kind::S_DOUBLE_OTHER:  // "arbitrary double"
                    value.move<double>(std::move(that.value));
                    break;

                case symbol_kind::S_INT_OTHER:  // "arbitrary integer"
                    value.move<int>(std::move(that.value));
                    break;

                case symbol_kind::S_LONG_OTHER:  // "arbitrary long"
                    value.move<long long>(std::move(that.value));
                    break;

                case symbol_kind::S_aggregationProjectField:  // aggregationProjectField
                case symbol_kind::
                    S_aggregationProjectionObjectField:         // aggregationProjectionObjectField
                case symbol_kind::S_expressionField:            // expressionField
                case symbol_kind::S_valueField:                 // valueField
                case symbol_kind::S_onErrorArg:                 // onErrorArg
                case symbol_kind::S_onNullArg:                  // onNullArg
                case symbol_kind::S_asArg:                      // asArg
                case symbol_kind::S_formatArg:                  // formatArg
                case symbol_kind::S_timezoneArg:                // timezoneArg
                case symbol_kind::S_charsArg:                   // charsArg
                case symbol_kind::S_optionsArg:                 // optionsArg
                case symbol_kind::S_hourArg:                    // hourArg
                case symbol_kind::S_minuteArg:                  // minuteArg
                case symbol_kind::S_secondArg:                  // secondArg
                case symbol_kind::S_millisecondArg:             // millisecondArg
                case symbol_kind::S_dayArg:                     // dayArg
                case symbol_kind::S_isoWeekArg:                 // isoWeekArg
                case symbol_kind::S_iso8601Arg:                 // iso8601Arg
                case symbol_kind::S_monthArg:                   // monthArg
                case symbol_kind::S_isoDayOfWeekArg:            // isoDayOfWeekArg
                case symbol_kind::S_predicate:                  // predicate
                case symbol_kind::S_fieldPredicate:             // fieldPredicate
                case symbol_kind::S_logicalExpr:                // logicalExpr
                case symbol_kind::S_operatorExpression:         // operatorExpression
                case symbol_kind::S_notExpr:                    // notExpr
                case symbol_kind::S_matchMod:                   // matchMod
                case symbol_kind::S_existsExpr:                 // existsExpr
                case symbol_kind::S_typeExpr:                   // typeExpr
                case symbol_kind::S_commentExpr:                // commentExpr
                case symbol_kind::S_matchExpr:                  // matchExpr
                case symbol_kind::S_matchText:                  // matchText
                case symbol_kind::S_matchWhere:                 // matchWhere
                case symbol_kind::S_findProjectField:           // findProjectField
                case symbol_kind::S_findProjectionObjectField:  // findProjectionObjectField
                case symbol_kind::S_sortSpec:                   // sortSpec
                    value.move<std::pair<CNode::Fieldname, CNode>>(std::move(that.value));
                    break;

                case symbol_kind::S_FIELDNAME:              // "fieldname"
                case symbol_kind::S_DOLLAR_PREF_FIELDNAME:  // "$-prefixed fieldname"
                case symbol_kind::S_STRING:                 // "string"
                case symbol_kind::S_DOLLAR_STRING:          // "$-prefixed string"
                case symbol_kind::S_DOLLAR_DOLLAR_STRING:   // "$$-prefixed string"
                case symbol_kind::S_arg:                    // arg
                    value.move<std::string>(std::move(that.value));
                    break;

                case symbol_kind::S_expressions:    // expressions
                case symbol_kind::S_values:         // values
                case symbol_kind::S_exprZeroToTwo:  // exprZeroToTwo
                case symbol_kind::S_typeValues:     // typeValues
                    value.move<std::vector<CNode>>(std::move(that.value));
                    break;

                case symbol_kind::S_DOTTED_FIELDNAME:  // "fieldname containing dotted path"
                    value.move<std::vector<std::string>>(std::move(that.value));
                    break;

                default:
                    break;
            }
        }
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
            symbol_kind_type yykind = this->kind();
            basic_symbol<Base>& yysym = *this;
            (void)yysym;
            switch (yykind) {
                default:
                    break;
            }

            // Value type destructor.
            switch (yykind) {
                case symbol_kind::S_BINARY:  // "BinData"
                    value.template destroy<BSONBinData>();
                    break;

                case symbol_kind::S_JAVASCRIPT:  // "Code"
                    value.template destroy<BSONCode>();
                    break;

                case symbol_kind::S_JAVASCRIPT_W_SCOPE:  // "CodeWScope"
                    value.template destroy<BSONCodeWScope>();
                    break;

                case symbol_kind::S_DB_POINTER:  // "dbPointer"
                    value.template destroy<BSONDBRef>();
                    break;

                case symbol_kind::S_REGEX:  // "regex"
                    value.template destroy<BSONRegEx>();
                    break;

                case symbol_kind::S_SYMBOL:  // "Symbol"
                    value.template destroy<BSONSymbol>();
                    break;

                case symbol_kind::S_dbPointer:                 // dbPointer
                case symbol_kind::S_javascript:                // javascript
                case symbol_kind::S_symbol:                    // symbol
                case symbol_kind::S_javascriptWScope:          // javascriptWScope
                case symbol_kind::S_int:                       // int
                case symbol_kind::S_timestamp:                 // timestamp
                case symbol_kind::S_long:                      // long
                case symbol_kind::S_double:                    // double
                case symbol_kind::S_decimal:                   // decimal
                case symbol_kind::S_minKey:                    // minKey
                case symbol_kind::S_maxKey:                    // maxKey
                case symbol_kind::S_value:                     // value
                case symbol_kind::S_string:                    // string
                case symbol_kind::S_aggregationFieldPath:      // aggregationFieldPath
                case symbol_kind::S_binary:                    // binary
                case symbol_kind::S_undefined:                 // undefined
                case symbol_kind::S_objectId:                  // objectId
                case symbol_kind::S_bool:                      // bool
                case symbol_kind::S_date:                      // date
                case symbol_kind::S_null:                      // null
                case symbol_kind::S_regex:                     // regex
                case symbol_kind::S_simpleValue:               // simpleValue
                case symbol_kind::S_compoundValue:             // compoundValue
                case symbol_kind::S_valueArray:                // valueArray
                case symbol_kind::S_valueObject:               // valueObject
                case symbol_kind::S_valueFields:               // valueFields
                case symbol_kind::S_variable:                  // variable
                case symbol_kind::S_typeArray:                 // typeArray
                case symbol_kind::S_typeValue:                 // typeValue
                case symbol_kind::S_pipeline:                  // pipeline
                case symbol_kind::S_stageList:                 // stageList
                case symbol_kind::S_stage:                     // stage
                case symbol_kind::S_inhibitOptimization:       // inhibitOptimization
                case symbol_kind::S_unionWith:                 // unionWith
                case symbol_kind::S_skip:                      // skip
                case symbol_kind::S_limit:                     // limit
                case symbol_kind::S_matchStage:                // matchStage
                case symbol_kind::S_project:                   // project
                case symbol_kind::S_sample:                    // sample
                case symbol_kind::S_aggregationProjectFields:  // aggregationProjectFields
                case symbol_kind::
                    S_aggregationProjectionObjectFields:  // aggregationProjectionObjectFields
                case symbol_kind::S_topLevelAggregationProjection:  // topLevelAggregationProjection
                case symbol_kind::S_aggregationProjection:          // aggregationProjection
                case symbol_kind::S_projectionCommon:               // projectionCommon
                case symbol_kind::S_aggregationProjectionObject:    // aggregationProjectionObject
                case symbol_kind::S_num:                            // num
                case symbol_kind::S_expression:                     // expression
                case symbol_kind::S_exprFixedTwoArg:                // exprFixedTwoArg
                case symbol_kind::S_exprFixedThreeArg:              // exprFixedThreeArg
                case symbol_kind::S_slice:                          // slice
                case symbol_kind::S_expressionArray:                // expressionArray
                case symbol_kind::S_expressionObject:               // expressionObject
                case symbol_kind::S_expressionFields:               // expressionFields
                case symbol_kind::S_maths:                          // maths
                case symbol_kind::S_meta:                           // meta
                case symbol_kind::S_add:                            // add
                case symbol_kind::S_boolExprs:                      // boolExprs
                case symbol_kind::S_and:                            // and
                case symbol_kind::S_or:                             // or
                case symbol_kind::S_not:                            // not
                case symbol_kind::S_literalEscapes:                 // literalEscapes
                case symbol_kind::S_const:                          // const
                case symbol_kind::S_literal:                        // literal
                case symbol_kind::S_stringExps:                     // stringExps
                case symbol_kind::S_concat:                         // concat
                case symbol_kind::S_dateFromString:                 // dateFromString
                case symbol_kind::S_dateToString:                   // dateToString
                case symbol_kind::S_indexOfBytes:                   // indexOfBytes
                case symbol_kind::S_indexOfCP:                      // indexOfCP
                case symbol_kind::S_ltrim:                          // ltrim
                case symbol_kind::S_regexFind:                      // regexFind
                case symbol_kind::S_regexFindAll:                   // regexFindAll
                case symbol_kind::S_regexMatch:                     // regexMatch
                case symbol_kind::S_regexArgs:                      // regexArgs
                case symbol_kind::S_replaceOne:                     // replaceOne
                case symbol_kind::S_replaceAll:                     // replaceAll
                case symbol_kind::S_rtrim:                          // rtrim
                case symbol_kind::S_split:                          // split
                case symbol_kind::S_strLenBytes:                    // strLenBytes
                case symbol_kind::S_strLenCP:                       // strLenCP
                case symbol_kind::S_strcasecmp:                     // strcasecmp
                case symbol_kind::S_substr:                         // substr
                case symbol_kind::S_substrBytes:                    // substrBytes
                case symbol_kind::S_substrCP:                       // substrCP
                case symbol_kind::S_toLower:                        // toLower
                case symbol_kind::S_toUpper:                        // toUpper
                case symbol_kind::S_trim:                           // trim
                case symbol_kind::S_compExprs:                      // compExprs
                case symbol_kind::S_cmp:                            // cmp
                case symbol_kind::S_eq:                             // eq
                case symbol_kind::S_gt:                             // gt
                case symbol_kind::S_gte:                            // gte
                case symbol_kind::S_lt:                             // lt
                case symbol_kind::S_lte:                            // lte
                case symbol_kind::S_ne:                             // ne
                case symbol_kind::S_dateExps:                       // dateExps
                case symbol_kind::S_dateFromParts:                  // dateFromParts
                case symbol_kind::S_dateToParts:                    // dateToParts
                case symbol_kind::S_dayOfMonth:                     // dayOfMonth
                case symbol_kind::S_dayOfWeek:                      // dayOfWeek
                case symbol_kind::S_dayOfYear:                      // dayOfYear
                case symbol_kind::S_hour:                           // hour
                case symbol_kind::S_isoDayOfWeek:                   // isoDayOfWeek
                case symbol_kind::S_isoWeek:                        // isoWeek
                case symbol_kind::S_isoWeekYear:                    // isoWeekYear
                case symbol_kind::S_millisecond:                    // millisecond
                case symbol_kind::S_minute:                         // minute
                case symbol_kind::S_month:                          // month
                case symbol_kind::S_second:                         // second
                case symbol_kind::S_week:                           // week
                case symbol_kind::S_year:                           // year
                case symbol_kind::S_typeExpression:                 // typeExpression
                case symbol_kind::S_convert:                        // convert
                case symbol_kind::S_toBool:                         // toBool
                case symbol_kind::S_toDate:                         // toDate
                case symbol_kind::S_toDecimal:                      // toDecimal
                case symbol_kind::S_toDouble:                       // toDouble
                case symbol_kind::S_toInt:                          // toInt
                case symbol_kind::S_toLong:                         // toLong
                case symbol_kind::S_toObjectId:                     // toObjectId
                case symbol_kind::S_toString:                       // toString
                case symbol_kind::S_type:                           // type
                case symbol_kind::S_abs:                            // abs
                case symbol_kind::S_ceil:                           // ceil
                case symbol_kind::S_divide:                         // divide
                case symbol_kind::S_exponent:                       // exponent
                case symbol_kind::S_floor:                          // floor
                case symbol_kind::S_ln:                             // ln
                case symbol_kind::S_log:                            // log
                case symbol_kind::S_logten:                         // logten
                case symbol_kind::S_mod:                            // mod
                case symbol_kind::S_multiply:                       // multiply
                case symbol_kind::S_pow:                            // pow
                case symbol_kind::S_round:                          // round
                case symbol_kind::S_sqrt:                           // sqrt
                case symbol_kind::S_subtract:                       // subtract
                case symbol_kind::S_trunc:                          // trunc
                case symbol_kind::S_arrayExps:                      // arrayExps
                case symbol_kind::S_arrayElemAt:                    // arrayElemAt
                case symbol_kind::S_arrayToObject:                  // arrayToObject
                case symbol_kind::S_concatArrays:                   // concatArrays
                case symbol_kind::S_filter:                         // filter
                case symbol_kind::S_first:                          // first
                case symbol_kind::S_in:                             // in
                case symbol_kind::S_indexOfArray:                   // indexOfArray
                case symbol_kind::S_isArray:                        // isArray
                case symbol_kind::S_setExpression:                  // setExpression
                case symbol_kind::S_allElementsTrue:                // allElementsTrue
                case symbol_kind::S_anyElementTrue:                 // anyElementTrue
                case symbol_kind::S_setDifference:                  // setDifference
                case symbol_kind::S_setEquals:                      // setEquals
                case symbol_kind::S_setIntersection:                // setIntersection
                case symbol_kind::S_setIsSubset:                    // setIsSubset
                case symbol_kind::S_setUnion:                       // setUnion
                case symbol_kind::S_trig:                           // trig
                case symbol_kind::S_sin:                            // sin
                case symbol_kind::S_cos:                            // cos
                case symbol_kind::S_tan:                            // tan
                case symbol_kind::S_sinh:                           // sinh
                case symbol_kind::S_cosh:                           // cosh
                case symbol_kind::S_tanh:                           // tanh
                case symbol_kind::S_asin:                           // asin
                case symbol_kind::S_acos:                           // acos
                case symbol_kind::S_atan:                           // atan
                case symbol_kind::S_asinh:                          // asinh
                case symbol_kind::S_acosh:                          // acosh
                case symbol_kind::S_atanh:                          // atanh
                case symbol_kind::S_atan2:                          // atan2
                case symbol_kind::S_degreesToRadians:               // degreesToRadians
                case symbol_kind::S_radiansToDegrees:               // radiansToDegrees
                case symbol_kind::S_nonArrayExpression:             // nonArrayExpression
                case symbol_kind::S_nonArrayCompoundExpression:     // nonArrayCompoundExpression
                case symbol_kind::S_aggregationOperator:            // aggregationOperator
                case symbol_kind::
                    S_aggregationOperatorWithoutSlice:           // aggregationOperatorWithoutSlice
                case symbol_kind::S_expressionSingletonArray:    // expressionSingletonArray
                case symbol_kind::S_singleArgExpression:         // singleArgExpression
                case symbol_kind::S_nonArrayNonObjExpression:    // nonArrayNonObjExpression
                case symbol_kind::S_matchExpression:             // matchExpression
                case symbol_kind::S_predicates:                  // predicates
                case symbol_kind::S_compoundMatchExprs:          // compoundMatchExprs
                case symbol_kind::S_predValue:                   // predValue
                case symbol_kind::S_additionalExprs:             // additionalExprs
                case symbol_kind::S_textArgCaseSensitive:        // textArgCaseSensitive
                case symbol_kind::S_textArgDiacriticSensitive:   // textArgDiacriticSensitive
                case symbol_kind::S_textArgLanguage:             // textArgLanguage
                case symbol_kind::S_textArgSearch:               // textArgSearch
                case symbol_kind::S_findProject:                 // findProject
                case symbol_kind::S_findProjectFields:           // findProjectFields
                case symbol_kind::S_topLevelFindProjection:      // topLevelFindProjection
                case symbol_kind::S_findProjection:              // findProjection
                case symbol_kind::S_findProjectionSlice:         // findProjectionSlice
                case symbol_kind::S_elemMatch:                   // elemMatch
                case symbol_kind::S_findProjectionObject:        // findProjectionObject
                case symbol_kind::S_findProjectionObjectFields:  // findProjectionObjectFields
                case symbol_kind::S_sortSpecs:                   // sortSpecs
                case symbol_kind::S_specList:                    // specList
                case symbol_kind::S_metaSort:                    // metaSort
                case symbol_kind::S_oneOrNegOne:                 // oneOrNegOne
                case symbol_kind::S_metaSortKeyword:             // metaSortKeyword
                    value.template destroy<CNode>();
                    break;

                case symbol_kind::
                    S_aggregationProjectionFieldname:          // aggregationProjectionFieldname
                case symbol_kind::S_projectionFieldname:       // projectionFieldname
                case symbol_kind::S_expressionFieldname:       // expressionFieldname
                case symbol_kind::S_stageAsUserFieldname:      // stageAsUserFieldname
                case symbol_kind::S_argAsUserFieldname:        // argAsUserFieldname
                case symbol_kind::S_argAsProjectionPath:       // argAsProjectionPath
                case symbol_kind::S_aggExprAsUserFieldname:    // aggExprAsUserFieldname
                case symbol_kind::S_invariableUserFieldname:   // invariableUserFieldname
                case symbol_kind::S_sortFieldname:             // sortFieldname
                case symbol_kind::S_idAsUserFieldname:         // idAsUserFieldname
                case symbol_kind::S_elemMatchAsUserFieldname:  // elemMatchAsUserFieldname
                case symbol_kind::S_idAsProjectionPath:        // idAsProjectionPath
                case symbol_kind::S_valueFieldname:            // valueFieldname
                case symbol_kind::S_predFieldname:             // predFieldname
                case symbol_kind::S_logicalExprField:          // logicalExprField
                    value.template destroy<CNode::Fieldname>();
                    break;

                case symbol_kind::S_DATE_LITERAL:  // "Date"
                    value.template destroy<Date_t>();
                    break;

                case symbol_kind::S_DECIMAL_OTHER:  // "arbitrary decimal"
                    value.template destroy<Decimal128>();
                    break;

                case symbol_kind::S_OBJECT_ID:  // "ObjectID"
                    value.template destroy<OID>();
                    break;

                case symbol_kind::S_TIMESTAMP:  // "Timestamp"
                    value.template destroy<Timestamp>();
                    break;

                case symbol_kind::S_MAX_KEY:  // "maxKey"
                    value.template destroy<UserMaxKey>();
                    break;

                case symbol_kind::S_MIN_KEY:  // "minKey"
                    value.template destroy<UserMinKey>();
                    break;

                case symbol_kind::S_JSNULL:  // "null"
                    value.template destroy<UserNull>();
                    break;

                case symbol_kind::S_UNDEFINED:  // "undefined"
                    value.template destroy<UserUndefined>();
                    break;

                case symbol_kind::S_DOUBLE_OTHER:  // "arbitrary double"
                    value.template destroy<double>();
                    break;

                case symbol_kind::S_INT_OTHER:  // "arbitrary integer"
                    value.template destroy<int>();
                    break;

                case symbol_kind::S_LONG_OTHER:  // "arbitrary long"
                    value.template destroy<long long>();
                    break;

                case symbol_kind::S_aggregationProjectField:  // aggregationProjectField
                case symbol_kind::
                    S_aggregationProjectionObjectField:         // aggregationProjectionObjectField
                case symbol_kind::S_expressionField:            // expressionField
                case symbol_kind::S_valueField:                 // valueField
                case symbol_kind::S_onErrorArg:                 // onErrorArg
                case symbol_kind::S_onNullArg:                  // onNullArg
                case symbol_kind::S_asArg:                      // asArg
                case symbol_kind::S_formatArg:                  // formatArg
                case symbol_kind::S_timezoneArg:                // timezoneArg
                case symbol_kind::S_charsArg:                   // charsArg
                case symbol_kind::S_optionsArg:                 // optionsArg
                case symbol_kind::S_hourArg:                    // hourArg
                case symbol_kind::S_minuteArg:                  // minuteArg
                case symbol_kind::S_secondArg:                  // secondArg
                case symbol_kind::S_millisecondArg:             // millisecondArg
                case symbol_kind::S_dayArg:                     // dayArg
                case symbol_kind::S_isoWeekArg:                 // isoWeekArg
                case symbol_kind::S_iso8601Arg:                 // iso8601Arg
                case symbol_kind::S_monthArg:                   // monthArg
                case symbol_kind::S_isoDayOfWeekArg:            // isoDayOfWeekArg
                case symbol_kind::S_predicate:                  // predicate
                case symbol_kind::S_fieldPredicate:             // fieldPredicate
                case symbol_kind::S_logicalExpr:                // logicalExpr
                case symbol_kind::S_operatorExpression:         // operatorExpression
                case symbol_kind::S_notExpr:                    // notExpr
                case symbol_kind::S_matchMod:                   // matchMod
                case symbol_kind::S_existsExpr:                 // existsExpr
                case symbol_kind::S_typeExpr:                   // typeExpr
                case symbol_kind::S_commentExpr:                // commentExpr
                case symbol_kind::S_matchExpr:                  // matchExpr
                case symbol_kind::S_matchText:                  // matchText
                case symbol_kind::S_matchWhere:                 // matchWhere
                case symbol_kind::S_findProjectField:           // findProjectField
                case symbol_kind::S_findProjectionObjectField:  // findProjectionObjectField
                case symbol_kind::S_sortSpec:                   // sortSpec
                    value.template destroy<std::pair<CNode::Fieldname, CNode>>();
                    break;

                case symbol_kind::S_FIELDNAME:              // "fieldname"
                case symbol_kind::S_DOLLAR_PREF_FIELDNAME:  // "$-prefixed fieldname"
                case symbol_kind::S_STRING:                 // "string"
                case symbol_kind::S_DOLLAR_STRING:          // "$-prefixed string"
                case symbol_kind::S_DOLLAR_DOLLAR_STRING:   // "$$-prefixed string"
                case symbol_kind::S_arg:                    // arg
                    value.template destroy<std::string>();
                    break;

                case symbol_kind::S_expressions:    // expressions
                case symbol_kind::S_values:         // values
                case symbol_kind::S_exprZeroToTwo:  // exprZeroToTwo
                case symbol_kind::S_typeValues:     // typeValues
                    value.template destroy<std::vector<CNode>>();
                    break;

                case symbol_kind::S_DOTTED_FIELDNAME:  // "fieldname containing dotted path"
                    value.template destroy<std::vector<std::string>>();
                    break;

                default:
                    break;
            }

            Base::clear();
        }

        /// The user-facing name of this symbol.
        std::string name() const YY_NOEXCEPT {
            return ParserGen::symbol_name(this->kind());
        }

        /// Backward compatibility (Bison 3.6).
        symbol_kind_type type_get() const YY_NOEXCEPT;

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
    struct by_kind {
        /// Default constructor.
        by_kind();

#if 201103L <= YY_CPLUSPLUS
        /// Move constructor.
        by_kind(by_kind&& that);
#endif

        /// Copy constructor.
        by_kind(const by_kind& that);

        /// The symbol kind as needed by the constructor.
        typedef token_kind_type kind_type;

        /// Constructor from (external) token numbers.
        by_kind(kind_type t);

        /// Record that this symbol is empty.
        void clear();

        /// Steal the symbol kind from \a that.
        void move(by_kind& that);

        /// The (internal) type number (corresponding to \a type).
        /// \a empty when empty.
        symbol_kind_type kind() const YY_NOEXCEPT;

        /// Backward compatibility (Bison 3.6).
        symbol_kind_type type_get() const YY_NOEXCEPT;

        /// The symbol kind.
        /// \a S_YYEMPTY when empty.
        symbol_kind_type kind_;
    };

    /// Backward compatibility for a private implementation detail (Bison 3.6).
    typedef by_kind by_type;

    /// "External" symbols: returned by the scanner.
    struct symbol_type : basic_symbol<by_kind> {
        /// Superclass.
        typedef basic_symbol<by_kind> super_type;

        /// Empty symbol.
        symbol_type() {}

        /// Constructor for valueless symbols, and symbols from each type.
#if 201103L <= YY_CPLUSPLUS
        symbol_type(int tok, location_type l) : super_type(token_type(tok), std::move(l)) {
            YY_ASSERT(
                tok == token::END_OF_FILE || tok == token::YYerror || tok == token::YYUNDEF ||
                tok == token::ABS || tok == token::ACOS || tok == token::ACOSH ||
                tok == token::ADD || tok == token::ALL_ELEMENTS_TRUE || tok == token::AND ||
                tok == token::ANY_ELEMENT_TRUE || tok == token::ARG_CASE_SENSITIVE ||
                tok == token::ARRAY_ELEM_AT || tok == token::ARRAY_TO_OBJECT ||
                tok == token::ARG_AS || tok == token::ARG_CHARS || tok == token::ARG_COLL ||
                tok == token::ARG_COND || tok == token::ARG_DATE || tok == token::ARG_DATE_STRING ||
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
                tok == token::CONCAT || tok == token::CONCAT_ARRAYS || tok == token::CONST_EXPR ||
                tok == token::CONVERT || tok == token::COS || tok == token::COSH ||
                tok == token::DATE_FROM_PARTS || tok == token::DATE_FROM_STRING ||
                tok == token::DATE_TO_PARTS || tok == token::DATE_TO_STRING ||
                tok == token::DAY_OF_MONTH || tok == token::DAY_OF_WEEK ||
                tok == token::DAY_OF_YEAR || tok == token::DECIMAL_NEGATIVE_ONE ||
                tok == token::DECIMAL_ONE || tok == token::DECIMAL_ZERO ||
                tok == token::DEGREES_TO_RADIANS || tok == token::DIVIDE ||
                tok == token::DOUBLE_NEGATIVE_ONE || tok == token::DOUBLE_ONE ||
                tok == token::DOUBLE_ZERO || tok == token::ELEM_MATCH || tok == token::END_ARRAY ||
                tok == token::END_OBJECT || tok == token::EQ || tok == token::EXISTS ||
                tok == token::EXPONENT || tok == token::EXPR || tok == token::FILTER ||
                tok == token::FIRST || tok == token::FLOOR || tok == token::GEO_NEAR_DISTANCE ||
                tok == token::GEO_NEAR_POINT || tok == token::GT || tok == token::GTE ||
                tok == token::HOUR || tok == token::ID || tok == token::IN ||
                tok == token::INDEX_KEY || tok == token::INDEX_OF_ARRAY ||
                tok == token::INDEX_OF_BYTES || tok == token::INDEX_OF_CP ||
                tok == token::INT_NEGATIVE_ONE || tok == token::INT_ONE || tok == token::INT_ZERO ||
                tok == token::IS_ARRAY || tok == token::ISO_DAY_OF_WEEK || tok == token::ISO_WEEK ||
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
                tok == token::END_OF_FILE || tok == token::YYerror || tok == token::YYUNDEF ||
                tok == token::ABS || tok == token::ACOS || tok == token::ACOSH ||
                tok == token::ADD || tok == token::ALL_ELEMENTS_TRUE || tok == token::AND ||
                tok == token::ANY_ELEMENT_TRUE || tok == token::ARG_CASE_SENSITIVE ||
                tok == token::ARRAY_ELEM_AT || tok == token::ARRAY_TO_OBJECT ||
                tok == token::ARG_AS || tok == token::ARG_CHARS || tok == token::ARG_COLL ||
                tok == token::ARG_COND || tok == token::ARG_DATE || tok == token::ARG_DATE_STRING ||
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
                tok == token::CONCAT || tok == token::CONCAT_ARRAYS || tok == token::CONST_EXPR ||
                tok == token::CONVERT || tok == token::COS || tok == token::COSH ||
                tok == token::DATE_FROM_PARTS || tok == token::DATE_FROM_STRING ||
                tok == token::DATE_TO_PARTS || tok == token::DATE_TO_STRING ||
                tok == token::DAY_OF_MONTH || tok == token::DAY_OF_WEEK ||
                tok == token::DAY_OF_YEAR || tok == token::DECIMAL_NEGATIVE_ONE ||
                tok == token::DECIMAL_ONE || tok == token::DECIMAL_ZERO ||
                tok == token::DEGREES_TO_RADIANS || tok == token::DIVIDE ||
                tok == token::DOUBLE_NEGATIVE_ONE || tok == token::DOUBLE_ONE ||
                tok == token::DOUBLE_ZERO || tok == token::ELEM_MATCH || tok == token::END_ARRAY ||
                tok == token::END_OBJECT || tok == token::EQ || tok == token::EXISTS ||
                tok == token::EXPONENT || tok == token::EXPR || tok == token::FILTER ||
                tok == token::FIRST || tok == token::FLOOR || tok == token::GEO_NEAR_DISTANCE ||
                tok == token::GEO_NEAR_POINT || tok == token::GT || tok == token::GTE ||
                tok == token::HOUR || tok == token::ID || tok == token::IN ||
                tok == token::INDEX_KEY || tok == token::INDEX_OF_ARRAY ||
                tok == token::INDEX_OF_BYTES || tok == token::INDEX_OF_CP ||
                tok == token::INT_NEGATIVE_ONE || tok == token::INT_ONE || tok == token::INT_ZERO ||
                tok == token::IS_ARRAY || tok == token::ISO_DAY_OF_WEEK || tok == token::ISO_WEEK ||
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

#if 201103L <= YY_CPLUSPLUS
    /// Non copyable.
    ParserGen(const ParserGen&) = delete;
    /// Non copyable.
    ParserGen& operator=(const ParserGen&) = delete;
#endif

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

    /// The user-facing name of the symbol whose (internal) number is
    /// YYSYMBOL.  No bounds checking.
    static std::string symbol_name(symbol_kind_type yysymbol);

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
    static symbol_type make_YYerror(location_type l) {
        return symbol_type(token::YYerror, std::move(l));
    }
#else
    static symbol_type make_YYerror(const location_type& l) {
        return symbol_type(token::YYerror, l);
    }
#endif
#if 201103L <= YY_CPLUSPLUS
    static symbol_type make_YYUNDEF(location_type l) {
        return symbol_type(token::YYUNDEF, std::move(l));
    }
#else
    static symbol_type make_YYUNDEF(const location_type& l) {
        return symbol_type(token::YYUNDEF, l);
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
    static symbol_type make_ARRAY_ELEM_AT(location_type l) {
        return symbol_type(token::ARRAY_ELEM_AT, std::move(l));
    }
#else
    static symbol_type make_ARRAY_ELEM_AT(const location_type& l) {
        return symbol_type(token::ARRAY_ELEM_AT, l);
    }
#endif
#if 201103L <= YY_CPLUSPLUS
    static symbol_type make_ARRAY_TO_OBJECT(location_type l) {
        return symbol_type(token::ARRAY_TO_OBJECT, std::move(l));
    }
#else
    static symbol_type make_ARRAY_TO_OBJECT(const location_type& l) {
        return symbol_type(token::ARRAY_TO_OBJECT, l);
    }
#endif
#if 201103L <= YY_CPLUSPLUS
    static symbol_type make_ARG_AS(location_type l) {
        return symbol_type(token::ARG_AS, std::move(l));
    }
#else
    static symbol_type make_ARG_AS(const location_type& l) {
        return symbol_type(token::ARG_AS, l);
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
    static symbol_type make_ARG_COND(location_type l) {
        return symbol_type(token::ARG_COND, std::move(l));
    }
#else
    static symbol_type make_ARG_COND(const location_type& l) {
        return symbol_type(token::ARG_COND, l);
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
    static symbol_type make_CONCAT_ARRAYS(location_type l) {
        return symbol_type(token::CONCAT_ARRAYS, std::move(l));
    }
#else
    static symbol_type make_CONCAT_ARRAYS(const location_type& l) {
        return symbol_type(token::CONCAT_ARRAYS, l);
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
    static symbol_type make_FILTER(location_type l) {
        return symbol_type(token::FILTER, std::move(l));
    }
#else
    static symbol_type make_FILTER(const location_type& l) {
        return symbol_type(token::FILTER, l);
    }
#endif
#if 201103L <= YY_CPLUSPLUS
    static symbol_type make_FIRST(location_type l) {
        return symbol_type(token::FIRST, std::move(l));
    }
#else
    static symbol_type make_FIRST(const location_type& l) {
        return symbol_type(token::FIRST, l);
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
    static symbol_type make_IN(location_type l) {
        return symbol_type(token::IN, std::move(l));
    }
#else
    static symbol_type make_IN(const location_type& l) {
        return symbol_type(token::IN, l);
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
    static symbol_type make_INDEX_OF_ARRAY(location_type l) {
        return symbol_type(token::INDEX_OF_ARRAY, std::move(l));
    }
#else
    static symbol_type make_INDEX_OF_ARRAY(const location_type& l) {
        return symbol_type(token::INDEX_OF_ARRAY, l);
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
    static symbol_type make_IS_ARRAY(location_type l) {
        return symbol_type(token::IS_ARRAY, std::move(l));
    }
#else
    static symbol_type make_IS_ARRAY(const location_type& l) {
        return symbol_type(token::IS_ARRAY, l);
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


    class context {
    public:
        context(const ParserGen& yyparser, const symbol_type& yyla);
        const symbol_type& lookahead() const {
            return yyla_;
        }
        symbol_kind_type token() const {
            return yyla_.kind();
        }
        const location_type& location() const {
            return yyla_.location;
        }

        /// Put in YYARG at most YYARGN of the expected tokens, and return the
        /// number of tokens stored in YYARG.  If YYARG is null, return the
        /// number of expected tokens (guaranteed to be less than YYNTOKENS).
        int expected_tokens(symbol_kind_type yyarg[], int yyargn) const;

    private:
        const ParserGen& yyparser_;
        const symbol_type& yyla_;
    };

private:
#if YY_CPLUSPLUS < 201103L
    /// Non copyable.
    ParserGen(const ParserGen&);
    /// Non copyable.
    ParserGen& operator=(const ParserGen&);
#endif


    /// Stored state numbers (used for stacks).
    typedef short state_type;

    /// The arguments of the error message.
    int yy_syntax_error_arguments_(const context& yyctx,
                                   symbol_kind_type yyarg[],
                                   int yyargn) const;

    /// Generate an error message.
    /// \param yyctx     the context in which the error occurred.
    virtual std::string yysyntax_error_(const context& yyctx) const;
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

    /// Convert a scanner token kind \a t to a symbol kind.
    /// In theory \a t should be a token_kind_type, but character literals
    /// are valid, yet not members of the token_type enum.
    static symbol_kind_type yytranslate_(int t);

    /// Convert the symbol name \a n to a form suitable for a diagnostic.
    static std::string yytnamerr_(const char* yystr);

    /// For a symbol, its name in clear.
    static const char* const yytname_[];


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


#if YYDEBUG
    // YYRLINE[YYN] -- Source line where rule number YYN was defined.
    static const short yyrline_[];
    /// Report on the debug stream that the rule \a r is going to be reduced.
    virtual void yy_reduce_print_(int r) const;
    /// Print the state stack on the debug stream.
    virtual void yy_stack_print_() const;

    /// Debugging level.
    int yydebug_;
    /// Debug stream.
    std::ostream* yycdebug_;

    /// \brief Display a symbol kind, value and location.
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

        /// The symbol kind as needed by the constructor.
        typedef state_type kind_type;

        /// Constructor.
        by_state(kind_type s) YY_NOEXCEPT;

        /// Copy constructor.
        by_state(const by_state& that) YY_NOEXCEPT;

        /// Record that this symbol is empty.
        void clear() YY_NOEXCEPT;

        /// Steal the symbol kind from \a that.
        void move(by_state& that);

        /// The symbol kind (corresponding to \a state).
        /// \a symbol_kind::S_YYEMPTY when empty.
        symbol_kind_type kind() const YY_NOEXCEPT;

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
        typedef typename S::iterator iterator;
        typedef typename S::const_iterator const_iterator;
        typedef typename S::size_type size_type;
        typedef typename std::ptrdiff_t index_type;

        stack(size_type n = 200) : seq_(n) {}

#if 201103L <= YY_CPLUSPLUS
        /// Non copyable.
        stack(const stack&) = delete;
        /// Non copyable.
        stack& operator=(const stack&) = delete;
#endif

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

        /// Iterator on top of the stack (going downwards).
        const_iterator begin() const YY_NOEXCEPT {
            return seq_.begin();
        }

        /// Bottom of the stack.
        const_iterator end() const YY_NOEXCEPT {
            return seq_.end();
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
#if YY_CPLUSPLUS < 201103L
        /// Non copyable.
        stack(const stack&);
        /// Non copyable.
        stack& operator=(const stack&);
#endif
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

    /// Constants.
    enum {
        yylast_ = 5490,  ///< Last index in yytable_.
        yynnts_ = 257,   ///< Number of nonterminal symbols.
        yyfinal_ = 14    ///< Termination state number.
    };


    // User arguments.
    BSONLexer& lexer;
    CNode* cst;
};

inline ParserGen::symbol_kind_type ParserGen::yytranslate_(int t) {
    return static_cast<symbol_kind_type>(t);
}

// basic_symbol.
template <typename Base>
ParserGen::basic_symbol<Base>::basic_symbol(const basic_symbol& that)
    : Base(that), value(), location(that.location) {
    switch (this->kind()) {
        case symbol_kind::S_BINARY:  // "BinData"
            value.copy<BSONBinData>(YY_MOVE(that.value));
            break;

        case symbol_kind::S_JAVASCRIPT:  // "Code"
            value.copy<BSONCode>(YY_MOVE(that.value));
            break;

        case symbol_kind::S_JAVASCRIPT_W_SCOPE:  // "CodeWScope"
            value.copy<BSONCodeWScope>(YY_MOVE(that.value));
            break;

        case symbol_kind::S_DB_POINTER:  // "dbPointer"
            value.copy<BSONDBRef>(YY_MOVE(that.value));
            break;

        case symbol_kind::S_REGEX:  // "regex"
            value.copy<BSONRegEx>(YY_MOVE(that.value));
            break;

        case symbol_kind::S_SYMBOL:  // "Symbol"
            value.copy<BSONSymbol>(YY_MOVE(that.value));
            break;

        case symbol_kind::S_dbPointer:                          // dbPointer
        case symbol_kind::S_javascript:                         // javascript
        case symbol_kind::S_symbol:                             // symbol
        case symbol_kind::S_javascriptWScope:                   // javascriptWScope
        case symbol_kind::S_int:                                // int
        case symbol_kind::S_timestamp:                          // timestamp
        case symbol_kind::S_long:                               // long
        case symbol_kind::S_double:                             // double
        case symbol_kind::S_decimal:                            // decimal
        case symbol_kind::S_minKey:                             // minKey
        case symbol_kind::S_maxKey:                             // maxKey
        case symbol_kind::S_value:                              // value
        case symbol_kind::S_string:                             // string
        case symbol_kind::S_aggregationFieldPath:               // aggregationFieldPath
        case symbol_kind::S_binary:                             // binary
        case symbol_kind::S_undefined:                          // undefined
        case symbol_kind::S_objectId:                           // objectId
        case symbol_kind::S_bool:                               // bool
        case symbol_kind::S_date:                               // date
        case symbol_kind::S_null:                               // null
        case symbol_kind::S_regex:                              // regex
        case symbol_kind::S_simpleValue:                        // simpleValue
        case symbol_kind::S_compoundValue:                      // compoundValue
        case symbol_kind::S_valueArray:                         // valueArray
        case symbol_kind::S_valueObject:                        // valueObject
        case symbol_kind::S_valueFields:                        // valueFields
        case symbol_kind::S_variable:                           // variable
        case symbol_kind::S_typeArray:                          // typeArray
        case symbol_kind::S_typeValue:                          // typeValue
        case symbol_kind::S_pipeline:                           // pipeline
        case symbol_kind::S_stageList:                          // stageList
        case symbol_kind::S_stage:                              // stage
        case symbol_kind::S_inhibitOptimization:                // inhibitOptimization
        case symbol_kind::S_unionWith:                          // unionWith
        case symbol_kind::S_skip:                               // skip
        case symbol_kind::S_limit:                              // limit
        case symbol_kind::S_matchStage:                         // matchStage
        case symbol_kind::S_project:                            // project
        case symbol_kind::S_sample:                             // sample
        case symbol_kind::S_aggregationProjectFields:           // aggregationProjectFields
        case symbol_kind::S_aggregationProjectionObjectFields:  // aggregationProjectionObjectFields
        case symbol_kind::S_topLevelAggregationProjection:      // topLevelAggregationProjection
        case symbol_kind::S_aggregationProjection:              // aggregationProjection
        case symbol_kind::S_projectionCommon:                   // projectionCommon
        case symbol_kind::S_aggregationProjectionObject:        // aggregationProjectionObject
        case symbol_kind::S_num:                                // num
        case symbol_kind::S_expression:                         // expression
        case symbol_kind::S_exprFixedTwoArg:                    // exprFixedTwoArg
        case symbol_kind::S_exprFixedThreeArg:                  // exprFixedThreeArg
        case symbol_kind::S_slice:                              // slice
        case symbol_kind::S_expressionArray:                    // expressionArray
        case symbol_kind::S_expressionObject:                   // expressionObject
        case symbol_kind::S_expressionFields:                   // expressionFields
        case symbol_kind::S_maths:                              // maths
        case symbol_kind::S_meta:                               // meta
        case symbol_kind::S_add:                                // add
        case symbol_kind::S_boolExprs:                          // boolExprs
        case symbol_kind::S_and:                                // and
        case symbol_kind::S_or:                                 // or
        case symbol_kind::S_not:                                // not
        case symbol_kind::S_literalEscapes:                     // literalEscapes
        case symbol_kind::S_const:                              // const
        case symbol_kind::S_literal:                            // literal
        case symbol_kind::S_stringExps:                         // stringExps
        case symbol_kind::S_concat:                             // concat
        case symbol_kind::S_dateFromString:                     // dateFromString
        case symbol_kind::S_dateToString:                       // dateToString
        case symbol_kind::S_indexOfBytes:                       // indexOfBytes
        case symbol_kind::S_indexOfCP:                          // indexOfCP
        case symbol_kind::S_ltrim:                              // ltrim
        case symbol_kind::S_regexFind:                          // regexFind
        case symbol_kind::S_regexFindAll:                       // regexFindAll
        case symbol_kind::S_regexMatch:                         // regexMatch
        case symbol_kind::S_regexArgs:                          // regexArgs
        case symbol_kind::S_replaceOne:                         // replaceOne
        case symbol_kind::S_replaceAll:                         // replaceAll
        case symbol_kind::S_rtrim:                              // rtrim
        case symbol_kind::S_split:                              // split
        case symbol_kind::S_strLenBytes:                        // strLenBytes
        case symbol_kind::S_strLenCP:                           // strLenCP
        case symbol_kind::S_strcasecmp:                         // strcasecmp
        case symbol_kind::S_substr:                             // substr
        case symbol_kind::S_substrBytes:                        // substrBytes
        case symbol_kind::S_substrCP:                           // substrCP
        case symbol_kind::S_toLower:                            // toLower
        case symbol_kind::S_toUpper:                            // toUpper
        case symbol_kind::S_trim:                               // trim
        case symbol_kind::S_compExprs:                          // compExprs
        case symbol_kind::S_cmp:                                // cmp
        case symbol_kind::S_eq:                                 // eq
        case symbol_kind::S_gt:                                 // gt
        case symbol_kind::S_gte:                                // gte
        case symbol_kind::S_lt:                                 // lt
        case symbol_kind::S_lte:                                // lte
        case symbol_kind::S_ne:                                 // ne
        case symbol_kind::S_dateExps:                           // dateExps
        case symbol_kind::S_dateFromParts:                      // dateFromParts
        case symbol_kind::S_dateToParts:                        // dateToParts
        case symbol_kind::S_dayOfMonth:                         // dayOfMonth
        case symbol_kind::S_dayOfWeek:                          // dayOfWeek
        case symbol_kind::S_dayOfYear:                          // dayOfYear
        case symbol_kind::S_hour:                               // hour
        case symbol_kind::S_isoDayOfWeek:                       // isoDayOfWeek
        case symbol_kind::S_isoWeek:                            // isoWeek
        case symbol_kind::S_isoWeekYear:                        // isoWeekYear
        case symbol_kind::S_millisecond:                        // millisecond
        case symbol_kind::S_minute:                             // minute
        case symbol_kind::S_month:                              // month
        case symbol_kind::S_second:                             // second
        case symbol_kind::S_week:                               // week
        case symbol_kind::S_year:                               // year
        case symbol_kind::S_typeExpression:                     // typeExpression
        case symbol_kind::S_convert:                            // convert
        case symbol_kind::S_toBool:                             // toBool
        case symbol_kind::S_toDate:                             // toDate
        case symbol_kind::S_toDecimal:                          // toDecimal
        case symbol_kind::S_toDouble:                           // toDouble
        case symbol_kind::S_toInt:                              // toInt
        case symbol_kind::S_toLong:                             // toLong
        case symbol_kind::S_toObjectId:                         // toObjectId
        case symbol_kind::S_toString:                           // toString
        case symbol_kind::S_type:                               // type
        case symbol_kind::S_abs:                                // abs
        case symbol_kind::S_ceil:                               // ceil
        case symbol_kind::S_divide:                             // divide
        case symbol_kind::S_exponent:                           // exponent
        case symbol_kind::S_floor:                              // floor
        case symbol_kind::S_ln:                                 // ln
        case symbol_kind::S_log:                                // log
        case symbol_kind::S_logten:                             // logten
        case symbol_kind::S_mod:                                // mod
        case symbol_kind::S_multiply:                           // multiply
        case symbol_kind::S_pow:                                // pow
        case symbol_kind::S_round:                              // round
        case symbol_kind::S_sqrt:                               // sqrt
        case symbol_kind::S_subtract:                           // subtract
        case symbol_kind::S_trunc:                              // trunc
        case symbol_kind::S_arrayExps:                          // arrayExps
        case symbol_kind::S_arrayElemAt:                        // arrayElemAt
        case symbol_kind::S_arrayToObject:                      // arrayToObject
        case symbol_kind::S_concatArrays:                       // concatArrays
        case symbol_kind::S_filter:                             // filter
        case symbol_kind::S_first:                              // first
        case symbol_kind::S_in:                                 // in
        case symbol_kind::S_indexOfArray:                       // indexOfArray
        case symbol_kind::S_isArray:                            // isArray
        case symbol_kind::S_setExpression:                      // setExpression
        case symbol_kind::S_allElementsTrue:                    // allElementsTrue
        case symbol_kind::S_anyElementTrue:                     // anyElementTrue
        case symbol_kind::S_setDifference:                      // setDifference
        case symbol_kind::S_setEquals:                          // setEquals
        case symbol_kind::S_setIntersection:                    // setIntersection
        case symbol_kind::S_setIsSubset:                        // setIsSubset
        case symbol_kind::S_setUnion:                           // setUnion
        case symbol_kind::S_trig:                               // trig
        case symbol_kind::S_sin:                                // sin
        case symbol_kind::S_cos:                                // cos
        case symbol_kind::S_tan:                                // tan
        case symbol_kind::S_sinh:                               // sinh
        case symbol_kind::S_cosh:                               // cosh
        case symbol_kind::S_tanh:                               // tanh
        case symbol_kind::S_asin:                               // asin
        case symbol_kind::S_acos:                               // acos
        case symbol_kind::S_atan:                               // atan
        case symbol_kind::S_asinh:                              // asinh
        case symbol_kind::S_acosh:                              // acosh
        case symbol_kind::S_atanh:                              // atanh
        case symbol_kind::S_atan2:                              // atan2
        case symbol_kind::S_degreesToRadians:                   // degreesToRadians
        case symbol_kind::S_radiansToDegrees:                   // radiansToDegrees
        case symbol_kind::S_nonArrayExpression:                 // nonArrayExpression
        case symbol_kind::S_nonArrayCompoundExpression:         // nonArrayCompoundExpression
        case symbol_kind::S_aggregationOperator:                // aggregationOperator
        case symbol_kind::S_aggregationOperatorWithoutSlice:    // aggregationOperatorWithoutSlice
        case symbol_kind::S_expressionSingletonArray:           // expressionSingletonArray
        case symbol_kind::S_singleArgExpression:                // singleArgExpression
        case symbol_kind::S_nonArrayNonObjExpression:           // nonArrayNonObjExpression
        case symbol_kind::S_matchExpression:                    // matchExpression
        case symbol_kind::S_predicates:                         // predicates
        case symbol_kind::S_compoundMatchExprs:                 // compoundMatchExprs
        case symbol_kind::S_predValue:                          // predValue
        case symbol_kind::S_additionalExprs:                    // additionalExprs
        case symbol_kind::S_textArgCaseSensitive:               // textArgCaseSensitive
        case symbol_kind::S_textArgDiacriticSensitive:          // textArgDiacriticSensitive
        case symbol_kind::S_textArgLanguage:                    // textArgLanguage
        case symbol_kind::S_textArgSearch:                      // textArgSearch
        case symbol_kind::S_findProject:                        // findProject
        case symbol_kind::S_findProjectFields:                  // findProjectFields
        case symbol_kind::S_topLevelFindProjection:             // topLevelFindProjection
        case symbol_kind::S_findProjection:                     // findProjection
        case symbol_kind::S_findProjectionSlice:                // findProjectionSlice
        case symbol_kind::S_elemMatch:                          // elemMatch
        case symbol_kind::S_findProjectionObject:               // findProjectionObject
        case symbol_kind::S_findProjectionObjectFields:         // findProjectionObjectFields
        case symbol_kind::S_sortSpecs:                          // sortSpecs
        case symbol_kind::S_specList:                           // specList
        case symbol_kind::S_metaSort:                           // metaSort
        case symbol_kind::S_oneOrNegOne:                        // oneOrNegOne
        case symbol_kind::S_metaSortKeyword:                    // metaSortKeyword
            value.copy<CNode>(YY_MOVE(that.value));
            break;

        case symbol_kind::S_aggregationProjectionFieldname:  // aggregationProjectionFieldname
        case symbol_kind::S_projectionFieldname:             // projectionFieldname
        case symbol_kind::S_expressionFieldname:             // expressionFieldname
        case symbol_kind::S_stageAsUserFieldname:            // stageAsUserFieldname
        case symbol_kind::S_argAsUserFieldname:              // argAsUserFieldname
        case symbol_kind::S_argAsProjectionPath:             // argAsProjectionPath
        case symbol_kind::S_aggExprAsUserFieldname:          // aggExprAsUserFieldname
        case symbol_kind::S_invariableUserFieldname:         // invariableUserFieldname
        case symbol_kind::S_sortFieldname:                   // sortFieldname
        case symbol_kind::S_idAsUserFieldname:               // idAsUserFieldname
        case symbol_kind::S_elemMatchAsUserFieldname:        // elemMatchAsUserFieldname
        case symbol_kind::S_idAsProjectionPath:              // idAsProjectionPath
        case symbol_kind::S_valueFieldname:                  // valueFieldname
        case symbol_kind::S_predFieldname:                   // predFieldname
        case symbol_kind::S_logicalExprField:                // logicalExprField
            value.copy<CNode::Fieldname>(YY_MOVE(that.value));
            break;

        case symbol_kind::S_DATE_LITERAL:  // "Date"
            value.copy<Date_t>(YY_MOVE(that.value));
            break;

        case symbol_kind::S_DECIMAL_OTHER:  // "arbitrary decimal"
            value.copy<Decimal128>(YY_MOVE(that.value));
            break;

        case symbol_kind::S_OBJECT_ID:  // "ObjectID"
            value.copy<OID>(YY_MOVE(that.value));
            break;

        case symbol_kind::S_TIMESTAMP:  // "Timestamp"
            value.copy<Timestamp>(YY_MOVE(that.value));
            break;

        case symbol_kind::S_MAX_KEY:  // "maxKey"
            value.copy<UserMaxKey>(YY_MOVE(that.value));
            break;

        case symbol_kind::S_MIN_KEY:  // "minKey"
            value.copy<UserMinKey>(YY_MOVE(that.value));
            break;

        case symbol_kind::S_JSNULL:  // "null"
            value.copy<UserNull>(YY_MOVE(that.value));
            break;

        case symbol_kind::S_UNDEFINED:  // "undefined"
            value.copy<UserUndefined>(YY_MOVE(that.value));
            break;

        case symbol_kind::S_DOUBLE_OTHER:  // "arbitrary double"
            value.copy<double>(YY_MOVE(that.value));
            break;

        case symbol_kind::S_INT_OTHER:  // "arbitrary integer"
            value.copy<int>(YY_MOVE(that.value));
            break;

        case symbol_kind::S_LONG_OTHER:  // "arbitrary long"
            value.copy<long long>(YY_MOVE(that.value));
            break;

        case symbol_kind::S_aggregationProjectField:           // aggregationProjectField
        case symbol_kind::S_aggregationProjectionObjectField:  // aggregationProjectionObjectField
        case symbol_kind::S_expressionField:                   // expressionField
        case symbol_kind::S_valueField:                        // valueField
        case symbol_kind::S_onErrorArg:                        // onErrorArg
        case symbol_kind::S_onNullArg:                         // onNullArg
        case symbol_kind::S_asArg:                             // asArg
        case symbol_kind::S_formatArg:                         // formatArg
        case symbol_kind::S_timezoneArg:                       // timezoneArg
        case symbol_kind::S_charsArg:                          // charsArg
        case symbol_kind::S_optionsArg:                        // optionsArg
        case symbol_kind::S_hourArg:                           // hourArg
        case symbol_kind::S_minuteArg:                         // minuteArg
        case symbol_kind::S_secondArg:                         // secondArg
        case symbol_kind::S_millisecondArg:                    // millisecondArg
        case symbol_kind::S_dayArg:                            // dayArg
        case symbol_kind::S_isoWeekArg:                        // isoWeekArg
        case symbol_kind::S_iso8601Arg:                        // iso8601Arg
        case symbol_kind::S_monthArg:                          // monthArg
        case symbol_kind::S_isoDayOfWeekArg:                   // isoDayOfWeekArg
        case symbol_kind::S_predicate:                         // predicate
        case symbol_kind::S_fieldPredicate:                    // fieldPredicate
        case symbol_kind::S_logicalExpr:                       // logicalExpr
        case symbol_kind::S_operatorExpression:                // operatorExpression
        case symbol_kind::S_notExpr:                           // notExpr
        case symbol_kind::S_matchMod:                          // matchMod
        case symbol_kind::S_existsExpr:                        // existsExpr
        case symbol_kind::S_typeExpr:                          // typeExpr
        case symbol_kind::S_commentExpr:                       // commentExpr
        case symbol_kind::S_matchExpr:                         // matchExpr
        case symbol_kind::S_matchText:                         // matchText
        case symbol_kind::S_matchWhere:                        // matchWhere
        case symbol_kind::S_findProjectField:                  // findProjectField
        case symbol_kind::S_findProjectionObjectField:         // findProjectionObjectField
        case symbol_kind::S_sortSpec:                          // sortSpec
            value.copy<std::pair<CNode::Fieldname, CNode>>(YY_MOVE(that.value));
            break;

        case symbol_kind::S_FIELDNAME:              // "fieldname"
        case symbol_kind::S_DOLLAR_PREF_FIELDNAME:  // "$-prefixed fieldname"
        case symbol_kind::S_STRING:                 // "string"
        case symbol_kind::S_DOLLAR_STRING:          // "$-prefixed string"
        case symbol_kind::S_DOLLAR_DOLLAR_STRING:   // "$$-prefixed string"
        case symbol_kind::S_arg:                    // arg
            value.copy<std::string>(YY_MOVE(that.value));
            break;

        case symbol_kind::S_expressions:    // expressions
        case symbol_kind::S_values:         // values
        case symbol_kind::S_exprZeroToTwo:  // exprZeroToTwo
        case symbol_kind::S_typeValues:     // typeValues
            value.copy<std::vector<CNode>>(YY_MOVE(that.value));
            break;

        case symbol_kind::S_DOTTED_FIELDNAME:  // "fieldname containing dotted path"
            value.copy<std::vector<std::string>>(YY_MOVE(that.value));
            break;

        default:
            break;
    }
}


template <typename Base>
ParserGen::symbol_kind_type ParserGen::basic_symbol<Base>::type_get() const YY_NOEXCEPT {
    return this->kind();
}

template <typename Base>
bool ParserGen::basic_symbol<Base>::empty() const YY_NOEXCEPT {
    return this->kind() == symbol_kind::S_YYEMPTY;
}

template <typename Base>
void ParserGen::basic_symbol<Base>::move(basic_symbol& s) {
    super_type::move(s);
    switch (this->kind()) {
        case symbol_kind::S_BINARY:  // "BinData"
            value.move<BSONBinData>(YY_MOVE(s.value));
            break;

        case symbol_kind::S_JAVASCRIPT:  // "Code"
            value.move<BSONCode>(YY_MOVE(s.value));
            break;

        case symbol_kind::S_JAVASCRIPT_W_SCOPE:  // "CodeWScope"
            value.move<BSONCodeWScope>(YY_MOVE(s.value));
            break;

        case symbol_kind::S_DB_POINTER:  // "dbPointer"
            value.move<BSONDBRef>(YY_MOVE(s.value));
            break;

        case symbol_kind::S_REGEX:  // "regex"
            value.move<BSONRegEx>(YY_MOVE(s.value));
            break;

        case symbol_kind::S_SYMBOL:  // "Symbol"
            value.move<BSONSymbol>(YY_MOVE(s.value));
            break;

        case symbol_kind::S_dbPointer:                          // dbPointer
        case symbol_kind::S_javascript:                         // javascript
        case symbol_kind::S_symbol:                             // symbol
        case symbol_kind::S_javascriptWScope:                   // javascriptWScope
        case symbol_kind::S_int:                                // int
        case symbol_kind::S_timestamp:                          // timestamp
        case symbol_kind::S_long:                               // long
        case symbol_kind::S_double:                             // double
        case symbol_kind::S_decimal:                            // decimal
        case symbol_kind::S_minKey:                             // minKey
        case symbol_kind::S_maxKey:                             // maxKey
        case symbol_kind::S_value:                              // value
        case symbol_kind::S_string:                             // string
        case symbol_kind::S_aggregationFieldPath:               // aggregationFieldPath
        case symbol_kind::S_binary:                             // binary
        case symbol_kind::S_undefined:                          // undefined
        case symbol_kind::S_objectId:                           // objectId
        case symbol_kind::S_bool:                               // bool
        case symbol_kind::S_date:                               // date
        case symbol_kind::S_null:                               // null
        case symbol_kind::S_regex:                              // regex
        case symbol_kind::S_simpleValue:                        // simpleValue
        case symbol_kind::S_compoundValue:                      // compoundValue
        case symbol_kind::S_valueArray:                         // valueArray
        case symbol_kind::S_valueObject:                        // valueObject
        case symbol_kind::S_valueFields:                        // valueFields
        case symbol_kind::S_variable:                           // variable
        case symbol_kind::S_typeArray:                          // typeArray
        case symbol_kind::S_typeValue:                          // typeValue
        case symbol_kind::S_pipeline:                           // pipeline
        case symbol_kind::S_stageList:                          // stageList
        case symbol_kind::S_stage:                              // stage
        case symbol_kind::S_inhibitOptimization:                // inhibitOptimization
        case symbol_kind::S_unionWith:                          // unionWith
        case symbol_kind::S_skip:                               // skip
        case symbol_kind::S_limit:                              // limit
        case symbol_kind::S_matchStage:                         // matchStage
        case symbol_kind::S_project:                            // project
        case symbol_kind::S_sample:                             // sample
        case symbol_kind::S_aggregationProjectFields:           // aggregationProjectFields
        case symbol_kind::S_aggregationProjectionObjectFields:  // aggregationProjectionObjectFields
        case symbol_kind::S_topLevelAggregationProjection:      // topLevelAggregationProjection
        case symbol_kind::S_aggregationProjection:              // aggregationProjection
        case symbol_kind::S_projectionCommon:                   // projectionCommon
        case symbol_kind::S_aggregationProjectionObject:        // aggregationProjectionObject
        case symbol_kind::S_num:                                // num
        case symbol_kind::S_expression:                         // expression
        case symbol_kind::S_exprFixedTwoArg:                    // exprFixedTwoArg
        case symbol_kind::S_exprFixedThreeArg:                  // exprFixedThreeArg
        case symbol_kind::S_slice:                              // slice
        case symbol_kind::S_expressionArray:                    // expressionArray
        case symbol_kind::S_expressionObject:                   // expressionObject
        case symbol_kind::S_expressionFields:                   // expressionFields
        case symbol_kind::S_maths:                              // maths
        case symbol_kind::S_meta:                               // meta
        case symbol_kind::S_add:                                // add
        case symbol_kind::S_boolExprs:                          // boolExprs
        case symbol_kind::S_and:                                // and
        case symbol_kind::S_or:                                 // or
        case symbol_kind::S_not:                                // not
        case symbol_kind::S_literalEscapes:                     // literalEscapes
        case symbol_kind::S_const:                              // const
        case symbol_kind::S_literal:                            // literal
        case symbol_kind::S_stringExps:                         // stringExps
        case symbol_kind::S_concat:                             // concat
        case symbol_kind::S_dateFromString:                     // dateFromString
        case symbol_kind::S_dateToString:                       // dateToString
        case symbol_kind::S_indexOfBytes:                       // indexOfBytes
        case symbol_kind::S_indexOfCP:                          // indexOfCP
        case symbol_kind::S_ltrim:                              // ltrim
        case symbol_kind::S_regexFind:                          // regexFind
        case symbol_kind::S_regexFindAll:                       // regexFindAll
        case symbol_kind::S_regexMatch:                         // regexMatch
        case symbol_kind::S_regexArgs:                          // regexArgs
        case symbol_kind::S_replaceOne:                         // replaceOne
        case symbol_kind::S_replaceAll:                         // replaceAll
        case symbol_kind::S_rtrim:                              // rtrim
        case symbol_kind::S_split:                              // split
        case symbol_kind::S_strLenBytes:                        // strLenBytes
        case symbol_kind::S_strLenCP:                           // strLenCP
        case symbol_kind::S_strcasecmp:                         // strcasecmp
        case symbol_kind::S_substr:                             // substr
        case symbol_kind::S_substrBytes:                        // substrBytes
        case symbol_kind::S_substrCP:                           // substrCP
        case symbol_kind::S_toLower:                            // toLower
        case symbol_kind::S_toUpper:                            // toUpper
        case symbol_kind::S_trim:                               // trim
        case symbol_kind::S_compExprs:                          // compExprs
        case symbol_kind::S_cmp:                                // cmp
        case symbol_kind::S_eq:                                 // eq
        case symbol_kind::S_gt:                                 // gt
        case symbol_kind::S_gte:                                // gte
        case symbol_kind::S_lt:                                 // lt
        case symbol_kind::S_lte:                                // lte
        case symbol_kind::S_ne:                                 // ne
        case symbol_kind::S_dateExps:                           // dateExps
        case symbol_kind::S_dateFromParts:                      // dateFromParts
        case symbol_kind::S_dateToParts:                        // dateToParts
        case symbol_kind::S_dayOfMonth:                         // dayOfMonth
        case symbol_kind::S_dayOfWeek:                          // dayOfWeek
        case symbol_kind::S_dayOfYear:                          // dayOfYear
        case symbol_kind::S_hour:                               // hour
        case symbol_kind::S_isoDayOfWeek:                       // isoDayOfWeek
        case symbol_kind::S_isoWeek:                            // isoWeek
        case symbol_kind::S_isoWeekYear:                        // isoWeekYear
        case symbol_kind::S_millisecond:                        // millisecond
        case symbol_kind::S_minute:                             // minute
        case symbol_kind::S_month:                              // month
        case symbol_kind::S_second:                             // second
        case symbol_kind::S_week:                               // week
        case symbol_kind::S_year:                               // year
        case symbol_kind::S_typeExpression:                     // typeExpression
        case symbol_kind::S_convert:                            // convert
        case symbol_kind::S_toBool:                             // toBool
        case symbol_kind::S_toDate:                             // toDate
        case symbol_kind::S_toDecimal:                          // toDecimal
        case symbol_kind::S_toDouble:                           // toDouble
        case symbol_kind::S_toInt:                              // toInt
        case symbol_kind::S_toLong:                             // toLong
        case symbol_kind::S_toObjectId:                         // toObjectId
        case symbol_kind::S_toString:                           // toString
        case symbol_kind::S_type:                               // type
        case symbol_kind::S_abs:                                // abs
        case symbol_kind::S_ceil:                               // ceil
        case symbol_kind::S_divide:                             // divide
        case symbol_kind::S_exponent:                           // exponent
        case symbol_kind::S_floor:                              // floor
        case symbol_kind::S_ln:                                 // ln
        case symbol_kind::S_log:                                // log
        case symbol_kind::S_logten:                             // logten
        case symbol_kind::S_mod:                                // mod
        case symbol_kind::S_multiply:                           // multiply
        case symbol_kind::S_pow:                                // pow
        case symbol_kind::S_round:                              // round
        case symbol_kind::S_sqrt:                               // sqrt
        case symbol_kind::S_subtract:                           // subtract
        case symbol_kind::S_trunc:                              // trunc
        case symbol_kind::S_arrayExps:                          // arrayExps
        case symbol_kind::S_arrayElemAt:                        // arrayElemAt
        case symbol_kind::S_arrayToObject:                      // arrayToObject
        case symbol_kind::S_concatArrays:                       // concatArrays
        case symbol_kind::S_filter:                             // filter
        case symbol_kind::S_first:                              // first
        case symbol_kind::S_in:                                 // in
        case symbol_kind::S_indexOfArray:                       // indexOfArray
        case symbol_kind::S_isArray:                            // isArray
        case symbol_kind::S_setExpression:                      // setExpression
        case symbol_kind::S_allElementsTrue:                    // allElementsTrue
        case symbol_kind::S_anyElementTrue:                     // anyElementTrue
        case symbol_kind::S_setDifference:                      // setDifference
        case symbol_kind::S_setEquals:                          // setEquals
        case symbol_kind::S_setIntersection:                    // setIntersection
        case symbol_kind::S_setIsSubset:                        // setIsSubset
        case symbol_kind::S_setUnion:                           // setUnion
        case symbol_kind::S_trig:                               // trig
        case symbol_kind::S_sin:                                // sin
        case symbol_kind::S_cos:                                // cos
        case symbol_kind::S_tan:                                // tan
        case symbol_kind::S_sinh:                               // sinh
        case symbol_kind::S_cosh:                               // cosh
        case symbol_kind::S_tanh:                               // tanh
        case symbol_kind::S_asin:                               // asin
        case symbol_kind::S_acos:                               // acos
        case symbol_kind::S_atan:                               // atan
        case symbol_kind::S_asinh:                              // asinh
        case symbol_kind::S_acosh:                              // acosh
        case symbol_kind::S_atanh:                              // atanh
        case symbol_kind::S_atan2:                              // atan2
        case symbol_kind::S_degreesToRadians:                   // degreesToRadians
        case symbol_kind::S_radiansToDegrees:                   // radiansToDegrees
        case symbol_kind::S_nonArrayExpression:                 // nonArrayExpression
        case symbol_kind::S_nonArrayCompoundExpression:         // nonArrayCompoundExpression
        case symbol_kind::S_aggregationOperator:                // aggregationOperator
        case symbol_kind::S_aggregationOperatorWithoutSlice:    // aggregationOperatorWithoutSlice
        case symbol_kind::S_expressionSingletonArray:           // expressionSingletonArray
        case symbol_kind::S_singleArgExpression:                // singleArgExpression
        case symbol_kind::S_nonArrayNonObjExpression:           // nonArrayNonObjExpression
        case symbol_kind::S_matchExpression:                    // matchExpression
        case symbol_kind::S_predicates:                         // predicates
        case symbol_kind::S_compoundMatchExprs:                 // compoundMatchExprs
        case symbol_kind::S_predValue:                          // predValue
        case symbol_kind::S_additionalExprs:                    // additionalExprs
        case symbol_kind::S_textArgCaseSensitive:               // textArgCaseSensitive
        case symbol_kind::S_textArgDiacriticSensitive:          // textArgDiacriticSensitive
        case symbol_kind::S_textArgLanguage:                    // textArgLanguage
        case symbol_kind::S_textArgSearch:                      // textArgSearch
        case symbol_kind::S_findProject:                        // findProject
        case symbol_kind::S_findProjectFields:                  // findProjectFields
        case symbol_kind::S_topLevelFindProjection:             // topLevelFindProjection
        case symbol_kind::S_findProjection:                     // findProjection
        case symbol_kind::S_findProjectionSlice:                // findProjectionSlice
        case symbol_kind::S_elemMatch:                          // elemMatch
        case symbol_kind::S_findProjectionObject:               // findProjectionObject
        case symbol_kind::S_findProjectionObjectFields:         // findProjectionObjectFields
        case symbol_kind::S_sortSpecs:                          // sortSpecs
        case symbol_kind::S_specList:                           // specList
        case symbol_kind::S_metaSort:                           // metaSort
        case symbol_kind::S_oneOrNegOne:                        // oneOrNegOne
        case symbol_kind::S_metaSortKeyword:                    // metaSortKeyword
            value.move<CNode>(YY_MOVE(s.value));
            break;

        case symbol_kind::S_aggregationProjectionFieldname:  // aggregationProjectionFieldname
        case symbol_kind::S_projectionFieldname:             // projectionFieldname
        case symbol_kind::S_expressionFieldname:             // expressionFieldname
        case symbol_kind::S_stageAsUserFieldname:            // stageAsUserFieldname
        case symbol_kind::S_argAsUserFieldname:              // argAsUserFieldname
        case symbol_kind::S_argAsProjectionPath:             // argAsProjectionPath
        case symbol_kind::S_aggExprAsUserFieldname:          // aggExprAsUserFieldname
        case symbol_kind::S_invariableUserFieldname:         // invariableUserFieldname
        case symbol_kind::S_sortFieldname:                   // sortFieldname
        case symbol_kind::S_idAsUserFieldname:               // idAsUserFieldname
        case symbol_kind::S_elemMatchAsUserFieldname:        // elemMatchAsUserFieldname
        case symbol_kind::S_idAsProjectionPath:              // idAsProjectionPath
        case symbol_kind::S_valueFieldname:                  // valueFieldname
        case symbol_kind::S_predFieldname:                   // predFieldname
        case symbol_kind::S_logicalExprField:                // logicalExprField
            value.move<CNode::Fieldname>(YY_MOVE(s.value));
            break;

        case symbol_kind::S_DATE_LITERAL:  // "Date"
            value.move<Date_t>(YY_MOVE(s.value));
            break;

        case symbol_kind::S_DECIMAL_OTHER:  // "arbitrary decimal"
            value.move<Decimal128>(YY_MOVE(s.value));
            break;

        case symbol_kind::S_OBJECT_ID:  // "ObjectID"
            value.move<OID>(YY_MOVE(s.value));
            break;

        case symbol_kind::S_TIMESTAMP:  // "Timestamp"
            value.move<Timestamp>(YY_MOVE(s.value));
            break;

        case symbol_kind::S_MAX_KEY:  // "maxKey"
            value.move<UserMaxKey>(YY_MOVE(s.value));
            break;

        case symbol_kind::S_MIN_KEY:  // "minKey"
            value.move<UserMinKey>(YY_MOVE(s.value));
            break;

        case symbol_kind::S_JSNULL:  // "null"
            value.move<UserNull>(YY_MOVE(s.value));
            break;

        case symbol_kind::S_UNDEFINED:  // "undefined"
            value.move<UserUndefined>(YY_MOVE(s.value));
            break;

        case symbol_kind::S_DOUBLE_OTHER:  // "arbitrary double"
            value.move<double>(YY_MOVE(s.value));
            break;

        case symbol_kind::S_INT_OTHER:  // "arbitrary integer"
            value.move<int>(YY_MOVE(s.value));
            break;

        case symbol_kind::S_LONG_OTHER:  // "arbitrary long"
            value.move<long long>(YY_MOVE(s.value));
            break;

        case symbol_kind::S_aggregationProjectField:           // aggregationProjectField
        case symbol_kind::S_aggregationProjectionObjectField:  // aggregationProjectionObjectField
        case symbol_kind::S_expressionField:                   // expressionField
        case symbol_kind::S_valueField:                        // valueField
        case symbol_kind::S_onErrorArg:                        // onErrorArg
        case symbol_kind::S_onNullArg:                         // onNullArg
        case symbol_kind::S_asArg:                             // asArg
        case symbol_kind::S_formatArg:                         // formatArg
        case symbol_kind::S_timezoneArg:                       // timezoneArg
        case symbol_kind::S_charsArg:                          // charsArg
        case symbol_kind::S_optionsArg:                        // optionsArg
        case symbol_kind::S_hourArg:                           // hourArg
        case symbol_kind::S_minuteArg:                         // minuteArg
        case symbol_kind::S_secondArg:                         // secondArg
        case symbol_kind::S_millisecondArg:                    // millisecondArg
        case symbol_kind::S_dayArg:                            // dayArg
        case symbol_kind::S_isoWeekArg:                        // isoWeekArg
        case symbol_kind::S_iso8601Arg:                        // iso8601Arg
        case symbol_kind::S_monthArg:                          // monthArg
        case symbol_kind::S_isoDayOfWeekArg:                   // isoDayOfWeekArg
        case symbol_kind::S_predicate:                         // predicate
        case symbol_kind::S_fieldPredicate:                    // fieldPredicate
        case symbol_kind::S_logicalExpr:                       // logicalExpr
        case symbol_kind::S_operatorExpression:                // operatorExpression
        case symbol_kind::S_notExpr:                           // notExpr
        case symbol_kind::S_matchMod:                          // matchMod
        case symbol_kind::S_existsExpr:                        // existsExpr
        case symbol_kind::S_typeExpr:                          // typeExpr
        case symbol_kind::S_commentExpr:                       // commentExpr
        case symbol_kind::S_matchExpr:                         // matchExpr
        case symbol_kind::S_matchText:                         // matchText
        case symbol_kind::S_matchWhere:                        // matchWhere
        case symbol_kind::S_findProjectField:                  // findProjectField
        case symbol_kind::S_findProjectionObjectField:         // findProjectionObjectField
        case symbol_kind::S_sortSpec:                          // sortSpec
            value.move<std::pair<CNode::Fieldname, CNode>>(YY_MOVE(s.value));
            break;

        case symbol_kind::S_FIELDNAME:              // "fieldname"
        case symbol_kind::S_DOLLAR_PREF_FIELDNAME:  // "$-prefixed fieldname"
        case symbol_kind::S_STRING:                 // "string"
        case symbol_kind::S_DOLLAR_STRING:          // "$-prefixed string"
        case symbol_kind::S_DOLLAR_DOLLAR_STRING:   // "$$-prefixed string"
        case symbol_kind::S_arg:                    // arg
            value.move<std::string>(YY_MOVE(s.value));
            break;

        case symbol_kind::S_expressions:    // expressions
        case symbol_kind::S_values:         // values
        case symbol_kind::S_exprZeroToTwo:  // exprZeroToTwo
        case symbol_kind::S_typeValues:     // typeValues
            value.move<std::vector<CNode>>(YY_MOVE(s.value));
            break;

        case symbol_kind::S_DOTTED_FIELDNAME:  // "fieldname containing dotted path"
            value.move<std::vector<std::string>>(YY_MOVE(s.value));
            break;

        default:
            break;
    }

    location = YY_MOVE(s.location);
}

// by_kind.
inline ParserGen::by_kind::by_kind() : kind_(symbol_kind::S_YYEMPTY) {}

#if 201103L <= YY_CPLUSPLUS
inline ParserGen::by_kind::by_kind(by_kind&& that) : kind_(that.kind_) {
    that.clear();
}
#endif

inline ParserGen::by_kind::by_kind(const by_kind& that) : kind_(that.kind_) {}

inline ParserGen::by_kind::by_kind(token_kind_type t) : kind_(yytranslate_(t)) {}

inline void ParserGen::by_kind::clear() {
    kind_ = symbol_kind::S_YYEMPTY;
}

inline void ParserGen::by_kind::move(by_kind& that) {
    kind_ = that.kind_;
    that.clear();
}

inline ParserGen::symbol_kind_type ParserGen::by_kind::kind() const YY_NOEXCEPT {
    return kind_;
}

inline ParserGen::symbol_kind_type ParserGen::by_kind::type_get() const YY_NOEXCEPT {
    return this->kind();
}

#line 57 "grammar.yy"
}  // namespace mongo
#line 7287 "parser_gen.hpp"


#endif  // !YY_YY_PARSER_GEN_HPP_INCLUDED
