// A Bison parser, made by GNU Bison 3.6.3.

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
            // pipeline
            // stageList
            // stage
            // inhibitOptimization
            // unionWith
            // skip
            // limit
            // project
            // sample
            // projectFields
            // projectionObjectFields
            // topLevelProjection
            // projection
            // projectionObject
            // num
            // expression
            // compoundNonObjectExpression
            // exprFixedTwoArg
            // exprFixedThreeArg
            // arrayManipulation
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
            // nonArrayNonObjCompoundExpression
            // expressionSingletonArray
            // singleArgExpression
            // match
            // predicates
            // compoundMatchExprs
            // predValue
            // additionalExprs
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
            // idAsUserFieldname
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

            // projectField
            // projectionObjectField
            // expressionField
            // valueField
            // onErrorArg
            // onNullArg
            // formatArg
            // timezoneArg
            // charsArg
            // optionsArg
            // predicate
            // logicalExpr
            // operatorExpression
            // notExpr
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
            ARG_CHARS = 10,                    // "chars argument"
            ARG_COLL = 11,                     // "coll argument"
            ARG_DATE = 12,                     // "date argument"
            ARG_DATE_STRING = 13,              // "dateString argument"
            ARG_FIND = 14,                     // "find argument"
            ARG_FORMAT = 15,                   // "format argument"
            ARG_INPUT = 16,                    // "input argument"
            ARG_ON_ERROR = 17,                 // "onError argument"
            ARG_ON_NULL = 18,                  // "onNull argument"
            ARG_OPTIONS = 19,                  // "options argument"
            ARG_PIPELINE = 20,                 // "pipeline argument"
            ARG_REGEX = 21,                    // "regex argument"
            ARG_REPLACEMENT = 22,              // "replacement argument"
            ARG_SIZE = 23,                     // "size argument"
            ARG_TIMEZONE = 24,                 // "timezone argument"
            ARG_TO = 25,                       // "to argument"
            ASIN = 26,                         // ASIN
            ASINH = 27,                        // ASINH
            ATAN = 28,                         // ATAN
            ATAN2 = 29,                        // ATAN2
            ATANH = 30,                        // ATANH
            BOOL_FALSE = 31,                   // "false"
            BOOL_TRUE = 32,                    // "true"
            CEIL = 33,                         // CEIL
            CMP = 34,                          // CMP
            CONCAT = 35,                       // CONCAT
            CONST_EXPR = 36,                   // CONST_EXPR
            CONVERT = 37,                      // CONVERT
            COS = 38,                          // COS
            COSH = 39,                         // COSH
            DATE_FROM_STRING = 40,             // DATE_FROM_STRING
            DATE_TO_STRING = 41,               // DATE_TO_STRING
            DECIMAL_NEGATIVE_ONE = 42,         // "-1 (decimal)"
            DECIMAL_ONE = 43,                  // "1 (decimal)"
            DECIMAL_ZERO = 44,                 // "zero (decimal)"
            DEGREES_TO_RADIANS = 45,           // DEGREES_TO_RADIANS
            DIVIDE = 46,                       // DIVIDE
            DOUBLE_NEGATIVE_ONE = 47,          // "-1 (double)"
            DOUBLE_ONE = 48,                   // "1 (double)"
            DOUBLE_ZERO = 49,                  // "zero (double)"
            END_ARRAY = 50,                    // "end of array"
            END_OBJECT = 51,                   // "end of object"
            EQ = 52,                           // EQ
            EXPONENT = 53,                     // EXPONENT
            FLOOR = 54,                        // FLOOR
            GEO_NEAR_DISTANCE = 55,            // "geoNearDistance"
            GEO_NEAR_POINT = 56,               // "geoNearPoint"
            GT = 57,                           // GT
            GTE = 58,                          // GTE
            ID = 59,                           // ID
            INDEX_OF_BYTES = 60,               // INDEX_OF_BYTES
            INDEX_OF_CP = 61,                  // INDEX_OF_CP
            INDEX_KEY = 62,                    // "indexKey"
            INT_NEGATIVE_ONE = 63,             // "-1 (int)"
            INT_ONE = 64,                      // "1 (int)"
            INT_ZERO = 65,                     // "zero (int)"
            LITERAL = 66,                      // LITERAL
            LN = 67,                           // LN
            LOG = 68,                          // LOG
            LOGTEN = 69,                       // LOGTEN
            LONG_NEGATIVE_ONE = 70,            // "-1 (long)"
            LONG_ONE = 71,                     // "1 (long)"
            LONG_ZERO = 72,                    // "zero (long)"
            LT = 73,                           // LT
            LTE = 74,                          // LTE
            LTRIM = 75,                        // LTRIM
            META = 76,                         // META
            MOD = 77,                          // MOD
            MULTIPLY = 78,                     // MULTIPLY
            NE = 79,                           // NE
            NOR = 80,                          // NOR
            NOT = 81,                          // NOT
            OR = 82,                           // OR
            POW = 83,                          // POW
            RADIANS_TO_DEGREES = 84,           // RADIANS_TO_DEGREES
            RAND_VAL = 85,                     // "randVal"
            RECORD_ID = 86,                    // "recordId"
            REGEX_FIND = 87,                   // REGEX_FIND
            REGEX_FIND_ALL = 88,               // REGEX_FIND_ALL
            REGEX_MATCH = 89,                  // REGEX_MATCH
            REPLACE_ALL = 90,                  // REPLACE_ALL
            REPLACE_ONE = 91,                  // REPLACE_ONE
            ROUND = 92,                        // ROUND
            RTRIM = 93,                        // RTRIM
            SEARCH_HIGHLIGHTS = 94,            // "searchHighlights"
            SEARCH_SCORE = 95,                 // "searchScore"
            SET_DIFFERENCE = 96,               // "setDifference"
            SET_EQUALS = 97,                   // "setEquals"
            SET_INTERSECTION = 98,             // "setIntersection"
            SET_IS_SUBSET = 99,                // "setIsSubset"
            SET_UNION = 100,                   // "setUnion"
            SLICE = 101,                       // "slice"
            SORT_KEY = 102,                    // "sortKey"
            SIN = 103,                         // SIN
            SINH = 104,                        // SINH
            SPLIT = 105,                       // SPLIT
            SQRT = 106,                        // SQRT
            STAGE_INHIBIT_OPTIMIZATION = 107,  // STAGE_INHIBIT_OPTIMIZATION
            STAGE_LIMIT = 108,                 // STAGE_LIMIT
            STAGE_PROJECT = 109,               // STAGE_PROJECT
            STAGE_SAMPLE = 110,                // STAGE_SAMPLE
            STAGE_SKIP = 111,                  // STAGE_SKIP
            STAGE_UNION_WITH = 112,            // STAGE_UNION_WITH
            START_ARRAY = 113,                 // "array"
            START_OBJECT = 114,                // "object"
            STR_CASE_CMP = 115,                // STR_CASE_CMP
            STR_LEN_BYTES = 116,               // STR_LEN_BYTES
            STR_LEN_CP = 117,                  // STR_LEN_CP
            SUBSTR = 118,                      // SUBSTR
            SUBSTR_BYTES = 119,                // SUBSTR_BYTES
            SUBSTR_CP = 120,                   // SUBSTR_CP
            SUBTRACT = 121,                    // SUBTRACT
            TAN = 122,                         // TAN
            TANH = 123,                        // TANH
            TEXT_SCORE = 124,                  // "textScore"
            TO_BOOL = 125,                     // TO_BOOL
            TO_DATE = 126,                     // TO_DATE
            TO_DECIMAL = 127,                  // TO_DECIMAL
            TO_DOUBLE = 128,                   // TO_DOUBLE
            TO_INT = 129,                      // TO_INT
            TO_LONG = 130,                     // TO_LONG
            TO_LOWER = 131,                    // TO_LOWER
            TO_OBJECT_ID = 132,                // TO_OBJECT_ID
            TO_STRING = 133,                   // TO_STRING
            TO_UPPER = 134,                    // TO_UPPER
            TRIM = 135,                        // TRIM
            TRUNC = 136,                       // TRUNC
            TYPE = 137,                        // TYPE
            FIELDNAME = 138,                   // "fieldname"
            DOTTED_FIELDNAME = 139,            // "fieldname containing dotted path"
            DOLLAR_PREF_FIELDNAME = 140,       // "$-prefixed fieldname"
            STRING = 141,                      // "string"
            DOLLAR_STRING = 142,               // "$-prefixed string"
            DOLLAR_DOLLAR_STRING = 143,        // "$$-prefixed string"
            BINARY = 144,                      // "BinData"
            UNDEFINED = 145,                   // "undefined"
            OBJECT_ID = 146,                   // "ObjectID"
            DATE_LITERAL = 147,                // "Date"
            JSNULL = 148,                      // "null"
            REGEX = 149,                       // "regex"
            DB_POINTER = 150,                  // "dbPointer"
            JAVASCRIPT = 151,                  // "Code"
            SYMBOL = 152,                      // "Symbol"
            JAVASCRIPT_W_SCOPE = 153,          // "CodeWScope"
            INT_OTHER = 154,                   // "arbitrary integer"
            LONG_OTHER = 155,                  // "arbitrary long"
            DOUBLE_OTHER = 156,                // "arbitrary double"
            DECIMAL_OTHER = 157,               // "arbitrary decimal"
            TIMESTAMP = 158,                   // "Timestamp"
            MIN_KEY = 159,                     // "minKey"
            MAX_KEY = 160,                     // "maxKey"
            START_PIPELINE = 161,              // START_PIPELINE
            START_MATCH = 162,                 // START_MATCH
            START_SORT = 163                   // START_SORT
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
            YYNTOKENS = 164,  ///< Number of tokens.
            S_YYEMPTY = -2,
            S_YYEOF = 0,                               // "EOF"
            S_YYerror = 1,                             // error
            S_YYUNDEF = 2,                             // "invalid token"
            S_ABS = 3,                                 // ABS
            S_ACOS = 4,                                // ACOS
            S_ACOSH = 5,                               // ACOSH
            S_ADD = 6,                                 // ADD
            S_ALL_ELEMENTS_TRUE = 7,                   // "allElementsTrue"
            S_AND = 8,                                 // AND
            S_ANY_ELEMENT_TRUE = 9,                    // "anyElementTrue"
            S_ARG_CHARS = 10,                          // "chars argument"
            S_ARG_COLL = 11,                           // "coll argument"
            S_ARG_DATE = 12,                           // "date argument"
            S_ARG_DATE_STRING = 13,                    // "dateString argument"
            S_ARG_FIND = 14,                           // "find argument"
            S_ARG_FORMAT = 15,                         // "format argument"
            S_ARG_INPUT = 16,                          // "input argument"
            S_ARG_ON_ERROR = 17,                       // "onError argument"
            S_ARG_ON_NULL = 18,                        // "onNull argument"
            S_ARG_OPTIONS = 19,                        // "options argument"
            S_ARG_PIPELINE = 20,                       // "pipeline argument"
            S_ARG_REGEX = 21,                          // "regex argument"
            S_ARG_REPLACEMENT = 22,                    // "replacement argument"
            S_ARG_SIZE = 23,                           // "size argument"
            S_ARG_TIMEZONE = 24,                       // "timezone argument"
            S_ARG_TO = 25,                             // "to argument"
            S_ASIN = 26,                               // ASIN
            S_ASINH = 27,                              // ASINH
            S_ATAN = 28,                               // ATAN
            S_ATAN2 = 29,                              // ATAN2
            S_ATANH = 30,                              // ATANH
            S_BOOL_FALSE = 31,                         // "false"
            S_BOOL_TRUE = 32,                          // "true"
            S_CEIL = 33,                               // CEIL
            S_CMP = 34,                                // CMP
            S_CONCAT = 35,                             // CONCAT
            S_CONST_EXPR = 36,                         // CONST_EXPR
            S_CONVERT = 37,                            // CONVERT
            S_COS = 38,                                // COS
            S_COSH = 39,                               // COSH
            S_DATE_FROM_STRING = 40,                   // DATE_FROM_STRING
            S_DATE_TO_STRING = 41,                     // DATE_TO_STRING
            S_DECIMAL_NEGATIVE_ONE = 42,               // "-1 (decimal)"
            S_DECIMAL_ONE = 43,                        // "1 (decimal)"
            S_DECIMAL_ZERO = 44,                       // "zero (decimal)"
            S_DEGREES_TO_RADIANS = 45,                 // DEGREES_TO_RADIANS
            S_DIVIDE = 46,                             // DIVIDE
            S_DOUBLE_NEGATIVE_ONE = 47,                // "-1 (double)"
            S_DOUBLE_ONE = 48,                         // "1 (double)"
            S_DOUBLE_ZERO = 49,                        // "zero (double)"
            S_END_ARRAY = 50,                          // "end of array"
            S_END_OBJECT = 51,                         // "end of object"
            S_EQ = 52,                                 // EQ
            S_EXPONENT = 53,                           // EXPONENT
            S_FLOOR = 54,                              // FLOOR
            S_GEO_NEAR_DISTANCE = 55,                  // "geoNearDistance"
            S_GEO_NEAR_POINT = 56,                     // "geoNearPoint"
            S_GT = 57,                                 // GT
            S_GTE = 58,                                // GTE
            S_ID = 59,                                 // ID
            S_INDEX_OF_BYTES = 60,                     // INDEX_OF_BYTES
            S_INDEX_OF_CP = 61,                        // INDEX_OF_CP
            S_INDEX_KEY = 62,                          // "indexKey"
            S_INT_NEGATIVE_ONE = 63,                   // "-1 (int)"
            S_INT_ONE = 64,                            // "1 (int)"
            S_INT_ZERO = 65,                           // "zero (int)"
            S_LITERAL = 66,                            // LITERAL
            S_LN = 67,                                 // LN
            S_LOG = 68,                                // LOG
            S_LOGTEN = 69,                             // LOGTEN
            S_LONG_NEGATIVE_ONE = 70,                  // "-1 (long)"
            S_LONG_ONE = 71,                           // "1 (long)"
            S_LONG_ZERO = 72,                          // "zero (long)"
            S_LT = 73,                                 // LT
            S_LTE = 74,                                // LTE
            S_LTRIM = 75,                              // LTRIM
            S_META = 76,                               // META
            S_MOD = 77,                                // MOD
            S_MULTIPLY = 78,                           // MULTIPLY
            S_NE = 79,                                 // NE
            S_NOR = 80,                                // NOR
            S_NOT = 81,                                // NOT
            S_OR = 82,                                 // OR
            S_POW = 83,                                // POW
            S_RADIANS_TO_DEGREES = 84,                 // RADIANS_TO_DEGREES
            S_RAND_VAL = 85,                           // "randVal"
            S_RECORD_ID = 86,                          // "recordId"
            S_REGEX_FIND = 87,                         // REGEX_FIND
            S_REGEX_FIND_ALL = 88,                     // REGEX_FIND_ALL
            S_REGEX_MATCH = 89,                        // REGEX_MATCH
            S_REPLACE_ALL = 90,                        // REPLACE_ALL
            S_REPLACE_ONE = 91,                        // REPLACE_ONE
            S_ROUND = 92,                              // ROUND
            S_RTRIM = 93,                              // RTRIM
            S_SEARCH_HIGHLIGHTS = 94,                  // "searchHighlights"
            S_SEARCH_SCORE = 95,                       // "searchScore"
            S_SET_DIFFERENCE = 96,                     // "setDifference"
            S_SET_EQUALS = 97,                         // "setEquals"
            S_SET_INTERSECTION = 98,                   // "setIntersection"
            S_SET_IS_SUBSET = 99,                      // "setIsSubset"
            S_SET_UNION = 100,                         // "setUnion"
            S_SLICE = 101,                             // "slice"
            S_SORT_KEY = 102,                          // "sortKey"
            S_SIN = 103,                               // SIN
            S_SINH = 104,                              // SINH
            S_SPLIT = 105,                             // SPLIT
            S_SQRT = 106,                              // SQRT
            S_STAGE_INHIBIT_OPTIMIZATION = 107,        // STAGE_INHIBIT_OPTIMIZATION
            S_STAGE_LIMIT = 108,                       // STAGE_LIMIT
            S_STAGE_PROJECT = 109,                     // STAGE_PROJECT
            S_STAGE_SAMPLE = 110,                      // STAGE_SAMPLE
            S_STAGE_SKIP = 111,                        // STAGE_SKIP
            S_STAGE_UNION_WITH = 112,                  // STAGE_UNION_WITH
            S_START_ARRAY = 113,                       // "array"
            S_START_OBJECT = 114,                      // "object"
            S_STR_CASE_CMP = 115,                      // STR_CASE_CMP
            S_STR_LEN_BYTES = 116,                     // STR_LEN_BYTES
            S_STR_LEN_CP = 117,                        // STR_LEN_CP
            S_SUBSTR = 118,                            // SUBSTR
            S_SUBSTR_BYTES = 119,                      // SUBSTR_BYTES
            S_SUBSTR_CP = 120,                         // SUBSTR_CP
            S_SUBTRACT = 121,                          // SUBTRACT
            S_TAN = 122,                               // TAN
            S_TANH = 123,                              // TANH
            S_TEXT_SCORE = 124,                        // "textScore"
            S_TO_BOOL = 125,                           // TO_BOOL
            S_TO_DATE = 126,                           // TO_DATE
            S_TO_DECIMAL = 127,                        // TO_DECIMAL
            S_TO_DOUBLE = 128,                         // TO_DOUBLE
            S_TO_INT = 129,                            // TO_INT
            S_TO_LONG = 130,                           // TO_LONG
            S_TO_LOWER = 131,                          // TO_LOWER
            S_TO_OBJECT_ID = 132,                      // TO_OBJECT_ID
            S_TO_STRING = 133,                         // TO_STRING
            S_TO_UPPER = 134,                          // TO_UPPER
            S_TRIM = 135,                              // TRIM
            S_TRUNC = 136,                             // TRUNC
            S_TYPE = 137,                              // TYPE
            S_FIELDNAME = 138,                         // "fieldname"
            S_DOTTED_FIELDNAME = 139,                  // "fieldname containing dotted path"
            S_DOLLAR_PREF_FIELDNAME = 140,             // "$-prefixed fieldname"
            S_STRING = 141,                            // "string"
            S_DOLLAR_STRING = 142,                     // "$-prefixed string"
            S_DOLLAR_DOLLAR_STRING = 143,              // "$$-prefixed string"
            S_BINARY = 144,                            // "BinData"
            S_UNDEFINED = 145,                         // "undefined"
            S_OBJECT_ID = 146,                         // "ObjectID"
            S_DATE_LITERAL = 147,                      // "Date"
            S_JSNULL = 148,                            // "null"
            S_REGEX = 149,                             // "regex"
            S_DB_POINTER = 150,                        // "dbPointer"
            S_JAVASCRIPT = 151,                        // "Code"
            S_SYMBOL = 152,                            // "Symbol"
            S_JAVASCRIPT_W_SCOPE = 153,                // "CodeWScope"
            S_INT_OTHER = 154,                         // "arbitrary integer"
            S_LONG_OTHER = 155,                        // "arbitrary long"
            S_DOUBLE_OTHER = 156,                      // "arbitrary double"
            S_DECIMAL_OTHER = 157,                     // "arbitrary decimal"
            S_TIMESTAMP = 158,                         // "Timestamp"
            S_MIN_KEY = 159,                           // "minKey"
            S_MAX_KEY = 160,                           // "maxKey"
            S_START_PIPELINE = 161,                    // START_PIPELINE
            S_START_MATCH = 162,                       // START_MATCH
            S_START_SORT = 163,                        // START_SORT
            S_YYACCEPT = 164,                          // $accept
            S_aggregationProjectionFieldname = 165,    // aggregationProjectionFieldname
            S_projectionFieldname = 166,               // projectionFieldname
            S_expressionFieldname = 167,               // expressionFieldname
            S_stageAsUserFieldname = 168,              // stageAsUserFieldname
            S_argAsUserFieldname = 169,                // argAsUserFieldname
            S_argAsProjectionPath = 170,               // argAsProjectionPath
            S_aggExprAsUserFieldname = 171,            // aggExprAsUserFieldname
            S_invariableUserFieldname = 172,           // invariableUserFieldname
            S_idAsUserFieldname = 173,                 // idAsUserFieldname
            S_idAsProjectionPath = 174,                // idAsProjectionPath
            S_valueFieldname = 175,                    // valueFieldname
            S_predFieldname = 176,                     // predFieldname
            S_projectField = 177,                      // projectField
            S_projectionObjectField = 178,             // projectionObjectField
            S_expressionField = 179,                   // expressionField
            S_valueField = 180,                        // valueField
            S_arg = 181,                               // arg
            S_dbPointer = 182,                         // dbPointer
            S_javascript = 183,                        // javascript
            S_symbol = 184,                            // symbol
            S_javascriptWScope = 185,                  // javascriptWScope
            S_int = 186,                               // int
            S_timestamp = 187,                         // timestamp
            S_long = 188,                              // long
            S_double = 189,                            // double
            S_decimal = 190,                           // decimal
            S_minKey = 191,                            // minKey
            S_maxKey = 192,                            // maxKey
            S_value = 193,                             // value
            S_string = 194,                            // string
            S_aggregationFieldPath = 195,              // aggregationFieldPath
            S_binary = 196,                            // binary
            S_undefined = 197,                         // undefined
            S_objectId = 198,                          // objectId
            S_bool = 199,                              // bool
            S_date = 200,                              // date
            S_null = 201,                              // null
            S_regex = 202,                             // regex
            S_simpleValue = 203,                       // simpleValue
            S_compoundValue = 204,                     // compoundValue
            S_valueArray = 205,                        // valueArray
            S_valueObject = 206,                       // valueObject
            S_valueFields = 207,                       // valueFields
            S_variable = 208,                          // variable
            S_pipeline = 209,                          // pipeline
            S_stageList = 210,                         // stageList
            S_stage = 211,                             // stage
            S_inhibitOptimization = 212,               // inhibitOptimization
            S_unionWith = 213,                         // unionWith
            S_skip = 214,                              // skip
            S_limit = 215,                             // limit
            S_project = 216,                           // project
            S_sample = 217,                            // sample
            S_projectFields = 218,                     // projectFields
            S_projectionObjectFields = 219,            // projectionObjectFields
            S_topLevelProjection = 220,                // topLevelProjection
            S_projection = 221,                        // projection
            S_projectionObject = 222,                  // projectionObject
            S_num = 223,                               // num
            S_expression = 224,                        // expression
            S_compoundNonObjectExpression = 225,       // compoundNonObjectExpression
            S_exprFixedTwoArg = 226,                   // exprFixedTwoArg
            S_exprFixedThreeArg = 227,                 // exprFixedThreeArg
            S_arrayManipulation = 228,                 // arrayManipulation
            S_slice = 229,                             // slice
            S_expressionArray = 230,                   // expressionArray
            S_expressionObject = 231,                  // expressionObject
            S_expressionFields = 232,                  // expressionFields
            S_maths = 233,                             // maths
            S_meta = 234,                              // meta
            S_add = 235,                               // add
            S_boolExprs = 236,                         // boolExprs
            S_and = 237,                               // and
            S_or = 238,                                // or
            S_not = 239,                               // not
            S_literalEscapes = 240,                    // literalEscapes
            S_const = 241,                             // const
            S_literal = 242,                           // literal
            S_stringExps = 243,                        // stringExps
            S_concat = 244,                            // concat
            S_dateFromString = 245,                    // dateFromString
            S_dateToString = 246,                      // dateToString
            S_indexOfBytes = 247,                      // indexOfBytes
            S_indexOfCP = 248,                         // indexOfCP
            S_ltrim = 249,                             // ltrim
            S_regexFind = 250,                         // regexFind
            S_regexFindAll = 251,                      // regexFindAll
            S_regexMatch = 252,                        // regexMatch
            S_regexArgs = 253,                         // regexArgs
            S_replaceOne = 254,                        // replaceOne
            S_replaceAll = 255,                        // replaceAll
            S_rtrim = 256,                             // rtrim
            S_split = 257,                             // split
            S_strLenBytes = 258,                       // strLenBytes
            S_strLenCP = 259,                          // strLenCP
            S_strcasecmp = 260,                        // strcasecmp
            S_substr = 261,                            // substr
            S_substrBytes = 262,                       // substrBytes
            S_substrCP = 263,                          // substrCP
            S_toLower = 264,                           // toLower
            S_toUpper = 265,                           // toUpper
            S_trim = 266,                              // trim
            S_compExprs = 267,                         // compExprs
            S_cmp = 268,                               // cmp
            S_eq = 269,                                // eq
            S_gt = 270,                                // gt
            S_gte = 271,                               // gte
            S_lt = 272,                                // lt
            S_lte = 273,                               // lte
            S_ne = 274,                                // ne
            S_typeExpression = 275,                    // typeExpression
            S_convert = 276,                           // convert
            S_toBool = 277,                            // toBool
            S_toDate = 278,                            // toDate
            S_toDecimal = 279,                         // toDecimal
            S_toDouble = 280,                          // toDouble
            S_toInt = 281,                             // toInt
            S_toLong = 282,                            // toLong
            S_toObjectId = 283,                        // toObjectId
            S_toString = 284,                          // toString
            S_type = 285,                              // type
            S_abs = 286,                               // abs
            S_ceil = 287,                              // ceil
            S_divide = 288,                            // divide
            S_exponent = 289,                          // exponent
            S_floor = 290,                             // floor
            S_ln = 291,                                // ln
            S_log = 292,                               // log
            S_logten = 293,                            // logten
            S_mod = 294,                               // mod
            S_multiply = 295,                          // multiply
            S_pow = 296,                               // pow
            S_round = 297,                             // round
            S_sqrt = 298,                              // sqrt
            S_subtract = 299,                          // subtract
            S_trunc = 300,                             // trunc
            S_onErrorArg = 301,                        // onErrorArg
            S_onNullArg = 302,                         // onNullArg
            S_formatArg = 303,                         // formatArg
            S_timezoneArg = 304,                       // timezoneArg
            S_charsArg = 305,                          // charsArg
            S_optionsArg = 306,                        // optionsArg
            S_expressions = 307,                       // expressions
            S_values = 308,                            // values
            S_exprZeroToTwo = 309,                     // exprZeroToTwo
            S_setExpression = 310,                     // setExpression
            S_allElementsTrue = 311,                   // allElementsTrue
            S_anyElementTrue = 312,                    // anyElementTrue
            S_setDifference = 313,                     // setDifference
            S_setEquals = 314,                         // setEquals
            S_setIntersection = 315,                   // setIntersection
            S_setIsSubset = 316,                       // setIsSubset
            S_setUnion = 317,                          // setUnion
            S_trig = 318,                              // trig
            S_sin = 319,                               // sin
            S_cos = 320,                               // cos
            S_tan = 321,                               // tan
            S_sinh = 322,                              // sinh
            S_cosh = 323,                              // cosh
            S_tanh = 324,                              // tanh
            S_asin = 325,                              // asin
            S_acos = 326,                              // acos
            S_atan = 327,                              // atan
            S_asinh = 328,                             // asinh
            S_acosh = 329,                             // acosh
            S_atanh = 330,                             // atanh
            S_atan2 = 331,                             // atan2
            S_degreesToRadians = 332,                  // degreesToRadians
            S_radiansToDegrees = 333,                  // radiansToDegrees
            S_nonArrayExpression = 334,                // nonArrayExpression
            S_nonArrayCompoundExpression = 335,        // nonArrayCompoundExpression
            S_nonArrayNonObjCompoundExpression = 336,  // nonArrayNonObjCompoundExpression
            S_expressionSingletonArray = 337,          // expressionSingletonArray
            S_singleArgExpression = 338,               // singleArgExpression
            S_match = 339,                             // match
            S_predicates = 340,                        // predicates
            S_compoundMatchExprs = 341,                // compoundMatchExprs
            S_predValue = 342,                         // predValue
            S_additionalExprs = 343,                   // additionalExprs
            S_predicate = 344,                         // predicate
            S_logicalExpr = 345,                       // logicalExpr
            S_operatorExpression = 346,                // operatorExpression
            S_notExpr = 347,                           // notExpr
            S_logicalExprField = 348,                  // logicalExprField
            S_sortSpecs = 349,                         // sortSpecs
            S_specList = 350,                          // specList
            S_metaSort = 351,                          // metaSort
            S_oneOrNegOne = 352,                       // oneOrNegOne
            S_metaSortKeyword = 353,                   // metaSortKeyword
            S_sortSpec = 354,                          // sortSpec
            S_start = 355,                             // start
            S_START_ORDERED_OBJECT = 356,              // START_ORDERED_OBJECT
            S_357_1 = 357                              // $@1
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
                case 144:  // "BinData"
                    value.move<BSONBinData>(std::move(that.value));
                    break;

                case 151:  // "Code"
                    value.move<BSONCode>(std::move(that.value));
                    break;

                case 153:  // "CodeWScope"
                    value.move<BSONCodeWScope>(std::move(that.value));
                    break;

                case 150:  // "dbPointer"
                    value.move<BSONDBRef>(std::move(that.value));
                    break;

                case 149:  // "regex"
                    value.move<BSONRegEx>(std::move(that.value));
                    break;

                case 152:  // "Symbol"
                    value.move<BSONSymbol>(std::move(that.value));
                    break;

                case 182:  // dbPointer
                case 183:  // javascript
                case 184:  // symbol
                case 185:  // javascriptWScope
                case 186:  // int
                case 187:  // timestamp
                case 188:  // long
                case 189:  // double
                case 190:  // decimal
                case 191:  // minKey
                case 192:  // maxKey
                case 193:  // value
                case 194:  // string
                case 195:  // aggregationFieldPath
                case 196:  // binary
                case 197:  // undefined
                case 198:  // objectId
                case 199:  // bool
                case 200:  // date
                case 201:  // null
                case 202:  // regex
                case 203:  // simpleValue
                case 204:  // compoundValue
                case 205:  // valueArray
                case 206:  // valueObject
                case 207:  // valueFields
                case 208:  // variable
                case 209:  // pipeline
                case 210:  // stageList
                case 211:  // stage
                case 212:  // inhibitOptimization
                case 213:  // unionWith
                case 214:  // skip
                case 215:  // limit
                case 216:  // project
                case 217:  // sample
                case 218:  // projectFields
                case 219:  // projectionObjectFields
                case 220:  // topLevelProjection
                case 221:  // projection
                case 222:  // projectionObject
                case 223:  // num
                case 224:  // expression
                case 225:  // compoundNonObjectExpression
                case 226:  // exprFixedTwoArg
                case 227:  // exprFixedThreeArg
                case 228:  // arrayManipulation
                case 229:  // slice
                case 230:  // expressionArray
                case 231:  // expressionObject
                case 232:  // expressionFields
                case 233:  // maths
                case 234:  // meta
                case 235:  // add
                case 236:  // boolExprs
                case 237:  // and
                case 238:  // or
                case 239:  // not
                case 240:  // literalEscapes
                case 241:  // const
                case 242:  // literal
                case 243:  // stringExps
                case 244:  // concat
                case 245:  // dateFromString
                case 246:  // dateToString
                case 247:  // indexOfBytes
                case 248:  // indexOfCP
                case 249:  // ltrim
                case 250:  // regexFind
                case 251:  // regexFindAll
                case 252:  // regexMatch
                case 253:  // regexArgs
                case 254:  // replaceOne
                case 255:  // replaceAll
                case 256:  // rtrim
                case 257:  // split
                case 258:  // strLenBytes
                case 259:  // strLenCP
                case 260:  // strcasecmp
                case 261:  // substr
                case 262:  // substrBytes
                case 263:  // substrCP
                case 264:  // toLower
                case 265:  // toUpper
                case 266:  // trim
                case 267:  // compExprs
                case 268:  // cmp
                case 269:  // eq
                case 270:  // gt
                case 271:  // gte
                case 272:  // lt
                case 273:  // lte
                case 274:  // ne
                case 275:  // typeExpression
                case 276:  // convert
                case 277:  // toBool
                case 278:  // toDate
                case 279:  // toDecimal
                case 280:  // toDouble
                case 281:  // toInt
                case 282:  // toLong
                case 283:  // toObjectId
                case 284:  // toString
                case 285:  // type
                case 286:  // abs
                case 287:  // ceil
                case 288:  // divide
                case 289:  // exponent
                case 290:  // floor
                case 291:  // ln
                case 292:  // log
                case 293:  // logten
                case 294:  // mod
                case 295:  // multiply
                case 296:  // pow
                case 297:  // round
                case 298:  // sqrt
                case 299:  // subtract
                case 300:  // trunc
                case 310:  // setExpression
                case 311:  // allElementsTrue
                case 312:  // anyElementTrue
                case 313:  // setDifference
                case 314:  // setEquals
                case 315:  // setIntersection
                case 316:  // setIsSubset
                case 317:  // setUnion
                case 318:  // trig
                case 319:  // sin
                case 320:  // cos
                case 321:  // tan
                case 322:  // sinh
                case 323:  // cosh
                case 324:  // tanh
                case 325:  // asin
                case 326:  // acos
                case 327:  // atan
                case 328:  // asinh
                case 329:  // acosh
                case 330:  // atanh
                case 331:  // atan2
                case 332:  // degreesToRadians
                case 333:  // radiansToDegrees
                case 334:  // nonArrayExpression
                case 335:  // nonArrayCompoundExpression
                case 336:  // nonArrayNonObjCompoundExpression
                case 337:  // expressionSingletonArray
                case 338:  // singleArgExpression
                case 339:  // match
                case 340:  // predicates
                case 341:  // compoundMatchExprs
                case 342:  // predValue
                case 343:  // additionalExprs
                case 349:  // sortSpecs
                case 350:  // specList
                case 351:  // metaSort
                case 352:  // oneOrNegOne
                case 353:  // metaSortKeyword
                    value.move<CNode>(std::move(that.value));
                    break;

                case 165:  // aggregationProjectionFieldname
                case 166:  // projectionFieldname
                case 167:  // expressionFieldname
                case 168:  // stageAsUserFieldname
                case 169:  // argAsUserFieldname
                case 170:  // argAsProjectionPath
                case 171:  // aggExprAsUserFieldname
                case 172:  // invariableUserFieldname
                case 173:  // idAsUserFieldname
                case 174:  // idAsProjectionPath
                case 175:  // valueFieldname
                case 176:  // predFieldname
                case 348:  // logicalExprField
                    value.move<CNode::Fieldname>(std::move(that.value));
                    break;

                case 147:  // "Date"
                    value.move<Date_t>(std::move(that.value));
                    break;

                case 157:  // "arbitrary decimal"
                    value.move<Decimal128>(std::move(that.value));
                    break;

                case 146:  // "ObjectID"
                    value.move<OID>(std::move(that.value));
                    break;

                case 158:  // "Timestamp"
                    value.move<Timestamp>(std::move(that.value));
                    break;

                case 160:  // "maxKey"
                    value.move<UserMaxKey>(std::move(that.value));
                    break;

                case 159:  // "minKey"
                    value.move<UserMinKey>(std::move(that.value));
                    break;

                case 148:  // "null"
                    value.move<UserNull>(std::move(that.value));
                    break;

                case 145:  // "undefined"
                    value.move<UserUndefined>(std::move(that.value));
                    break;

                case 156:  // "arbitrary double"
                    value.move<double>(std::move(that.value));
                    break;

                case 154:  // "arbitrary integer"
                    value.move<int>(std::move(that.value));
                    break;

                case 155:  // "arbitrary long"
                    value.move<long long>(std::move(that.value));
                    break;

                case 177:  // projectField
                case 178:  // projectionObjectField
                case 179:  // expressionField
                case 180:  // valueField
                case 301:  // onErrorArg
                case 302:  // onNullArg
                case 303:  // formatArg
                case 304:  // timezoneArg
                case 305:  // charsArg
                case 306:  // optionsArg
                case 344:  // predicate
                case 345:  // logicalExpr
                case 346:  // operatorExpression
                case 347:  // notExpr
                case 354:  // sortSpec
                    value.move<std::pair<CNode::Fieldname, CNode>>(std::move(that.value));
                    break;

                case 138:  // "fieldname"
                case 140:  // "$-prefixed fieldname"
                case 141:  // "string"
                case 142:  // "$-prefixed string"
                case 143:  // "$$-prefixed string"
                case 181:  // arg
                    value.move<std::string>(std::move(that.value));
                    break;

                case 307:  // expressions
                case 308:  // values
                case 309:  // exprZeroToTwo
                    value.move<std::vector<CNode>>(std::move(that.value));
                    break;

                case 139:  // "fieldname containing dotted path"
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
                case 144:  // "BinData"
                    value.template destroy<BSONBinData>();
                    break;

                case 151:  // "Code"
                    value.template destroy<BSONCode>();
                    break;

                case 153:  // "CodeWScope"
                    value.template destroy<BSONCodeWScope>();
                    break;

                case 150:  // "dbPointer"
                    value.template destroy<BSONDBRef>();
                    break;

                case 149:  // "regex"
                    value.template destroy<BSONRegEx>();
                    break;

                case 152:  // "Symbol"
                    value.template destroy<BSONSymbol>();
                    break;

                case 182:  // dbPointer
                case 183:  // javascript
                case 184:  // symbol
                case 185:  // javascriptWScope
                case 186:  // int
                case 187:  // timestamp
                case 188:  // long
                case 189:  // double
                case 190:  // decimal
                case 191:  // minKey
                case 192:  // maxKey
                case 193:  // value
                case 194:  // string
                case 195:  // aggregationFieldPath
                case 196:  // binary
                case 197:  // undefined
                case 198:  // objectId
                case 199:  // bool
                case 200:  // date
                case 201:  // null
                case 202:  // regex
                case 203:  // simpleValue
                case 204:  // compoundValue
                case 205:  // valueArray
                case 206:  // valueObject
                case 207:  // valueFields
                case 208:  // variable
                case 209:  // pipeline
                case 210:  // stageList
                case 211:  // stage
                case 212:  // inhibitOptimization
                case 213:  // unionWith
                case 214:  // skip
                case 215:  // limit
                case 216:  // project
                case 217:  // sample
                case 218:  // projectFields
                case 219:  // projectionObjectFields
                case 220:  // topLevelProjection
                case 221:  // projection
                case 222:  // projectionObject
                case 223:  // num
                case 224:  // expression
                case 225:  // compoundNonObjectExpression
                case 226:  // exprFixedTwoArg
                case 227:  // exprFixedThreeArg
                case 228:  // arrayManipulation
                case 229:  // slice
                case 230:  // expressionArray
                case 231:  // expressionObject
                case 232:  // expressionFields
                case 233:  // maths
                case 234:  // meta
                case 235:  // add
                case 236:  // boolExprs
                case 237:  // and
                case 238:  // or
                case 239:  // not
                case 240:  // literalEscapes
                case 241:  // const
                case 242:  // literal
                case 243:  // stringExps
                case 244:  // concat
                case 245:  // dateFromString
                case 246:  // dateToString
                case 247:  // indexOfBytes
                case 248:  // indexOfCP
                case 249:  // ltrim
                case 250:  // regexFind
                case 251:  // regexFindAll
                case 252:  // regexMatch
                case 253:  // regexArgs
                case 254:  // replaceOne
                case 255:  // replaceAll
                case 256:  // rtrim
                case 257:  // split
                case 258:  // strLenBytes
                case 259:  // strLenCP
                case 260:  // strcasecmp
                case 261:  // substr
                case 262:  // substrBytes
                case 263:  // substrCP
                case 264:  // toLower
                case 265:  // toUpper
                case 266:  // trim
                case 267:  // compExprs
                case 268:  // cmp
                case 269:  // eq
                case 270:  // gt
                case 271:  // gte
                case 272:  // lt
                case 273:  // lte
                case 274:  // ne
                case 275:  // typeExpression
                case 276:  // convert
                case 277:  // toBool
                case 278:  // toDate
                case 279:  // toDecimal
                case 280:  // toDouble
                case 281:  // toInt
                case 282:  // toLong
                case 283:  // toObjectId
                case 284:  // toString
                case 285:  // type
                case 286:  // abs
                case 287:  // ceil
                case 288:  // divide
                case 289:  // exponent
                case 290:  // floor
                case 291:  // ln
                case 292:  // log
                case 293:  // logten
                case 294:  // mod
                case 295:  // multiply
                case 296:  // pow
                case 297:  // round
                case 298:  // sqrt
                case 299:  // subtract
                case 300:  // trunc
                case 310:  // setExpression
                case 311:  // allElementsTrue
                case 312:  // anyElementTrue
                case 313:  // setDifference
                case 314:  // setEquals
                case 315:  // setIntersection
                case 316:  // setIsSubset
                case 317:  // setUnion
                case 318:  // trig
                case 319:  // sin
                case 320:  // cos
                case 321:  // tan
                case 322:  // sinh
                case 323:  // cosh
                case 324:  // tanh
                case 325:  // asin
                case 326:  // acos
                case 327:  // atan
                case 328:  // asinh
                case 329:  // acosh
                case 330:  // atanh
                case 331:  // atan2
                case 332:  // degreesToRadians
                case 333:  // radiansToDegrees
                case 334:  // nonArrayExpression
                case 335:  // nonArrayCompoundExpression
                case 336:  // nonArrayNonObjCompoundExpression
                case 337:  // expressionSingletonArray
                case 338:  // singleArgExpression
                case 339:  // match
                case 340:  // predicates
                case 341:  // compoundMatchExprs
                case 342:  // predValue
                case 343:  // additionalExprs
                case 349:  // sortSpecs
                case 350:  // specList
                case 351:  // metaSort
                case 352:  // oneOrNegOne
                case 353:  // metaSortKeyword
                    value.template destroy<CNode>();
                    break;

                case 165:  // aggregationProjectionFieldname
                case 166:  // projectionFieldname
                case 167:  // expressionFieldname
                case 168:  // stageAsUserFieldname
                case 169:  // argAsUserFieldname
                case 170:  // argAsProjectionPath
                case 171:  // aggExprAsUserFieldname
                case 172:  // invariableUserFieldname
                case 173:  // idAsUserFieldname
                case 174:  // idAsProjectionPath
                case 175:  // valueFieldname
                case 176:  // predFieldname
                case 348:  // logicalExprField
                    value.template destroy<CNode::Fieldname>();
                    break;

                case 147:  // "Date"
                    value.template destroy<Date_t>();
                    break;

                case 157:  // "arbitrary decimal"
                    value.template destroy<Decimal128>();
                    break;

                case 146:  // "ObjectID"
                    value.template destroy<OID>();
                    break;

                case 158:  // "Timestamp"
                    value.template destroy<Timestamp>();
                    break;

                case 160:  // "maxKey"
                    value.template destroy<UserMaxKey>();
                    break;

                case 159:  // "minKey"
                    value.template destroy<UserMinKey>();
                    break;

                case 148:  // "null"
                    value.template destroy<UserNull>();
                    break;

                case 145:  // "undefined"
                    value.template destroy<UserUndefined>();
                    break;

                case 156:  // "arbitrary double"
                    value.template destroy<double>();
                    break;

                case 154:  // "arbitrary integer"
                    value.template destroy<int>();
                    break;

                case 155:  // "arbitrary long"
                    value.template destroy<long long>();
                    break;

                case 177:  // projectField
                case 178:  // projectionObjectField
                case 179:  // expressionField
                case 180:  // valueField
                case 301:  // onErrorArg
                case 302:  // onNullArg
                case 303:  // formatArg
                case 304:  // timezoneArg
                case 305:  // charsArg
                case 306:  // optionsArg
                case 344:  // predicate
                case 345:  // logicalExpr
                case 346:  // operatorExpression
                case 347:  // notExpr
                case 354:  // sortSpec
                    value.template destroy<std::pair<CNode::Fieldname, CNode>>();
                    break;

                case 138:  // "fieldname"
                case 140:  // "$-prefixed fieldname"
                case 141:  // "string"
                case 142:  // "$-prefixed string"
                case 143:  // "$$-prefixed string"
                case 181:  // arg
                    value.template destroy<std::string>();
                    break;

                case 307:  // expressions
                case 308:  // values
                case 309:  // exprZeroToTwo
                    value.template destroy<std::vector<CNode>>();
                    break;

                case 139:  // "fieldname containing dotted path"
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
                tok == token::ANY_ELEMENT_TRUE || tok == token::ARG_CHARS ||
                tok == token::ARG_COLL || tok == token::ARG_DATE || tok == token::ARG_DATE_STRING ||
                tok == token::ARG_FIND || tok == token::ARG_FORMAT || tok == token::ARG_INPUT ||
                tok == token::ARG_ON_ERROR || tok == token::ARG_ON_NULL ||
                tok == token::ARG_OPTIONS || tok == token::ARG_PIPELINE ||
                tok == token::ARG_REGEX || tok == token::ARG_REPLACEMENT ||
                tok == token::ARG_SIZE || tok == token::ARG_TIMEZONE || tok == token::ARG_TO ||
                tok == token::ASIN || tok == token::ASINH || tok == token::ATAN ||
                tok == token::ATAN2 || tok == token::ATANH || tok == token::BOOL_FALSE ||
                tok == token::BOOL_TRUE || tok == token::CEIL || tok == token::CMP ||
                tok == token::CONCAT || tok == token::CONST_EXPR || tok == token::CONVERT ||
                tok == token::COS || tok == token::COSH || tok == token::DATE_FROM_STRING ||
                tok == token::DATE_TO_STRING || tok == token::DECIMAL_NEGATIVE_ONE ||
                tok == token::DECIMAL_ONE || tok == token::DECIMAL_ZERO ||
                tok == token::DEGREES_TO_RADIANS || tok == token::DIVIDE ||
                tok == token::DOUBLE_NEGATIVE_ONE || tok == token::DOUBLE_ONE ||
                tok == token::DOUBLE_ZERO || tok == token::END_ARRAY || tok == token::END_OBJECT ||
                tok == token::EQ || tok == token::EXPONENT || tok == token::FLOOR ||
                tok == token::GEO_NEAR_DISTANCE || tok == token::GEO_NEAR_POINT ||
                tok == token::GT || tok == token::GTE || tok == token::ID ||
                tok == token::INDEX_OF_BYTES || tok == token::INDEX_OF_CP ||
                tok == token::INDEX_KEY || tok == token::INT_NEGATIVE_ONE ||
                tok == token::INT_ONE || tok == token::INT_ZERO || tok == token::LITERAL ||
                tok == token::LN || tok == token::LOG || tok == token::LOGTEN ||
                tok == token::LONG_NEGATIVE_ONE || tok == token::LONG_ONE ||
                tok == token::LONG_ZERO || tok == token::LT || tok == token::LTE ||
                tok == token::LTRIM || tok == token::META || tok == token::MOD ||
                tok == token::MULTIPLY || tok == token::NE || tok == token::NOR ||
                tok == token::NOT || tok == token::OR || tok == token::POW ||
                tok == token::RADIANS_TO_DEGREES || tok == token::RAND_VAL ||
                tok == token::RECORD_ID || tok == token::REGEX_FIND ||
                tok == token::REGEX_FIND_ALL || tok == token::REGEX_MATCH ||
                tok == token::REPLACE_ALL || tok == token::REPLACE_ONE || tok == token::ROUND ||
                tok == token::RTRIM || tok == token::SEARCH_HIGHLIGHTS ||
                tok == token::SEARCH_SCORE || tok == token::SET_DIFFERENCE ||
                tok == token::SET_EQUALS || tok == token::SET_INTERSECTION ||
                tok == token::SET_IS_SUBSET || tok == token::SET_UNION || tok == token::SLICE ||
                tok == token::SORT_KEY || tok == token::SIN || tok == token::SINH ||
                tok == token::SPLIT || tok == token::SQRT ||
                tok == token::STAGE_INHIBIT_OPTIMIZATION || tok == token::STAGE_LIMIT ||
                tok == token::STAGE_PROJECT || tok == token::STAGE_SAMPLE ||
                tok == token::STAGE_SKIP || tok == token::STAGE_UNION_WITH ||
                tok == token::START_ARRAY || tok == token::START_OBJECT ||
                tok == token::STR_CASE_CMP || tok == token::STR_LEN_BYTES ||
                tok == token::STR_LEN_CP || tok == token::SUBSTR || tok == token::SUBSTR_BYTES ||
                tok == token::SUBSTR_CP || tok == token::SUBTRACT || tok == token::TAN ||
                tok == token::TANH || tok == token::TEXT_SCORE || tok == token::TO_BOOL ||
                tok == token::TO_DATE || tok == token::TO_DECIMAL || tok == token::TO_DOUBLE ||
                tok == token::TO_INT || tok == token::TO_LONG || tok == token::TO_LOWER ||
                tok == token::TO_OBJECT_ID || tok == token::TO_STRING || tok == token::TO_UPPER ||
                tok == token::TRIM || tok == token::TRUNC || tok == token::TYPE ||
                tok == token::START_PIPELINE || tok == token::START_MATCH ||
                tok == token::START_SORT);
        }
#else
        symbol_type(int tok, const location_type& l) : super_type(token_type(tok), l) {
            YY_ASSERT(
                tok == token::END_OF_FILE || tok == token::YYerror || tok == token::YYUNDEF ||
                tok == token::ABS || tok == token::ACOS || tok == token::ACOSH ||
                tok == token::ADD || tok == token::ALL_ELEMENTS_TRUE || tok == token::AND ||
                tok == token::ANY_ELEMENT_TRUE || tok == token::ARG_CHARS ||
                tok == token::ARG_COLL || tok == token::ARG_DATE || tok == token::ARG_DATE_STRING ||
                tok == token::ARG_FIND || tok == token::ARG_FORMAT || tok == token::ARG_INPUT ||
                tok == token::ARG_ON_ERROR || tok == token::ARG_ON_NULL ||
                tok == token::ARG_OPTIONS || tok == token::ARG_PIPELINE ||
                tok == token::ARG_REGEX || tok == token::ARG_REPLACEMENT ||
                tok == token::ARG_SIZE || tok == token::ARG_TIMEZONE || tok == token::ARG_TO ||
                tok == token::ASIN || tok == token::ASINH || tok == token::ATAN ||
                tok == token::ATAN2 || tok == token::ATANH || tok == token::BOOL_FALSE ||
                tok == token::BOOL_TRUE || tok == token::CEIL || tok == token::CMP ||
                tok == token::CONCAT || tok == token::CONST_EXPR || tok == token::CONVERT ||
                tok == token::COS || tok == token::COSH || tok == token::DATE_FROM_STRING ||
                tok == token::DATE_TO_STRING || tok == token::DECIMAL_NEGATIVE_ONE ||
                tok == token::DECIMAL_ONE || tok == token::DECIMAL_ZERO ||
                tok == token::DEGREES_TO_RADIANS || tok == token::DIVIDE ||
                tok == token::DOUBLE_NEGATIVE_ONE || tok == token::DOUBLE_ONE ||
                tok == token::DOUBLE_ZERO || tok == token::END_ARRAY || tok == token::END_OBJECT ||
                tok == token::EQ || tok == token::EXPONENT || tok == token::FLOOR ||
                tok == token::GEO_NEAR_DISTANCE || tok == token::GEO_NEAR_POINT ||
                tok == token::GT || tok == token::GTE || tok == token::ID ||
                tok == token::INDEX_OF_BYTES || tok == token::INDEX_OF_CP ||
                tok == token::INDEX_KEY || tok == token::INT_NEGATIVE_ONE ||
                tok == token::INT_ONE || tok == token::INT_ZERO || tok == token::LITERAL ||
                tok == token::LN || tok == token::LOG || tok == token::LOGTEN ||
                tok == token::LONG_NEGATIVE_ONE || tok == token::LONG_ONE ||
                tok == token::LONG_ZERO || tok == token::LT || tok == token::LTE ||
                tok == token::LTRIM || tok == token::META || tok == token::MOD ||
                tok == token::MULTIPLY || tok == token::NE || tok == token::NOR ||
                tok == token::NOT || tok == token::OR || tok == token::POW ||
                tok == token::RADIANS_TO_DEGREES || tok == token::RAND_VAL ||
                tok == token::RECORD_ID || tok == token::REGEX_FIND ||
                tok == token::REGEX_FIND_ALL || tok == token::REGEX_MATCH ||
                tok == token::REPLACE_ALL || tok == token::REPLACE_ONE || tok == token::ROUND ||
                tok == token::RTRIM || tok == token::SEARCH_HIGHLIGHTS ||
                tok == token::SEARCH_SCORE || tok == token::SET_DIFFERENCE ||
                tok == token::SET_EQUALS || tok == token::SET_INTERSECTION ||
                tok == token::SET_IS_SUBSET || tok == token::SET_UNION || tok == token::SLICE ||
                tok == token::SORT_KEY || tok == token::SIN || tok == token::SINH ||
                tok == token::SPLIT || tok == token::SQRT ||
                tok == token::STAGE_INHIBIT_OPTIMIZATION || tok == token::STAGE_LIMIT ||
                tok == token::STAGE_PROJECT || tok == token::STAGE_SAMPLE ||
                tok == token::STAGE_SKIP || tok == token::STAGE_UNION_WITH ||
                tok == token::START_ARRAY || tok == token::START_OBJECT ||
                tok == token::STR_CASE_CMP || tok == token::STR_LEN_BYTES ||
                tok == token::STR_LEN_CP || tok == token::SUBSTR || tok == token::SUBSTR_BYTES ||
                tok == token::SUBSTR_CP || tok == token::SUBTRACT || tok == token::TAN ||
                tok == token::TANH || tok == token::TEXT_SCORE || tok == token::TO_BOOL ||
                tok == token::TO_DATE || tok == token::TO_DECIMAL || tok == token::TO_DOUBLE ||
                tok == token::TO_INT || tok == token::TO_LONG || tok == token::TO_LOWER ||
                tok == token::TO_OBJECT_ID || tok == token::TO_STRING || tok == token::TO_UPPER ||
                tok == token::TRIM || tok == token::TRUNC || tok == token::TYPE ||
                tok == token::START_PIPELINE || tok == token::START_MATCH ||
                tok == token::START_SORT);
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
    static symbol_type make_ARG_INPUT(location_type l) {
        return symbol_type(token::ARG_INPUT, std::move(l));
    }
#else
    static symbol_type make_ARG_INPUT(const location_type& l) {
        return symbol_type(token::ARG_INPUT, l);
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
    static symbol_type make_DATE_FROM_STRING(location_type l) {
        return symbol_type(token::DATE_FROM_STRING, std::move(l));
    }
#else
    static symbol_type make_DATE_FROM_STRING(const location_type& l) {
        return symbol_type(token::DATE_FROM_STRING, l);
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
    static symbol_type make_EXPONENT(location_type l) {
        return symbol_type(token::EXPONENT, std::move(l));
    }
#else
    static symbol_type make_EXPONENT(const location_type& l) {
        return symbol_type(token::EXPONENT, l);
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
    static symbol_type make_ID(location_type l) {
        return symbol_type(token::ID, std::move(l));
    }
#else
    static symbol_type make_ID(const location_type& l) {
        return symbol_type(token::ID, l);
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
    static symbol_type make_INDEX_KEY(location_type l) {
        return symbol_type(token::INDEX_KEY, std::move(l));
    }
#else
    static symbol_type make_INDEX_KEY(const location_type& l) {
        return symbol_type(token::INDEX_KEY, l);
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
    static symbol_type make_MOD(location_type l) {
        return symbol_type(token::MOD, std::move(l));
    }
#else
    static symbol_type make_MOD(const location_type& l) {
        return symbol_type(token::MOD, l);
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
    static const signed char yytable_ninf_;

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
        /// \a S_YYEMPTY when empty.
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
        yylast_ = 3007,  ///< Last index in yytable_.
        yynnts_ = 194,   ///< Number of nonterminal symbols.
        yyfinal_ = 11    ///< Termination state number.
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
        case 144:  // "BinData"
            value.copy<BSONBinData>(YY_MOVE(that.value));
            break;

        case 151:  // "Code"
            value.copy<BSONCode>(YY_MOVE(that.value));
            break;

        case 153:  // "CodeWScope"
            value.copy<BSONCodeWScope>(YY_MOVE(that.value));
            break;

        case 150:  // "dbPointer"
            value.copy<BSONDBRef>(YY_MOVE(that.value));
            break;

        case 149:  // "regex"
            value.copy<BSONRegEx>(YY_MOVE(that.value));
            break;

        case 152:  // "Symbol"
            value.copy<BSONSymbol>(YY_MOVE(that.value));
            break;

        case 182:  // dbPointer
        case 183:  // javascript
        case 184:  // symbol
        case 185:  // javascriptWScope
        case 186:  // int
        case 187:  // timestamp
        case 188:  // long
        case 189:  // double
        case 190:  // decimal
        case 191:  // minKey
        case 192:  // maxKey
        case 193:  // value
        case 194:  // string
        case 195:  // aggregationFieldPath
        case 196:  // binary
        case 197:  // undefined
        case 198:  // objectId
        case 199:  // bool
        case 200:  // date
        case 201:  // null
        case 202:  // regex
        case 203:  // simpleValue
        case 204:  // compoundValue
        case 205:  // valueArray
        case 206:  // valueObject
        case 207:  // valueFields
        case 208:  // variable
        case 209:  // pipeline
        case 210:  // stageList
        case 211:  // stage
        case 212:  // inhibitOptimization
        case 213:  // unionWith
        case 214:  // skip
        case 215:  // limit
        case 216:  // project
        case 217:  // sample
        case 218:  // projectFields
        case 219:  // projectionObjectFields
        case 220:  // topLevelProjection
        case 221:  // projection
        case 222:  // projectionObject
        case 223:  // num
        case 224:  // expression
        case 225:  // compoundNonObjectExpression
        case 226:  // exprFixedTwoArg
        case 227:  // exprFixedThreeArg
        case 228:  // arrayManipulation
        case 229:  // slice
        case 230:  // expressionArray
        case 231:  // expressionObject
        case 232:  // expressionFields
        case 233:  // maths
        case 234:  // meta
        case 235:  // add
        case 236:  // boolExprs
        case 237:  // and
        case 238:  // or
        case 239:  // not
        case 240:  // literalEscapes
        case 241:  // const
        case 242:  // literal
        case 243:  // stringExps
        case 244:  // concat
        case 245:  // dateFromString
        case 246:  // dateToString
        case 247:  // indexOfBytes
        case 248:  // indexOfCP
        case 249:  // ltrim
        case 250:  // regexFind
        case 251:  // regexFindAll
        case 252:  // regexMatch
        case 253:  // regexArgs
        case 254:  // replaceOne
        case 255:  // replaceAll
        case 256:  // rtrim
        case 257:  // split
        case 258:  // strLenBytes
        case 259:  // strLenCP
        case 260:  // strcasecmp
        case 261:  // substr
        case 262:  // substrBytes
        case 263:  // substrCP
        case 264:  // toLower
        case 265:  // toUpper
        case 266:  // trim
        case 267:  // compExprs
        case 268:  // cmp
        case 269:  // eq
        case 270:  // gt
        case 271:  // gte
        case 272:  // lt
        case 273:  // lte
        case 274:  // ne
        case 275:  // typeExpression
        case 276:  // convert
        case 277:  // toBool
        case 278:  // toDate
        case 279:  // toDecimal
        case 280:  // toDouble
        case 281:  // toInt
        case 282:  // toLong
        case 283:  // toObjectId
        case 284:  // toString
        case 285:  // type
        case 286:  // abs
        case 287:  // ceil
        case 288:  // divide
        case 289:  // exponent
        case 290:  // floor
        case 291:  // ln
        case 292:  // log
        case 293:  // logten
        case 294:  // mod
        case 295:  // multiply
        case 296:  // pow
        case 297:  // round
        case 298:  // sqrt
        case 299:  // subtract
        case 300:  // trunc
        case 310:  // setExpression
        case 311:  // allElementsTrue
        case 312:  // anyElementTrue
        case 313:  // setDifference
        case 314:  // setEquals
        case 315:  // setIntersection
        case 316:  // setIsSubset
        case 317:  // setUnion
        case 318:  // trig
        case 319:  // sin
        case 320:  // cos
        case 321:  // tan
        case 322:  // sinh
        case 323:  // cosh
        case 324:  // tanh
        case 325:  // asin
        case 326:  // acos
        case 327:  // atan
        case 328:  // asinh
        case 329:  // acosh
        case 330:  // atanh
        case 331:  // atan2
        case 332:  // degreesToRadians
        case 333:  // radiansToDegrees
        case 334:  // nonArrayExpression
        case 335:  // nonArrayCompoundExpression
        case 336:  // nonArrayNonObjCompoundExpression
        case 337:  // expressionSingletonArray
        case 338:  // singleArgExpression
        case 339:  // match
        case 340:  // predicates
        case 341:  // compoundMatchExprs
        case 342:  // predValue
        case 343:  // additionalExprs
        case 349:  // sortSpecs
        case 350:  // specList
        case 351:  // metaSort
        case 352:  // oneOrNegOne
        case 353:  // metaSortKeyword
            value.copy<CNode>(YY_MOVE(that.value));
            break;

        case 165:  // aggregationProjectionFieldname
        case 166:  // projectionFieldname
        case 167:  // expressionFieldname
        case 168:  // stageAsUserFieldname
        case 169:  // argAsUserFieldname
        case 170:  // argAsProjectionPath
        case 171:  // aggExprAsUserFieldname
        case 172:  // invariableUserFieldname
        case 173:  // idAsUserFieldname
        case 174:  // idAsProjectionPath
        case 175:  // valueFieldname
        case 176:  // predFieldname
        case 348:  // logicalExprField
            value.copy<CNode::Fieldname>(YY_MOVE(that.value));
            break;

        case 147:  // "Date"
            value.copy<Date_t>(YY_MOVE(that.value));
            break;

        case 157:  // "arbitrary decimal"
            value.copy<Decimal128>(YY_MOVE(that.value));
            break;

        case 146:  // "ObjectID"
            value.copy<OID>(YY_MOVE(that.value));
            break;

        case 158:  // "Timestamp"
            value.copy<Timestamp>(YY_MOVE(that.value));
            break;

        case 160:  // "maxKey"
            value.copy<UserMaxKey>(YY_MOVE(that.value));
            break;

        case 159:  // "minKey"
            value.copy<UserMinKey>(YY_MOVE(that.value));
            break;

        case 148:  // "null"
            value.copy<UserNull>(YY_MOVE(that.value));
            break;

        case 145:  // "undefined"
            value.copy<UserUndefined>(YY_MOVE(that.value));
            break;

        case 156:  // "arbitrary double"
            value.copy<double>(YY_MOVE(that.value));
            break;

        case 154:  // "arbitrary integer"
            value.copy<int>(YY_MOVE(that.value));
            break;

        case 155:  // "arbitrary long"
            value.copy<long long>(YY_MOVE(that.value));
            break;

        case 177:  // projectField
        case 178:  // projectionObjectField
        case 179:  // expressionField
        case 180:  // valueField
        case 301:  // onErrorArg
        case 302:  // onNullArg
        case 303:  // formatArg
        case 304:  // timezoneArg
        case 305:  // charsArg
        case 306:  // optionsArg
        case 344:  // predicate
        case 345:  // logicalExpr
        case 346:  // operatorExpression
        case 347:  // notExpr
        case 354:  // sortSpec
            value.copy<std::pair<CNode::Fieldname, CNode>>(YY_MOVE(that.value));
            break;

        case 138:  // "fieldname"
        case 140:  // "$-prefixed fieldname"
        case 141:  // "string"
        case 142:  // "$-prefixed string"
        case 143:  // "$$-prefixed string"
        case 181:  // arg
            value.copy<std::string>(YY_MOVE(that.value));
            break;

        case 307:  // expressions
        case 308:  // values
        case 309:  // exprZeroToTwo
            value.copy<std::vector<CNode>>(YY_MOVE(that.value));
            break;

        case 139:  // "fieldname containing dotted path"
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
        case 144:  // "BinData"
            value.move<BSONBinData>(YY_MOVE(s.value));
            break;

        case 151:  // "Code"
            value.move<BSONCode>(YY_MOVE(s.value));
            break;

        case 153:  // "CodeWScope"
            value.move<BSONCodeWScope>(YY_MOVE(s.value));
            break;

        case 150:  // "dbPointer"
            value.move<BSONDBRef>(YY_MOVE(s.value));
            break;

        case 149:  // "regex"
            value.move<BSONRegEx>(YY_MOVE(s.value));
            break;

        case 152:  // "Symbol"
            value.move<BSONSymbol>(YY_MOVE(s.value));
            break;

        case 182:  // dbPointer
        case 183:  // javascript
        case 184:  // symbol
        case 185:  // javascriptWScope
        case 186:  // int
        case 187:  // timestamp
        case 188:  // long
        case 189:  // double
        case 190:  // decimal
        case 191:  // minKey
        case 192:  // maxKey
        case 193:  // value
        case 194:  // string
        case 195:  // aggregationFieldPath
        case 196:  // binary
        case 197:  // undefined
        case 198:  // objectId
        case 199:  // bool
        case 200:  // date
        case 201:  // null
        case 202:  // regex
        case 203:  // simpleValue
        case 204:  // compoundValue
        case 205:  // valueArray
        case 206:  // valueObject
        case 207:  // valueFields
        case 208:  // variable
        case 209:  // pipeline
        case 210:  // stageList
        case 211:  // stage
        case 212:  // inhibitOptimization
        case 213:  // unionWith
        case 214:  // skip
        case 215:  // limit
        case 216:  // project
        case 217:  // sample
        case 218:  // projectFields
        case 219:  // projectionObjectFields
        case 220:  // topLevelProjection
        case 221:  // projection
        case 222:  // projectionObject
        case 223:  // num
        case 224:  // expression
        case 225:  // compoundNonObjectExpression
        case 226:  // exprFixedTwoArg
        case 227:  // exprFixedThreeArg
        case 228:  // arrayManipulation
        case 229:  // slice
        case 230:  // expressionArray
        case 231:  // expressionObject
        case 232:  // expressionFields
        case 233:  // maths
        case 234:  // meta
        case 235:  // add
        case 236:  // boolExprs
        case 237:  // and
        case 238:  // or
        case 239:  // not
        case 240:  // literalEscapes
        case 241:  // const
        case 242:  // literal
        case 243:  // stringExps
        case 244:  // concat
        case 245:  // dateFromString
        case 246:  // dateToString
        case 247:  // indexOfBytes
        case 248:  // indexOfCP
        case 249:  // ltrim
        case 250:  // regexFind
        case 251:  // regexFindAll
        case 252:  // regexMatch
        case 253:  // regexArgs
        case 254:  // replaceOne
        case 255:  // replaceAll
        case 256:  // rtrim
        case 257:  // split
        case 258:  // strLenBytes
        case 259:  // strLenCP
        case 260:  // strcasecmp
        case 261:  // substr
        case 262:  // substrBytes
        case 263:  // substrCP
        case 264:  // toLower
        case 265:  // toUpper
        case 266:  // trim
        case 267:  // compExprs
        case 268:  // cmp
        case 269:  // eq
        case 270:  // gt
        case 271:  // gte
        case 272:  // lt
        case 273:  // lte
        case 274:  // ne
        case 275:  // typeExpression
        case 276:  // convert
        case 277:  // toBool
        case 278:  // toDate
        case 279:  // toDecimal
        case 280:  // toDouble
        case 281:  // toInt
        case 282:  // toLong
        case 283:  // toObjectId
        case 284:  // toString
        case 285:  // type
        case 286:  // abs
        case 287:  // ceil
        case 288:  // divide
        case 289:  // exponent
        case 290:  // floor
        case 291:  // ln
        case 292:  // log
        case 293:  // logten
        case 294:  // mod
        case 295:  // multiply
        case 296:  // pow
        case 297:  // round
        case 298:  // sqrt
        case 299:  // subtract
        case 300:  // trunc
        case 310:  // setExpression
        case 311:  // allElementsTrue
        case 312:  // anyElementTrue
        case 313:  // setDifference
        case 314:  // setEquals
        case 315:  // setIntersection
        case 316:  // setIsSubset
        case 317:  // setUnion
        case 318:  // trig
        case 319:  // sin
        case 320:  // cos
        case 321:  // tan
        case 322:  // sinh
        case 323:  // cosh
        case 324:  // tanh
        case 325:  // asin
        case 326:  // acos
        case 327:  // atan
        case 328:  // asinh
        case 329:  // acosh
        case 330:  // atanh
        case 331:  // atan2
        case 332:  // degreesToRadians
        case 333:  // radiansToDegrees
        case 334:  // nonArrayExpression
        case 335:  // nonArrayCompoundExpression
        case 336:  // nonArrayNonObjCompoundExpression
        case 337:  // expressionSingletonArray
        case 338:  // singleArgExpression
        case 339:  // match
        case 340:  // predicates
        case 341:  // compoundMatchExprs
        case 342:  // predValue
        case 343:  // additionalExprs
        case 349:  // sortSpecs
        case 350:  // specList
        case 351:  // metaSort
        case 352:  // oneOrNegOne
        case 353:  // metaSortKeyword
            value.move<CNode>(YY_MOVE(s.value));
            break;

        case 165:  // aggregationProjectionFieldname
        case 166:  // projectionFieldname
        case 167:  // expressionFieldname
        case 168:  // stageAsUserFieldname
        case 169:  // argAsUserFieldname
        case 170:  // argAsProjectionPath
        case 171:  // aggExprAsUserFieldname
        case 172:  // invariableUserFieldname
        case 173:  // idAsUserFieldname
        case 174:  // idAsProjectionPath
        case 175:  // valueFieldname
        case 176:  // predFieldname
        case 348:  // logicalExprField
            value.move<CNode::Fieldname>(YY_MOVE(s.value));
            break;

        case 147:  // "Date"
            value.move<Date_t>(YY_MOVE(s.value));
            break;

        case 157:  // "arbitrary decimal"
            value.move<Decimal128>(YY_MOVE(s.value));
            break;

        case 146:  // "ObjectID"
            value.move<OID>(YY_MOVE(s.value));
            break;

        case 158:  // "Timestamp"
            value.move<Timestamp>(YY_MOVE(s.value));
            break;

        case 160:  // "maxKey"
            value.move<UserMaxKey>(YY_MOVE(s.value));
            break;

        case 159:  // "minKey"
            value.move<UserMinKey>(YY_MOVE(s.value));
            break;

        case 148:  // "null"
            value.move<UserNull>(YY_MOVE(s.value));
            break;

        case 145:  // "undefined"
            value.move<UserUndefined>(YY_MOVE(s.value));
            break;

        case 156:  // "arbitrary double"
            value.move<double>(YY_MOVE(s.value));
            break;

        case 154:  // "arbitrary integer"
            value.move<int>(YY_MOVE(s.value));
            break;

        case 155:  // "arbitrary long"
            value.move<long long>(YY_MOVE(s.value));
            break;

        case 177:  // projectField
        case 178:  // projectionObjectField
        case 179:  // expressionField
        case 180:  // valueField
        case 301:  // onErrorArg
        case 302:  // onNullArg
        case 303:  // formatArg
        case 304:  // timezoneArg
        case 305:  // charsArg
        case 306:  // optionsArg
        case 344:  // predicate
        case 345:  // logicalExpr
        case 346:  // operatorExpression
        case 347:  // notExpr
        case 354:  // sortSpec
            value.move<std::pair<CNode::Fieldname, CNode>>(YY_MOVE(s.value));
            break;

        case 138:  // "fieldname"
        case 140:  // "$-prefixed fieldname"
        case 141:  // "string"
        case 142:  // "$-prefixed string"
        case 143:  // "$$-prefixed string"
        case 181:  // arg
            value.move<std::string>(YY_MOVE(s.value));
            break;

        case 307:  // expressions
        case 308:  // values
        case 309:  // exprZeroToTwo
            value.move<std::vector<CNode>>(YY_MOVE(s.value));
            break;

        case 139:  // "fieldname containing dotted path"
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
#line 6071 "parser_gen.hpp"


#endif  // !YY_YY_PARSER_GEN_HPP_INCLUDED
