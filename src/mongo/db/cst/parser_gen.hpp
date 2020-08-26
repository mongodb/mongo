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
            // atan2
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
            END_OF_FILE = 0,                  // "EOF"
            YYerror = 1,                      // error
            YYUNDEF = 2,                      // "invalid token"
            ABS = 3,                          // ABS
            ADD = 4,                          // ADD
            ALL_ELEMENTS_TRUE = 5,            // "allElementsTrue"
            AND = 6,                          // AND
            ANY_ELEMENT_TRUE = 7,             // "anyElementTrue"
            ARG_CHARS = 8,                    // "chars argument"
            ARG_COLL = 9,                     // "coll argument"
            ARG_DATE = 10,                    // "date argument"
            ARG_DATE_STRING = 11,             // "dateString argument"
            ARG_FILTER = 12,                  // "filter"
            ARG_FIND = 13,                    // "find argument"
            ARG_FORMAT = 14,                  // "format argument"
            ARG_INPUT = 15,                   // "input argument"
            ARG_ON_ERROR = 16,                // "onError argument"
            ARG_ON_NULL = 17,                 // "onNull argument"
            ARG_OPTIONS = 18,                 // "options argument"
            ARG_PIPELINE = 19,                // "pipeline argument"
            ARG_Q = 20,                       // "q"
            ARG_QUERY = 21,                   // "query"
            ARG_REGEX = 22,                   // "regex argument"
            ARG_REPLACEMENT = 23,             // "replacement argument"
            ARG_SIZE = 24,                    // "size argument"
            ARG_SORT = 25,                    // "sort argument"
            ARG_TIMEZONE = 26,                // "timezone argument"
            ARG_TO = 27,                      // "to argument"
            ATAN2 = 28,                       // ATAN2
            BOOL_FALSE = 29,                  // "false"
            BOOL_TRUE = 30,                   // "true"
            CEIL = 31,                        // CEIL
            CMP = 32,                         // CMP
            CONCAT = 33,                      // CONCAT
            CONST_EXPR = 34,                  // CONST_EXPR
            CONVERT = 35,                     // CONVERT
            DATE_FROM_STRING = 36,            // DATE_FROM_STRING
            DATE_TO_STRING = 37,              // DATE_TO_STRING
            DECIMAL_NEGATIVE_ONE = 38,        // "-1 (decimal)"
            DECIMAL_ONE = 39,                 // "1 (decimal)"
            DECIMAL_ZERO = 40,                // "zero (decimal)"
            DIVIDE = 41,                      // DIVIDE
            DOUBLE_NEGATIVE_ONE = 42,         // "-1 (double)"
            DOUBLE_ONE = 43,                  // "1 (double)"
            DOUBLE_ZERO = 44,                 // "zero (double)"
            END_ARRAY = 45,                   // "end of array"
            END_OBJECT = 46,                  // "end of object"
            EQ = 47,                          // EQ
            EXPONENT = 48,                    // EXPONENT
            FLOOR = 49,                       // FLOOR
            GEO_NEAR_DISTANCE = 50,           // "geoNearDistance"
            GEO_NEAR_POINT = 51,              // "geoNearPoint"
            GT = 52,                          // GT
            GTE = 53,                         // GTE
            ID = 54,                          // ID
            INDEX_OF_BYTES = 55,              // INDEX_OF_BYTES
            INDEX_OF_CP = 56,                 // INDEX_OF_CP
            INDEX_KEY = 57,                   // "indexKey"
            INT_NEGATIVE_ONE = 58,            // "-1 (int)"
            INT_ONE = 59,                     // "1 (int)"
            INT_ZERO = 60,                    // "zero (int)"
            LITERAL = 61,                     // LITERAL
            LN = 62,                          // LN
            LOG = 63,                         // LOG
            LOGTEN = 64,                      // LOGTEN
            LONG_NEGATIVE_ONE = 65,           // "-1 (long)"
            LONG_ONE = 66,                    // "1 (long)"
            LONG_ZERO = 67,                   // "zero (long)"
            LT = 68,                          // LT
            LTE = 69,                         // LTE
            LTRIM = 70,                       // LTRIM
            META = 71,                        // META
            MOD = 72,                         // MOD
            MULTIPLY = 73,                    // MULTIPLY
            NE = 74,                          // NE
            NOR = 75,                         // NOR
            NOT = 76,                         // NOT
            OR = 77,                          // OR
            POW = 78,                         // POW
            RAND_VAL = 79,                    // "randVal"
            RECORD_ID = 80,                   // "recordId"
            REGEX_FIND = 81,                  // REGEX_FIND
            REGEX_FIND_ALL = 82,              // REGEX_FIND_ALL
            REGEX_MATCH = 83,                 // REGEX_MATCH
            REPLACE_ALL = 84,                 // REPLACE_ALL
            REPLACE_ONE = 85,                 // REPLACE_ONE
            ROUND = 86,                       // ROUND
            RTRIM = 87,                       // RTRIM
            SEARCH_HIGHLIGHTS = 88,           // "searchHighlights"
            SEARCH_SCORE = 89,                // "searchScore"
            SET_DIFFERENCE = 90,              // "setDifference"
            SET_EQUALS = 91,                  // "setEquals"
            SET_INTERSECTION = 92,            // "setIntersection"
            SET_IS_SUBSET = 93,               // "setIsSubset"
            SET_UNION = 94,                   // "setUnion"
            SLICE = 95,                       // "slice"
            SORT_KEY = 96,                    // "sortKey"
            SPLIT = 97,                       // SPLIT
            SQRT = 98,                        // SQRT
            STAGE_INHIBIT_OPTIMIZATION = 99,  // STAGE_INHIBIT_OPTIMIZATION
            STAGE_LIMIT = 100,                // STAGE_LIMIT
            STAGE_PROJECT = 101,              // STAGE_PROJECT
            STAGE_SAMPLE = 102,               // STAGE_SAMPLE
            STAGE_SKIP = 103,                 // STAGE_SKIP
            STAGE_UNION_WITH = 104,           // STAGE_UNION_WITH
            START_ARRAY = 105,                // "array"
            START_OBJECT = 106,               // "object"
            STR_CASE_CMP = 107,               // STR_CASE_CMP
            STR_LEN_BYTES = 108,              // STR_LEN_BYTES
            STR_LEN_CP = 109,                 // STR_LEN_CP
            SUBSTR = 110,                     // SUBSTR
            SUBSTR_BYTES = 111,               // SUBSTR_BYTES
            SUBSTR_CP = 112,                  // SUBSTR_CP
            SUBTRACT = 113,                   // SUBTRACT
            TEXT_SCORE = 114,                 // "textScore"
            TO_BOOL = 115,                    // TO_BOOL
            TO_DATE = 116,                    // TO_DATE
            TO_DECIMAL = 117,                 // TO_DECIMAL
            TO_DOUBLE = 118,                  // TO_DOUBLE
            TO_INT = 119,                     // TO_INT
            TO_LONG = 120,                    // TO_LONG
            TO_LOWER = 121,                   // TO_LOWER
            TO_OBJECT_ID = 122,               // TO_OBJECT_ID
            TO_STRING = 123,                  // TO_STRING
            TO_UPPER = 124,                   // TO_UPPER
            TRIM = 125,                       // TRIM
            TRUNC = 126,                      // TRUNC
            TYPE = 127,                       // TYPE
            FIELDNAME = 128,                  // "fieldname"
            DOTTED_FIELDNAME = 129,           // "fieldname containing dotted path"
            DOLLAR_PREF_FIELDNAME = 130,      // "$-prefixed fieldname"
            STRING = 131,                     // "string"
            DOLLAR_STRING = 132,              // "$-prefixed string"
            DOLLAR_DOLLAR_STRING = 133,       // "$$-prefixed string"
            BINARY = 134,                     // "BinData"
            UNDEFINED = 135,                  // "undefined"
            OBJECT_ID = 136,                  // "ObjectID"
            DATE_LITERAL = 137,               // "Date"
            JSNULL = 138,                     // "null"
            REGEX = 139,                      // "regex"
            DB_POINTER = 140,                 // "dbPointer"
            JAVASCRIPT = 141,                 // "Code"
            SYMBOL = 142,                     // "Symbol"
            JAVASCRIPT_W_SCOPE = 143,         // "CodeWScope"
            INT_OTHER = 144,                  // "arbitrary integer"
            LONG_OTHER = 145,                 // "arbitrary long"
            DOUBLE_OTHER = 146,               // "arbitrary double"
            DECIMAL_OTHER = 147,              // "arbitrary decimal"
            TIMESTAMP = 148,                  // "Timestamp"
            MIN_KEY = 149,                    // "minKey"
            MAX_KEY = 150,                    // "maxKey"
            START_PIPELINE = 151,             // START_PIPELINE
            START_MATCH = 152,                // START_MATCH
            START_SORT = 153                  // START_SORT
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
            YYNTOKENS = 154,  ///< Number of tokens.
            S_YYEMPTY = -2,
            S_YYEOF = 0,                             // "EOF"
            S_YYerror = 1,                           // error
            S_YYUNDEF = 2,                           // "invalid token"
            S_ABS = 3,                               // ABS
            S_ADD = 4,                               // ADD
            S_ALL_ELEMENTS_TRUE = 5,                 // "allElementsTrue"
            S_AND = 6,                               // AND
            S_ANY_ELEMENT_TRUE = 7,                  // "anyElementTrue"
            S_ARG_CHARS = 8,                         // "chars argument"
            S_ARG_COLL = 9,                          // "coll argument"
            S_ARG_DATE = 10,                         // "date argument"
            S_ARG_DATE_STRING = 11,                  // "dateString argument"
            S_ARG_FILTER = 12,                       // "filter"
            S_ARG_FIND = 13,                         // "find argument"
            S_ARG_FORMAT = 14,                       // "format argument"
            S_ARG_INPUT = 15,                        // "input argument"
            S_ARG_ON_ERROR = 16,                     // "onError argument"
            S_ARG_ON_NULL = 17,                      // "onNull argument"
            S_ARG_OPTIONS = 18,                      // "options argument"
            S_ARG_PIPELINE = 19,                     // "pipeline argument"
            S_ARG_Q = 20,                            // "q"
            S_ARG_QUERY = 21,                        // "query"
            S_ARG_REGEX = 22,                        // "regex argument"
            S_ARG_REPLACEMENT = 23,                  // "replacement argument"
            S_ARG_SIZE = 24,                         // "size argument"
            S_ARG_SORT = 25,                         // "sort argument"
            S_ARG_TIMEZONE = 26,                     // "timezone argument"
            S_ARG_TO = 27,                           // "to argument"
            S_ATAN2 = 28,                            // ATAN2
            S_BOOL_FALSE = 29,                       // "false"
            S_BOOL_TRUE = 30,                        // "true"
            S_CEIL = 31,                             // CEIL
            S_CMP = 32,                              // CMP
            S_CONCAT = 33,                           // CONCAT
            S_CONST_EXPR = 34,                       // CONST_EXPR
            S_CONVERT = 35,                          // CONVERT
            S_DATE_FROM_STRING = 36,                 // DATE_FROM_STRING
            S_DATE_TO_STRING = 37,                   // DATE_TO_STRING
            S_DECIMAL_NEGATIVE_ONE = 38,             // "-1 (decimal)"
            S_DECIMAL_ONE = 39,                      // "1 (decimal)"
            S_DECIMAL_ZERO = 40,                     // "zero (decimal)"
            S_DIVIDE = 41,                           // DIVIDE
            S_DOUBLE_NEGATIVE_ONE = 42,              // "-1 (double)"
            S_DOUBLE_ONE = 43,                       // "1 (double)"
            S_DOUBLE_ZERO = 44,                      // "zero (double)"
            S_END_ARRAY = 45,                        // "end of array"
            S_END_OBJECT = 46,                       // "end of object"
            S_EQ = 47,                               // EQ
            S_EXPONENT = 48,                         // EXPONENT
            S_FLOOR = 49,                            // FLOOR
            S_GEO_NEAR_DISTANCE = 50,                // "geoNearDistance"
            S_GEO_NEAR_POINT = 51,                   // "geoNearPoint"
            S_GT = 52,                               // GT
            S_GTE = 53,                              // GTE
            S_ID = 54,                               // ID
            S_INDEX_OF_BYTES = 55,                   // INDEX_OF_BYTES
            S_INDEX_OF_CP = 56,                      // INDEX_OF_CP
            S_INDEX_KEY = 57,                        // "indexKey"
            S_INT_NEGATIVE_ONE = 58,                 // "-1 (int)"
            S_INT_ONE = 59,                          // "1 (int)"
            S_INT_ZERO = 60,                         // "zero (int)"
            S_LITERAL = 61,                          // LITERAL
            S_LN = 62,                               // LN
            S_LOG = 63,                              // LOG
            S_LOGTEN = 64,                           // LOGTEN
            S_LONG_NEGATIVE_ONE = 65,                // "-1 (long)"
            S_LONG_ONE = 66,                         // "1 (long)"
            S_LONG_ZERO = 67,                        // "zero (long)"
            S_LT = 68,                               // LT
            S_LTE = 69,                              // LTE
            S_LTRIM = 70,                            // LTRIM
            S_META = 71,                             // META
            S_MOD = 72,                              // MOD
            S_MULTIPLY = 73,                         // MULTIPLY
            S_NE = 74,                               // NE
            S_NOR = 75,                              // NOR
            S_NOT = 76,                              // NOT
            S_OR = 77,                               // OR
            S_POW = 78,                              // POW
            S_RAND_VAL = 79,                         // "randVal"
            S_RECORD_ID = 80,                        // "recordId"
            S_REGEX_FIND = 81,                       // REGEX_FIND
            S_REGEX_FIND_ALL = 82,                   // REGEX_FIND_ALL
            S_REGEX_MATCH = 83,                      // REGEX_MATCH
            S_REPLACE_ALL = 84,                      // REPLACE_ALL
            S_REPLACE_ONE = 85,                      // REPLACE_ONE
            S_ROUND = 86,                            // ROUND
            S_RTRIM = 87,                            // RTRIM
            S_SEARCH_HIGHLIGHTS = 88,                // "searchHighlights"
            S_SEARCH_SCORE = 89,                     // "searchScore"
            S_SET_DIFFERENCE = 90,                   // "setDifference"
            S_SET_EQUALS = 91,                       // "setEquals"
            S_SET_INTERSECTION = 92,                 // "setIntersection"
            S_SET_IS_SUBSET = 93,                    // "setIsSubset"
            S_SET_UNION = 94,                        // "setUnion"
            S_SLICE = 95,                            // "slice"
            S_SORT_KEY = 96,                         // "sortKey"
            S_SPLIT = 97,                            // SPLIT
            S_SQRT = 98,                             // SQRT
            S_STAGE_INHIBIT_OPTIMIZATION = 99,       // STAGE_INHIBIT_OPTIMIZATION
            S_STAGE_LIMIT = 100,                     // STAGE_LIMIT
            S_STAGE_PROJECT = 101,                   // STAGE_PROJECT
            S_STAGE_SAMPLE = 102,                    // STAGE_SAMPLE
            S_STAGE_SKIP = 103,                      // STAGE_SKIP
            S_STAGE_UNION_WITH = 104,                // STAGE_UNION_WITH
            S_START_ARRAY = 105,                     // "array"
            S_START_OBJECT = 106,                    // "object"
            S_STR_CASE_CMP = 107,                    // STR_CASE_CMP
            S_STR_LEN_BYTES = 108,                   // STR_LEN_BYTES
            S_STR_LEN_CP = 109,                      // STR_LEN_CP
            S_SUBSTR = 110,                          // SUBSTR
            S_SUBSTR_BYTES = 111,                    // SUBSTR_BYTES
            S_SUBSTR_CP = 112,                       // SUBSTR_CP
            S_SUBTRACT = 113,                        // SUBTRACT
            S_TEXT_SCORE = 114,                      // "textScore"
            S_TO_BOOL = 115,                         // TO_BOOL
            S_TO_DATE = 116,                         // TO_DATE
            S_TO_DECIMAL = 117,                      // TO_DECIMAL
            S_TO_DOUBLE = 118,                       // TO_DOUBLE
            S_TO_INT = 119,                          // TO_INT
            S_TO_LONG = 120,                         // TO_LONG
            S_TO_LOWER = 121,                        // TO_LOWER
            S_TO_OBJECT_ID = 122,                    // TO_OBJECT_ID
            S_TO_STRING = 123,                       // TO_STRING
            S_TO_UPPER = 124,                        // TO_UPPER
            S_TRIM = 125,                            // TRIM
            S_TRUNC = 126,                           // TRUNC
            S_TYPE = 127,                            // TYPE
            S_FIELDNAME = 128,                       // "fieldname"
            S_DOTTED_FIELDNAME = 129,                // "fieldname containing dotted path"
            S_DOLLAR_PREF_FIELDNAME = 130,           // "$-prefixed fieldname"
            S_STRING = 131,                          // "string"
            S_DOLLAR_STRING = 132,                   // "$-prefixed string"
            S_DOLLAR_DOLLAR_STRING = 133,            // "$$-prefixed string"
            S_BINARY = 134,                          // "BinData"
            S_UNDEFINED = 135,                       // "undefined"
            S_OBJECT_ID = 136,                       // "ObjectID"
            S_DATE_LITERAL = 137,                    // "Date"
            S_JSNULL = 138,                          // "null"
            S_REGEX = 139,                           // "regex"
            S_DB_POINTER = 140,                      // "dbPointer"
            S_JAVASCRIPT = 141,                      // "Code"
            S_SYMBOL = 142,                          // "Symbol"
            S_JAVASCRIPT_W_SCOPE = 143,              // "CodeWScope"
            S_INT_OTHER = 144,                       // "arbitrary integer"
            S_LONG_OTHER = 145,                      // "arbitrary long"
            S_DOUBLE_OTHER = 146,                    // "arbitrary double"
            S_DECIMAL_OTHER = 147,                   // "arbitrary decimal"
            S_TIMESTAMP = 148,                       // "Timestamp"
            S_MIN_KEY = 149,                         // "minKey"
            S_MAX_KEY = 150,                         // "maxKey"
            S_START_PIPELINE = 151,                  // START_PIPELINE
            S_START_MATCH = 152,                     // START_MATCH
            S_START_SORT = 153,                      // START_SORT
            S_YYACCEPT = 154,                        // $accept
            S_aggregationProjectionFieldname = 155,  // aggregationProjectionFieldname
            S_projectionFieldname = 156,             // projectionFieldname
            S_expressionFieldname = 157,             // expressionFieldname
            S_stageAsUserFieldname = 158,            // stageAsUserFieldname
            S_argAsUserFieldname = 159,              // argAsUserFieldname
            S_argAsProjectionPath = 160,             // argAsProjectionPath
            S_aggExprAsUserFieldname = 161,          // aggExprAsUserFieldname
            S_invariableUserFieldname = 162,         // invariableUserFieldname
            S_idAsUserFieldname = 163,               // idAsUserFieldname
            S_idAsProjectionPath = 164,              // idAsProjectionPath
            S_valueFieldname = 165,                  // valueFieldname
            S_predFieldname = 166,                   // predFieldname
            S_projectField = 167,                    // projectField
            S_projectionObjectField = 168,           // projectionObjectField
            S_expressionField = 169,                 // expressionField
            S_valueField = 170,                      // valueField
            S_arg = 171,                             // arg
            S_dbPointer = 172,                       // dbPointer
            S_javascript = 173,                      // javascript
            S_symbol = 174,                          // symbol
            S_javascriptWScope = 175,                // javascriptWScope
            S_int = 176,                             // int
            S_timestamp = 177,                       // timestamp
            S_long = 178,                            // long
            S_double = 179,                          // double
            S_decimal = 180,                         // decimal
            S_minKey = 181,                          // minKey
            S_maxKey = 182,                          // maxKey
            S_value = 183,                           // value
            S_string = 184,                          // string
            S_aggregationFieldPath = 185,            // aggregationFieldPath
            S_binary = 186,                          // binary
            S_undefined = 187,                       // undefined
            S_objectId = 188,                        // objectId
            S_bool = 189,                            // bool
            S_date = 190,                            // date
            S_null = 191,                            // null
            S_regex = 192,                           // regex
            S_simpleValue = 193,                     // simpleValue
            S_compoundValue = 194,                   // compoundValue
            S_valueArray = 195,                      // valueArray
            S_valueObject = 196,                     // valueObject
            S_valueFields = 197,                     // valueFields
            S_variable = 198,                        // variable
            S_pipeline = 199,                        // pipeline
            S_stageList = 200,                       // stageList
            S_stage = 201,                           // stage
            S_inhibitOptimization = 202,             // inhibitOptimization
            S_unionWith = 203,                       // unionWith
            S_skip = 204,                            // skip
            S_limit = 205,                           // limit
            S_project = 206,                         // project
            S_sample = 207,                          // sample
            S_projectFields = 208,                   // projectFields
            S_projectionObjectFields = 209,          // projectionObjectFields
            S_topLevelProjection = 210,              // topLevelProjection
            S_projection = 211,                      // projection
            S_projectionObject = 212,                // projectionObject
            S_num = 213,                             // num
            S_expression = 214,                      // expression
            S_compoundNonObjectExpression = 215,     // compoundNonObjectExpression
            S_exprFixedTwoArg = 216,                 // exprFixedTwoArg
            S_exprFixedThreeArg = 217,               // exprFixedThreeArg
            S_arrayManipulation = 218,               // arrayManipulation
            S_slice = 219,                           // slice
            S_expressionArray = 220,                 // expressionArray
            S_expressionObject = 221,                // expressionObject
            S_expressionFields = 222,                // expressionFields
            S_maths = 223,                           // maths
            S_meta = 224,                            // meta
            S_add = 225,                             // add
            S_atan2 = 226,                           // atan2
            S_boolExprs = 227,                       // boolExprs
            S_and = 228,                             // and
            S_or = 229,                              // or
            S_not = 230,                             // not
            S_literalEscapes = 231,                  // literalEscapes
            S_const = 232,                           // const
            S_literal = 233,                         // literal
            S_stringExps = 234,                      // stringExps
            S_concat = 235,                          // concat
            S_dateFromString = 236,                  // dateFromString
            S_dateToString = 237,                    // dateToString
            S_indexOfBytes = 238,                    // indexOfBytes
            S_indexOfCP = 239,                       // indexOfCP
            S_ltrim = 240,                           // ltrim
            S_regexFind = 241,                       // regexFind
            S_regexFindAll = 242,                    // regexFindAll
            S_regexMatch = 243,                      // regexMatch
            S_regexArgs = 244,                       // regexArgs
            S_replaceOne = 245,                      // replaceOne
            S_replaceAll = 246,                      // replaceAll
            S_rtrim = 247,                           // rtrim
            S_split = 248,                           // split
            S_strLenBytes = 249,                     // strLenBytes
            S_strLenCP = 250,                        // strLenCP
            S_strcasecmp = 251,                      // strcasecmp
            S_substr = 252,                          // substr
            S_substrBytes = 253,                     // substrBytes
            S_substrCP = 254,                        // substrCP
            S_toLower = 255,                         // toLower
            S_toUpper = 256,                         // toUpper
            S_trim = 257,                            // trim
            S_compExprs = 258,                       // compExprs
            S_cmp = 259,                             // cmp
            S_eq = 260,                              // eq
            S_gt = 261,                              // gt
            S_gte = 262,                             // gte
            S_lt = 263,                              // lt
            S_lte = 264,                             // lte
            S_ne = 265,                              // ne
            S_typeExpression = 266,                  // typeExpression
            S_convert = 267,                         // convert
            S_toBool = 268,                          // toBool
            S_toDate = 269,                          // toDate
            S_toDecimal = 270,                       // toDecimal
            S_toDouble = 271,                        // toDouble
            S_toInt = 272,                           // toInt
            S_toLong = 273,                          // toLong
            S_toObjectId = 274,                      // toObjectId
            S_toString = 275,                        // toString
            S_type = 276,                            // type
            S_abs = 277,                             // abs
            S_ceil = 278,                            // ceil
            S_divide = 279,                          // divide
            S_exponent = 280,                        // exponent
            S_floor = 281,                           // floor
            S_ln = 282,                              // ln
            S_log = 283,                             // log
            S_logten = 284,                          // logten
            S_mod = 285,                             // mod
            S_multiply = 286,                        // multiply
            S_pow = 287,                             // pow
            S_round = 288,                           // round
            S_sqrt = 289,                            // sqrt
            S_subtract = 290,                        // subtract
            S_trunc = 291,                           // trunc
            S_onErrorArg = 292,                      // onErrorArg
            S_onNullArg = 293,                       // onNullArg
            S_formatArg = 294,                       // formatArg
            S_timezoneArg = 295,                     // timezoneArg
            S_charsArg = 296,                        // charsArg
            S_optionsArg = 297,                      // optionsArg
            S_expressions = 298,                     // expressions
            S_values = 299,                          // values
            S_exprZeroToTwo = 300,                   // exprZeroToTwo
            S_setExpression = 301,                   // setExpression
            S_allElementsTrue = 302,                 // allElementsTrue
            S_anyElementTrue = 303,                  // anyElementTrue
            S_setDifference = 304,                   // setDifference
            S_setEquals = 305,                       // setEquals
            S_setIntersection = 306,                 // setIntersection
            S_setIsSubset = 307,                     // setIsSubset
            S_setUnion = 308,                        // setUnion
            S_match = 309,                           // match
            S_predicates = 310,                      // predicates
            S_compoundMatchExprs = 311,              // compoundMatchExprs
            S_predValue = 312,                       // predValue
            S_additionalExprs = 313,                 // additionalExprs
            S_predicate = 314,                       // predicate
            S_logicalExpr = 315,                     // logicalExpr
            S_operatorExpression = 316,              // operatorExpression
            S_notExpr = 317,                         // notExpr
            S_logicalExprField = 318,                // logicalExprField
            S_sortSpecs = 319,                       // sortSpecs
            S_specList = 320,                        // specList
            S_metaSort = 321,                        // metaSort
            S_oneOrNegOne = 322,                     // oneOrNegOne
            S_metaSortKeyword = 323,                 // metaSortKeyword
            S_sortSpec = 324,                        // sortSpec
            S_start = 325,                           // start
            S_START_ORDERED_OBJECT = 326,            // START_ORDERED_OBJECT
            S_327_1 = 327                            // $@1
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
                case 134:  // "BinData"
                    value.move<BSONBinData>(std::move(that.value));
                    break;

                case 141:  // "Code"
                    value.move<BSONCode>(std::move(that.value));
                    break;

                case 143:  // "CodeWScope"
                    value.move<BSONCodeWScope>(std::move(that.value));
                    break;

                case 140:  // "dbPointer"
                    value.move<BSONDBRef>(std::move(that.value));
                    break;

                case 139:  // "regex"
                    value.move<BSONRegEx>(std::move(that.value));
                    break;

                case 142:  // "Symbol"
                    value.move<BSONSymbol>(std::move(that.value));
                    break;

                case 172:  // dbPointer
                case 173:  // javascript
                case 174:  // symbol
                case 175:  // javascriptWScope
                case 176:  // int
                case 177:  // timestamp
                case 178:  // long
                case 179:  // double
                case 180:  // decimal
                case 181:  // minKey
                case 182:  // maxKey
                case 183:  // value
                case 184:  // string
                case 185:  // aggregationFieldPath
                case 186:  // binary
                case 187:  // undefined
                case 188:  // objectId
                case 189:  // bool
                case 190:  // date
                case 191:  // null
                case 192:  // regex
                case 193:  // simpleValue
                case 194:  // compoundValue
                case 195:  // valueArray
                case 196:  // valueObject
                case 197:  // valueFields
                case 198:  // variable
                case 199:  // pipeline
                case 200:  // stageList
                case 201:  // stage
                case 202:  // inhibitOptimization
                case 203:  // unionWith
                case 204:  // skip
                case 205:  // limit
                case 206:  // project
                case 207:  // sample
                case 208:  // projectFields
                case 209:  // projectionObjectFields
                case 210:  // topLevelProjection
                case 211:  // projection
                case 212:  // projectionObject
                case 213:  // num
                case 214:  // expression
                case 215:  // compoundNonObjectExpression
                case 216:  // exprFixedTwoArg
                case 217:  // exprFixedThreeArg
                case 218:  // arrayManipulation
                case 219:  // slice
                case 220:  // expressionArray
                case 221:  // expressionObject
                case 222:  // expressionFields
                case 223:  // maths
                case 224:  // meta
                case 225:  // add
                case 226:  // atan2
                case 227:  // boolExprs
                case 228:  // and
                case 229:  // or
                case 230:  // not
                case 231:  // literalEscapes
                case 232:  // const
                case 233:  // literal
                case 234:  // stringExps
                case 235:  // concat
                case 236:  // dateFromString
                case 237:  // dateToString
                case 238:  // indexOfBytes
                case 239:  // indexOfCP
                case 240:  // ltrim
                case 241:  // regexFind
                case 242:  // regexFindAll
                case 243:  // regexMatch
                case 244:  // regexArgs
                case 245:  // replaceOne
                case 246:  // replaceAll
                case 247:  // rtrim
                case 248:  // split
                case 249:  // strLenBytes
                case 250:  // strLenCP
                case 251:  // strcasecmp
                case 252:  // substr
                case 253:  // substrBytes
                case 254:  // substrCP
                case 255:  // toLower
                case 256:  // toUpper
                case 257:  // trim
                case 258:  // compExprs
                case 259:  // cmp
                case 260:  // eq
                case 261:  // gt
                case 262:  // gte
                case 263:  // lt
                case 264:  // lte
                case 265:  // ne
                case 266:  // typeExpression
                case 267:  // convert
                case 268:  // toBool
                case 269:  // toDate
                case 270:  // toDecimal
                case 271:  // toDouble
                case 272:  // toInt
                case 273:  // toLong
                case 274:  // toObjectId
                case 275:  // toString
                case 276:  // type
                case 277:  // abs
                case 278:  // ceil
                case 279:  // divide
                case 280:  // exponent
                case 281:  // floor
                case 282:  // ln
                case 283:  // log
                case 284:  // logten
                case 285:  // mod
                case 286:  // multiply
                case 287:  // pow
                case 288:  // round
                case 289:  // sqrt
                case 290:  // subtract
                case 291:  // trunc
                case 301:  // setExpression
                case 302:  // allElementsTrue
                case 303:  // anyElementTrue
                case 304:  // setDifference
                case 305:  // setEquals
                case 306:  // setIntersection
                case 307:  // setIsSubset
                case 308:  // setUnion
                case 309:  // match
                case 310:  // predicates
                case 311:  // compoundMatchExprs
                case 312:  // predValue
                case 313:  // additionalExprs
                case 319:  // sortSpecs
                case 320:  // specList
                case 321:  // metaSort
                case 322:  // oneOrNegOne
                case 323:  // metaSortKeyword
                    value.move<CNode>(std::move(that.value));
                    break;

                case 155:  // aggregationProjectionFieldname
                case 156:  // projectionFieldname
                case 157:  // expressionFieldname
                case 158:  // stageAsUserFieldname
                case 159:  // argAsUserFieldname
                case 160:  // argAsProjectionPath
                case 161:  // aggExprAsUserFieldname
                case 162:  // invariableUserFieldname
                case 163:  // idAsUserFieldname
                case 164:  // idAsProjectionPath
                case 165:  // valueFieldname
                case 166:  // predFieldname
                case 318:  // logicalExprField
                    value.move<CNode::Fieldname>(std::move(that.value));
                    break;

                case 137:  // "Date"
                    value.move<Date_t>(std::move(that.value));
                    break;

                case 147:  // "arbitrary decimal"
                    value.move<Decimal128>(std::move(that.value));
                    break;

                case 136:  // "ObjectID"
                    value.move<OID>(std::move(that.value));
                    break;

                case 148:  // "Timestamp"
                    value.move<Timestamp>(std::move(that.value));
                    break;

                case 150:  // "maxKey"
                    value.move<UserMaxKey>(std::move(that.value));
                    break;

                case 149:  // "minKey"
                    value.move<UserMinKey>(std::move(that.value));
                    break;

                case 138:  // "null"
                    value.move<UserNull>(std::move(that.value));
                    break;

                case 135:  // "undefined"
                    value.move<UserUndefined>(std::move(that.value));
                    break;

                case 146:  // "arbitrary double"
                    value.move<double>(std::move(that.value));
                    break;

                case 144:  // "arbitrary integer"
                    value.move<int>(std::move(that.value));
                    break;

                case 145:  // "arbitrary long"
                    value.move<long long>(std::move(that.value));
                    break;

                case 167:  // projectField
                case 168:  // projectionObjectField
                case 169:  // expressionField
                case 170:  // valueField
                case 292:  // onErrorArg
                case 293:  // onNullArg
                case 294:  // formatArg
                case 295:  // timezoneArg
                case 296:  // charsArg
                case 297:  // optionsArg
                case 314:  // predicate
                case 315:  // logicalExpr
                case 316:  // operatorExpression
                case 317:  // notExpr
                case 324:  // sortSpec
                    value.move<std::pair<CNode::Fieldname, CNode>>(std::move(that.value));
                    break;

                case 128:  // "fieldname"
                case 130:  // "$-prefixed fieldname"
                case 131:  // "string"
                case 132:  // "$-prefixed string"
                case 133:  // "$$-prefixed string"
                case 171:  // arg
                    value.move<std::string>(std::move(that.value));
                    break;

                case 298:  // expressions
                case 299:  // values
                case 300:  // exprZeroToTwo
                    value.move<std::vector<CNode>>(std::move(that.value));
                    break;

                case 129:  // "fieldname containing dotted path"
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
                case 134:  // "BinData"
                    value.template destroy<BSONBinData>();
                    break;

                case 141:  // "Code"
                    value.template destroy<BSONCode>();
                    break;

                case 143:  // "CodeWScope"
                    value.template destroy<BSONCodeWScope>();
                    break;

                case 140:  // "dbPointer"
                    value.template destroy<BSONDBRef>();
                    break;

                case 139:  // "regex"
                    value.template destroy<BSONRegEx>();
                    break;

                case 142:  // "Symbol"
                    value.template destroy<BSONSymbol>();
                    break;

                case 172:  // dbPointer
                case 173:  // javascript
                case 174:  // symbol
                case 175:  // javascriptWScope
                case 176:  // int
                case 177:  // timestamp
                case 178:  // long
                case 179:  // double
                case 180:  // decimal
                case 181:  // minKey
                case 182:  // maxKey
                case 183:  // value
                case 184:  // string
                case 185:  // aggregationFieldPath
                case 186:  // binary
                case 187:  // undefined
                case 188:  // objectId
                case 189:  // bool
                case 190:  // date
                case 191:  // null
                case 192:  // regex
                case 193:  // simpleValue
                case 194:  // compoundValue
                case 195:  // valueArray
                case 196:  // valueObject
                case 197:  // valueFields
                case 198:  // variable
                case 199:  // pipeline
                case 200:  // stageList
                case 201:  // stage
                case 202:  // inhibitOptimization
                case 203:  // unionWith
                case 204:  // skip
                case 205:  // limit
                case 206:  // project
                case 207:  // sample
                case 208:  // projectFields
                case 209:  // projectionObjectFields
                case 210:  // topLevelProjection
                case 211:  // projection
                case 212:  // projectionObject
                case 213:  // num
                case 214:  // expression
                case 215:  // compoundNonObjectExpression
                case 216:  // exprFixedTwoArg
                case 217:  // exprFixedThreeArg
                case 218:  // arrayManipulation
                case 219:  // slice
                case 220:  // expressionArray
                case 221:  // expressionObject
                case 222:  // expressionFields
                case 223:  // maths
                case 224:  // meta
                case 225:  // add
                case 226:  // atan2
                case 227:  // boolExprs
                case 228:  // and
                case 229:  // or
                case 230:  // not
                case 231:  // literalEscapes
                case 232:  // const
                case 233:  // literal
                case 234:  // stringExps
                case 235:  // concat
                case 236:  // dateFromString
                case 237:  // dateToString
                case 238:  // indexOfBytes
                case 239:  // indexOfCP
                case 240:  // ltrim
                case 241:  // regexFind
                case 242:  // regexFindAll
                case 243:  // regexMatch
                case 244:  // regexArgs
                case 245:  // replaceOne
                case 246:  // replaceAll
                case 247:  // rtrim
                case 248:  // split
                case 249:  // strLenBytes
                case 250:  // strLenCP
                case 251:  // strcasecmp
                case 252:  // substr
                case 253:  // substrBytes
                case 254:  // substrCP
                case 255:  // toLower
                case 256:  // toUpper
                case 257:  // trim
                case 258:  // compExprs
                case 259:  // cmp
                case 260:  // eq
                case 261:  // gt
                case 262:  // gte
                case 263:  // lt
                case 264:  // lte
                case 265:  // ne
                case 266:  // typeExpression
                case 267:  // convert
                case 268:  // toBool
                case 269:  // toDate
                case 270:  // toDecimal
                case 271:  // toDouble
                case 272:  // toInt
                case 273:  // toLong
                case 274:  // toObjectId
                case 275:  // toString
                case 276:  // type
                case 277:  // abs
                case 278:  // ceil
                case 279:  // divide
                case 280:  // exponent
                case 281:  // floor
                case 282:  // ln
                case 283:  // log
                case 284:  // logten
                case 285:  // mod
                case 286:  // multiply
                case 287:  // pow
                case 288:  // round
                case 289:  // sqrt
                case 290:  // subtract
                case 291:  // trunc
                case 301:  // setExpression
                case 302:  // allElementsTrue
                case 303:  // anyElementTrue
                case 304:  // setDifference
                case 305:  // setEquals
                case 306:  // setIntersection
                case 307:  // setIsSubset
                case 308:  // setUnion
                case 309:  // match
                case 310:  // predicates
                case 311:  // compoundMatchExprs
                case 312:  // predValue
                case 313:  // additionalExprs
                case 319:  // sortSpecs
                case 320:  // specList
                case 321:  // metaSort
                case 322:  // oneOrNegOne
                case 323:  // metaSortKeyword
                    value.template destroy<CNode>();
                    break;

                case 155:  // aggregationProjectionFieldname
                case 156:  // projectionFieldname
                case 157:  // expressionFieldname
                case 158:  // stageAsUserFieldname
                case 159:  // argAsUserFieldname
                case 160:  // argAsProjectionPath
                case 161:  // aggExprAsUserFieldname
                case 162:  // invariableUserFieldname
                case 163:  // idAsUserFieldname
                case 164:  // idAsProjectionPath
                case 165:  // valueFieldname
                case 166:  // predFieldname
                case 318:  // logicalExprField
                    value.template destroy<CNode::Fieldname>();
                    break;

                case 137:  // "Date"
                    value.template destroy<Date_t>();
                    break;

                case 147:  // "arbitrary decimal"
                    value.template destroy<Decimal128>();
                    break;

                case 136:  // "ObjectID"
                    value.template destroy<OID>();
                    break;

                case 148:  // "Timestamp"
                    value.template destroy<Timestamp>();
                    break;

                case 150:  // "maxKey"
                    value.template destroy<UserMaxKey>();
                    break;

                case 149:  // "minKey"
                    value.template destroy<UserMinKey>();
                    break;

                case 138:  // "null"
                    value.template destroy<UserNull>();
                    break;

                case 135:  // "undefined"
                    value.template destroy<UserUndefined>();
                    break;

                case 146:  // "arbitrary double"
                    value.template destroy<double>();
                    break;

                case 144:  // "arbitrary integer"
                    value.template destroy<int>();
                    break;

                case 145:  // "arbitrary long"
                    value.template destroy<long long>();
                    break;

                case 167:  // projectField
                case 168:  // projectionObjectField
                case 169:  // expressionField
                case 170:  // valueField
                case 292:  // onErrorArg
                case 293:  // onNullArg
                case 294:  // formatArg
                case 295:  // timezoneArg
                case 296:  // charsArg
                case 297:  // optionsArg
                case 314:  // predicate
                case 315:  // logicalExpr
                case 316:  // operatorExpression
                case 317:  // notExpr
                case 324:  // sortSpec
                    value.template destroy<std::pair<CNode::Fieldname, CNode>>();
                    break;

                case 128:  // "fieldname"
                case 130:  // "$-prefixed fieldname"
                case 131:  // "string"
                case 132:  // "$-prefixed string"
                case 133:  // "$$-prefixed string"
                case 171:  // arg
                    value.template destroy<std::string>();
                    break;

                case 298:  // expressions
                case 299:  // values
                case 300:  // exprZeroToTwo
                    value.template destroy<std::vector<CNode>>();
                    break;

                case 129:  // "fieldname containing dotted path"
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
                tok == token::ABS || tok == token::ADD || tok == token::ALL_ELEMENTS_TRUE ||
                tok == token::AND || tok == token::ANY_ELEMENT_TRUE || tok == token::ARG_CHARS ||
                tok == token::ARG_COLL || tok == token::ARG_DATE || tok == token::ARG_DATE_STRING ||
                tok == token::ARG_FILTER || tok == token::ARG_FIND || tok == token::ARG_FORMAT ||
                tok == token::ARG_INPUT || tok == token::ARG_ON_ERROR ||
                tok == token::ARG_ON_NULL || tok == token::ARG_OPTIONS ||
                tok == token::ARG_PIPELINE || tok == token::ARG_Q || tok == token::ARG_QUERY ||
                tok == token::ARG_REGEX || tok == token::ARG_REPLACEMENT ||
                tok == token::ARG_SIZE || tok == token::ARG_SORT || tok == token::ARG_TIMEZONE ||
                tok == token::ARG_TO || tok == token::ATAN2 || tok == token::BOOL_FALSE ||
                tok == token::BOOL_TRUE || tok == token::CEIL || tok == token::CMP ||
                tok == token::CONCAT || tok == token::CONST_EXPR || tok == token::CONVERT ||
                tok == token::DATE_FROM_STRING || tok == token::DATE_TO_STRING ||
                tok == token::DECIMAL_NEGATIVE_ONE || tok == token::DECIMAL_ONE ||
                tok == token::DECIMAL_ZERO || tok == token::DIVIDE ||
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
                tok == token::RAND_VAL || tok == token::RECORD_ID || tok == token::REGEX_FIND ||
                tok == token::REGEX_FIND_ALL || tok == token::REGEX_MATCH ||
                tok == token::REPLACE_ALL || tok == token::REPLACE_ONE || tok == token::ROUND ||
                tok == token::RTRIM || tok == token::SEARCH_HIGHLIGHTS ||
                tok == token::SEARCH_SCORE || tok == token::SET_DIFFERENCE ||
                tok == token::SET_EQUALS || tok == token::SET_INTERSECTION ||
                tok == token::SET_IS_SUBSET || tok == token::SET_UNION || tok == token::SLICE ||
                tok == token::SORT_KEY || tok == token::SPLIT || tok == token::SQRT ||
                tok == token::STAGE_INHIBIT_OPTIMIZATION || tok == token::STAGE_LIMIT ||
                tok == token::STAGE_PROJECT || tok == token::STAGE_SAMPLE ||
                tok == token::STAGE_SKIP || tok == token::STAGE_UNION_WITH ||
                tok == token::START_ARRAY || tok == token::START_OBJECT ||
                tok == token::STR_CASE_CMP || tok == token::STR_LEN_BYTES ||
                tok == token::STR_LEN_CP || tok == token::SUBSTR || tok == token::SUBSTR_BYTES ||
                tok == token::SUBSTR_CP || tok == token::SUBTRACT || tok == token::TEXT_SCORE ||
                tok == token::TO_BOOL || tok == token::TO_DATE || tok == token::TO_DECIMAL ||
                tok == token::TO_DOUBLE || tok == token::TO_INT || tok == token::TO_LONG ||
                tok == token::TO_LOWER || tok == token::TO_OBJECT_ID || tok == token::TO_STRING ||
                tok == token::TO_UPPER || tok == token::TRIM || tok == token::TRUNC ||
                tok == token::TYPE || tok == token::START_PIPELINE || tok == token::START_MATCH ||
                tok == token::START_SORT);
        }
#else
        symbol_type(int tok, const location_type& l) : super_type(token_type(tok), l) {
            YY_ASSERT(
                tok == token::END_OF_FILE || tok == token::YYerror || tok == token::YYUNDEF ||
                tok == token::ABS || tok == token::ADD || tok == token::ALL_ELEMENTS_TRUE ||
                tok == token::AND || tok == token::ANY_ELEMENT_TRUE || tok == token::ARG_CHARS ||
                tok == token::ARG_COLL || tok == token::ARG_DATE || tok == token::ARG_DATE_STRING ||
                tok == token::ARG_FILTER || tok == token::ARG_FIND || tok == token::ARG_FORMAT ||
                tok == token::ARG_INPUT || tok == token::ARG_ON_ERROR ||
                tok == token::ARG_ON_NULL || tok == token::ARG_OPTIONS ||
                tok == token::ARG_PIPELINE || tok == token::ARG_Q || tok == token::ARG_QUERY ||
                tok == token::ARG_REGEX || tok == token::ARG_REPLACEMENT ||
                tok == token::ARG_SIZE || tok == token::ARG_SORT || tok == token::ARG_TIMEZONE ||
                tok == token::ARG_TO || tok == token::ATAN2 || tok == token::BOOL_FALSE ||
                tok == token::BOOL_TRUE || tok == token::CEIL || tok == token::CMP ||
                tok == token::CONCAT || tok == token::CONST_EXPR || tok == token::CONVERT ||
                tok == token::DATE_FROM_STRING || tok == token::DATE_TO_STRING ||
                tok == token::DECIMAL_NEGATIVE_ONE || tok == token::DECIMAL_ONE ||
                tok == token::DECIMAL_ZERO || tok == token::DIVIDE ||
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
                tok == token::RAND_VAL || tok == token::RECORD_ID || tok == token::REGEX_FIND ||
                tok == token::REGEX_FIND_ALL || tok == token::REGEX_MATCH ||
                tok == token::REPLACE_ALL || tok == token::REPLACE_ONE || tok == token::ROUND ||
                tok == token::RTRIM || tok == token::SEARCH_HIGHLIGHTS ||
                tok == token::SEARCH_SCORE || tok == token::SET_DIFFERENCE ||
                tok == token::SET_EQUALS || tok == token::SET_INTERSECTION ||
                tok == token::SET_IS_SUBSET || tok == token::SET_UNION || tok == token::SLICE ||
                tok == token::SORT_KEY || tok == token::SPLIT || tok == token::SQRT ||
                tok == token::STAGE_INHIBIT_OPTIMIZATION || tok == token::STAGE_LIMIT ||
                tok == token::STAGE_PROJECT || tok == token::STAGE_SAMPLE ||
                tok == token::STAGE_SKIP || tok == token::STAGE_UNION_WITH ||
                tok == token::START_ARRAY || tok == token::START_OBJECT ||
                tok == token::STR_CASE_CMP || tok == token::STR_LEN_BYTES ||
                tok == token::STR_LEN_CP || tok == token::SUBSTR || tok == token::SUBSTR_BYTES ||
                tok == token::SUBSTR_CP || tok == token::SUBTRACT || tok == token::TEXT_SCORE ||
                tok == token::TO_BOOL || tok == token::TO_DATE || tok == token::TO_DECIMAL ||
                tok == token::TO_DOUBLE || tok == token::TO_INT || tok == token::TO_LONG ||
                tok == token::TO_LOWER || tok == token::TO_OBJECT_ID || tok == token::TO_STRING ||
                tok == token::TO_UPPER || tok == token::TRIM || tok == token::TRUNC ||
                tok == token::TYPE || tok == token::START_PIPELINE || tok == token::START_MATCH ||
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
    static symbol_type make_ARG_Q(location_type l) {
        return symbol_type(token::ARG_Q, std::move(l));
    }
#else
    static symbol_type make_ARG_Q(const location_type& l) {
        return symbol_type(token::ARG_Q, l);
    }
#endif
#if 201103L <= YY_CPLUSPLUS
    static symbol_type make_ARG_QUERY(location_type l) {
        return symbol_type(token::ARG_QUERY, std::move(l));
    }
#else
    static symbol_type make_ARG_QUERY(const location_type& l) {
        return symbol_type(token::ARG_QUERY, l);
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
    static symbol_type make_ARG_SORT(location_type l) {
        return symbol_type(token::ARG_SORT, std::move(l));
    }
#else
    static symbol_type make_ARG_SORT(const location_type& l) {
        return symbol_type(token::ARG_SORT, l);
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
    static symbol_type make_ATAN2(location_type l) {
        return symbol_type(token::ATAN2, std::move(l));
    }
#else
    static symbol_type make_ATAN2(const location_type& l) {
        return symbol_type(token::ATAN2, l);
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
        yylast_ = 1798,  ///< Last index in yytable_.
        yynnts_ = 174,   ///< Number of nonterminal symbols.
        yyfinal_ = 15    ///< Termination state number.
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
        case 134:  // "BinData"
            value.copy<BSONBinData>(YY_MOVE(that.value));
            break;

        case 141:  // "Code"
            value.copy<BSONCode>(YY_MOVE(that.value));
            break;

        case 143:  // "CodeWScope"
            value.copy<BSONCodeWScope>(YY_MOVE(that.value));
            break;

        case 140:  // "dbPointer"
            value.copy<BSONDBRef>(YY_MOVE(that.value));
            break;

        case 139:  // "regex"
            value.copy<BSONRegEx>(YY_MOVE(that.value));
            break;

        case 142:  // "Symbol"
            value.copy<BSONSymbol>(YY_MOVE(that.value));
            break;

        case 172:  // dbPointer
        case 173:  // javascript
        case 174:  // symbol
        case 175:  // javascriptWScope
        case 176:  // int
        case 177:  // timestamp
        case 178:  // long
        case 179:  // double
        case 180:  // decimal
        case 181:  // minKey
        case 182:  // maxKey
        case 183:  // value
        case 184:  // string
        case 185:  // aggregationFieldPath
        case 186:  // binary
        case 187:  // undefined
        case 188:  // objectId
        case 189:  // bool
        case 190:  // date
        case 191:  // null
        case 192:  // regex
        case 193:  // simpleValue
        case 194:  // compoundValue
        case 195:  // valueArray
        case 196:  // valueObject
        case 197:  // valueFields
        case 198:  // variable
        case 199:  // pipeline
        case 200:  // stageList
        case 201:  // stage
        case 202:  // inhibitOptimization
        case 203:  // unionWith
        case 204:  // skip
        case 205:  // limit
        case 206:  // project
        case 207:  // sample
        case 208:  // projectFields
        case 209:  // projectionObjectFields
        case 210:  // topLevelProjection
        case 211:  // projection
        case 212:  // projectionObject
        case 213:  // num
        case 214:  // expression
        case 215:  // compoundNonObjectExpression
        case 216:  // exprFixedTwoArg
        case 217:  // exprFixedThreeArg
        case 218:  // arrayManipulation
        case 219:  // slice
        case 220:  // expressionArray
        case 221:  // expressionObject
        case 222:  // expressionFields
        case 223:  // maths
        case 224:  // meta
        case 225:  // add
        case 226:  // atan2
        case 227:  // boolExprs
        case 228:  // and
        case 229:  // or
        case 230:  // not
        case 231:  // literalEscapes
        case 232:  // const
        case 233:  // literal
        case 234:  // stringExps
        case 235:  // concat
        case 236:  // dateFromString
        case 237:  // dateToString
        case 238:  // indexOfBytes
        case 239:  // indexOfCP
        case 240:  // ltrim
        case 241:  // regexFind
        case 242:  // regexFindAll
        case 243:  // regexMatch
        case 244:  // regexArgs
        case 245:  // replaceOne
        case 246:  // replaceAll
        case 247:  // rtrim
        case 248:  // split
        case 249:  // strLenBytes
        case 250:  // strLenCP
        case 251:  // strcasecmp
        case 252:  // substr
        case 253:  // substrBytes
        case 254:  // substrCP
        case 255:  // toLower
        case 256:  // toUpper
        case 257:  // trim
        case 258:  // compExprs
        case 259:  // cmp
        case 260:  // eq
        case 261:  // gt
        case 262:  // gte
        case 263:  // lt
        case 264:  // lte
        case 265:  // ne
        case 266:  // typeExpression
        case 267:  // convert
        case 268:  // toBool
        case 269:  // toDate
        case 270:  // toDecimal
        case 271:  // toDouble
        case 272:  // toInt
        case 273:  // toLong
        case 274:  // toObjectId
        case 275:  // toString
        case 276:  // type
        case 277:  // abs
        case 278:  // ceil
        case 279:  // divide
        case 280:  // exponent
        case 281:  // floor
        case 282:  // ln
        case 283:  // log
        case 284:  // logten
        case 285:  // mod
        case 286:  // multiply
        case 287:  // pow
        case 288:  // round
        case 289:  // sqrt
        case 290:  // subtract
        case 291:  // trunc
        case 301:  // setExpression
        case 302:  // allElementsTrue
        case 303:  // anyElementTrue
        case 304:  // setDifference
        case 305:  // setEquals
        case 306:  // setIntersection
        case 307:  // setIsSubset
        case 308:  // setUnion
        case 309:  // match
        case 310:  // predicates
        case 311:  // compoundMatchExprs
        case 312:  // predValue
        case 313:  // additionalExprs
        case 319:  // sortSpecs
        case 320:  // specList
        case 321:  // metaSort
        case 322:  // oneOrNegOne
        case 323:  // metaSortKeyword
            value.copy<CNode>(YY_MOVE(that.value));
            break;

        case 155:  // aggregationProjectionFieldname
        case 156:  // projectionFieldname
        case 157:  // expressionFieldname
        case 158:  // stageAsUserFieldname
        case 159:  // argAsUserFieldname
        case 160:  // argAsProjectionPath
        case 161:  // aggExprAsUserFieldname
        case 162:  // invariableUserFieldname
        case 163:  // idAsUserFieldname
        case 164:  // idAsProjectionPath
        case 165:  // valueFieldname
        case 166:  // predFieldname
        case 318:  // logicalExprField
            value.copy<CNode::Fieldname>(YY_MOVE(that.value));
            break;

        case 137:  // "Date"
            value.copy<Date_t>(YY_MOVE(that.value));
            break;

        case 147:  // "arbitrary decimal"
            value.copy<Decimal128>(YY_MOVE(that.value));
            break;

        case 136:  // "ObjectID"
            value.copy<OID>(YY_MOVE(that.value));
            break;

        case 148:  // "Timestamp"
            value.copy<Timestamp>(YY_MOVE(that.value));
            break;

        case 150:  // "maxKey"
            value.copy<UserMaxKey>(YY_MOVE(that.value));
            break;

        case 149:  // "minKey"
            value.copy<UserMinKey>(YY_MOVE(that.value));
            break;

        case 138:  // "null"
            value.copy<UserNull>(YY_MOVE(that.value));
            break;

        case 135:  // "undefined"
            value.copy<UserUndefined>(YY_MOVE(that.value));
            break;

        case 146:  // "arbitrary double"
            value.copy<double>(YY_MOVE(that.value));
            break;

        case 144:  // "arbitrary integer"
            value.copy<int>(YY_MOVE(that.value));
            break;

        case 145:  // "arbitrary long"
            value.copy<long long>(YY_MOVE(that.value));
            break;

        case 167:  // projectField
        case 168:  // projectionObjectField
        case 169:  // expressionField
        case 170:  // valueField
        case 292:  // onErrorArg
        case 293:  // onNullArg
        case 294:  // formatArg
        case 295:  // timezoneArg
        case 296:  // charsArg
        case 297:  // optionsArg
        case 314:  // predicate
        case 315:  // logicalExpr
        case 316:  // operatorExpression
        case 317:  // notExpr
        case 324:  // sortSpec
            value.copy<std::pair<CNode::Fieldname, CNode>>(YY_MOVE(that.value));
            break;

        case 128:  // "fieldname"
        case 130:  // "$-prefixed fieldname"
        case 131:  // "string"
        case 132:  // "$-prefixed string"
        case 133:  // "$$-prefixed string"
        case 171:  // arg
            value.copy<std::string>(YY_MOVE(that.value));
            break;

        case 298:  // expressions
        case 299:  // values
        case 300:  // exprZeroToTwo
            value.copy<std::vector<CNode>>(YY_MOVE(that.value));
            break;

        case 129:  // "fieldname containing dotted path"
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
        case 134:  // "BinData"
            value.move<BSONBinData>(YY_MOVE(s.value));
            break;

        case 141:  // "Code"
            value.move<BSONCode>(YY_MOVE(s.value));
            break;

        case 143:  // "CodeWScope"
            value.move<BSONCodeWScope>(YY_MOVE(s.value));
            break;

        case 140:  // "dbPointer"
            value.move<BSONDBRef>(YY_MOVE(s.value));
            break;

        case 139:  // "regex"
            value.move<BSONRegEx>(YY_MOVE(s.value));
            break;

        case 142:  // "Symbol"
            value.move<BSONSymbol>(YY_MOVE(s.value));
            break;

        case 172:  // dbPointer
        case 173:  // javascript
        case 174:  // symbol
        case 175:  // javascriptWScope
        case 176:  // int
        case 177:  // timestamp
        case 178:  // long
        case 179:  // double
        case 180:  // decimal
        case 181:  // minKey
        case 182:  // maxKey
        case 183:  // value
        case 184:  // string
        case 185:  // aggregationFieldPath
        case 186:  // binary
        case 187:  // undefined
        case 188:  // objectId
        case 189:  // bool
        case 190:  // date
        case 191:  // null
        case 192:  // regex
        case 193:  // simpleValue
        case 194:  // compoundValue
        case 195:  // valueArray
        case 196:  // valueObject
        case 197:  // valueFields
        case 198:  // variable
        case 199:  // pipeline
        case 200:  // stageList
        case 201:  // stage
        case 202:  // inhibitOptimization
        case 203:  // unionWith
        case 204:  // skip
        case 205:  // limit
        case 206:  // project
        case 207:  // sample
        case 208:  // projectFields
        case 209:  // projectionObjectFields
        case 210:  // topLevelProjection
        case 211:  // projection
        case 212:  // projectionObject
        case 213:  // num
        case 214:  // expression
        case 215:  // compoundNonObjectExpression
        case 216:  // exprFixedTwoArg
        case 217:  // exprFixedThreeArg
        case 218:  // arrayManipulation
        case 219:  // slice
        case 220:  // expressionArray
        case 221:  // expressionObject
        case 222:  // expressionFields
        case 223:  // maths
        case 224:  // meta
        case 225:  // add
        case 226:  // atan2
        case 227:  // boolExprs
        case 228:  // and
        case 229:  // or
        case 230:  // not
        case 231:  // literalEscapes
        case 232:  // const
        case 233:  // literal
        case 234:  // stringExps
        case 235:  // concat
        case 236:  // dateFromString
        case 237:  // dateToString
        case 238:  // indexOfBytes
        case 239:  // indexOfCP
        case 240:  // ltrim
        case 241:  // regexFind
        case 242:  // regexFindAll
        case 243:  // regexMatch
        case 244:  // regexArgs
        case 245:  // replaceOne
        case 246:  // replaceAll
        case 247:  // rtrim
        case 248:  // split
        case 249:  // strLenBytes
        case 250:  // strLenCP
        case 251:  // strcasecmp
        case 252:  // substr
        case 253:  // substrBytes
        case 254:  // substrCP
        case 255:  // toLower
        case 256:  // toUpper
        case 257:  // trim
        case 258:  // compExprs
        case 259:  // cmp
        case 260:  // eq
        case 261:  // gt
        case 262:  // gte
        case 263:  // lt
        case 264:  // lte
        case 265:  // ne
        case 266:  // typeExpression
        case 267:  // convert
        case 268:  // toBool
        case 269:  // toDate
        case 270:  // toDecimal
        case 271:  // toDouble
        case 272:  // toInt
        case 273:  // toLong
        case 274:  // toObjectId
        case 275:  // toString
        case 276:  // type
        case 277:  // abs
        case 278:  // ceil
        case 279:  // divide
        case 280:  // exponent
        case 281:  // floor
        case 282:  // ln
        case 283:  // log
        case 284:  // logten
        case 285:  // mod
        case 286:  // multiply
        case 287:  // pow
        case 288:  // round
        case 289:  // sqrt
        case 290:  // subtract
        case 291:  // trunc
        case 301:  // setExpression
        case 302:  // allElementsTrue
        case 303:  // anyElementTrue
        case 304:  // setDifference
        case 305:  // setEquals
        case 306:  // setIntersection
        case 307:  // setIsSubset
        case 308:  // setUnion
        case 309:  // match
        case 310:  // predicates
        case 311:  // compoundMatchExprs
        case 312:  // predValue
        case 313:  // additionalExprs
        case 319:  // sortSpecs
        case 320:  // specList
        case 321:  // metaSort
        case 322:  // oneOrNegOne
        case 323:  // metaSortKeyword
            value.move<CNode>(YY_MOVE(s.value));
            break;

        case 155:  // aggregationProjectionFieldname
        case 156:  // projectionFieldname
        case 157:  // expressionFieldname
        case 158:  // stageAsUserFieldname
        case 159:  // argAsUserFieldname
        case 160:  // argAsProjectionPath
        case 161:  // aggExprAsUserFieldname
        case 162:  // invariableUserFieldname
        case 163:  // idAsUserFieldname
        case 164:  // idAsProjectionPath
        case 165:  // valueFieldname
        case 166:  // predFieldname
        case 318:  // logicalExprField
            value.move<CNode::Fieldname>(YY_MOVE(s.value));
            break;

        case 137:  // "Date"
            value.move<Date_t>(YY_MOVE(s.value));
            break;

        case 147:  // "arbitrary decimal"
            value.move<Decimal128>(YY_MOVE(s.value));
            break;

        case 136:  // "ObjectID"
            value.move<OID>(YY_MOVE(s.value));
            break;

        case 148:  // "Timestamp"
            value.move<Timestamp>(YY_MOVE(s.value));
            break;

        case 150:  // "maxKey"
            value.move<UserMaxKey>(YY_MOVE(s.value));
            break;

        case 149:  // "minKey"
            value.move<UserMinKey>(YY_MOVE(s.value));
            break;

        case 138:  // "null"
            value.move<UserNull>(YY_MOVE(s.value));
            break;

        case 135:  // "undefined"
            value.move<UserUndefined>(YY_MOVE(s.value));
            break;

        case 146:  // "arbitrary double"
            value.move<double>(YY_MOVE(s.value));
            break;

        case 144:  // "arbitrary integer"
            value.move<int>(YY_MOVE(s.value));
            break;

        case 145:  // "arbitrary long"
            value.move<long long>(YY_MOVE(s.value));
            break;

        case 167:  // projectField
        case 168:  // projectionObjectField
        case 169:  // expressionField
        case 170:  // valueField
        case 292:  // onErrorArg
        case 293:  // onNullArg
        case 294:  // formatArg
        case 295:  // timezoneArg
        case 296:  // charsArg
        case 297:  // optionsArg
        case 314:  // predicate
        case 315:  // logicalExpr
        case 316:  // operatorExpression
        case 317:  // notExpr
        case 324:  // sortSpec
            value.move<std::pair<CNode::Fieldname, CNode>>(YY_MOVE(s.value));
            break;

        case 128:  // "fieldname"
        case 130:  // "$-prefixed fieldname"
        case 131:  // "string"
        case 132:  // "$-prefixed string"
        case 133:  // "$$-prefixed string"
        case 171:  // arg
            value.move<std::string>(YY_MOVE(s.value));
            break;

        case 298:  // expressions
        case 299:  // values
        case 300:  // exprZeroToTwo
            value.move<std::vector<CNode>>(YY_MOVE(s.value));
            break;

        case 129:  // "fieldname containing dotted path"
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
#line 5781 "parser_gen.hpp"


#endif  // !YY_YY_PARSER_GEN_HPP_INCLUDED
