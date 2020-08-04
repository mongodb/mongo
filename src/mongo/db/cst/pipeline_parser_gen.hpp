// A Bison parser, made by GNU Bison 3.6.

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

// DO NOT RELY ON FEATURES THAT ARE NOT DOCUMENTED in the manual,
// especially those whose name start with YY_ or yy_.  They are
// private implementation details that can be changed or removed.

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

#line 65 "src/mongo/db/cst/pipeline_parser_gen.hpp"

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
#line 200 "src/mongo/db/cst/pipeline_parser_gen.hpp"


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
            // dollarString
            // nonDollarString
            // stageList
            // stage
            // inhibitOptimization
            // unionWith
            // skip
            // limit
            // project
            // sample
            // unwind
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
            // typeExpression
            // typeValue
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
            // matchExpression
            // filterFields
            // filterVal
            char dummy7[sizeof(CNode)];

            // projectionFieldname
            // expressionFieldname
            // stageAsUserFieldname
            // filterFieldname
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
            // filterField
            // includeArrayIndexArg
            // preserveNullAndEmptyArraysArg
            // onErrorArg
            // onNullArg
            char dummy20[sizeof(std::pair<CNode::Fieldname, CNode>)];

            // FIELDNAME
            // NONEMPTY_STRING
            // "a $-prefixed string"
            // "an empty string"
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

    /// Token kinds.
    struct token {
        enum token_kind_type {
            YYEMPTY = -2,
            END_OF_FILE = 0,                          // "EOF"
            YYerror = 1,                              // error
            YYUNDEF = 2,                              // "invalid token"
            START_OBJECT = 3,                         // START_OBJECT
            END_OBJECT = 4,                           // END_OBJECT
            START_ARRAY = 5,                          // START_ARRAY
            END_ARRAY = 6,                            // END_ARRAY
            ID = 7,                                   // ID
            INT_ZERO = 8,                             // INT_ZERO
            LONG_ZERO = 9,                            // LONG_ZERO
            DOUBLE_ZERO = 10,                         // DOUBLE_ZERO
            DECIMAL_ZERO = 11,                        // DECIMAL_ZERO
            BOOL_TRUE = 12,                           // BOOL_TRUE
            BOOL_FALSE = 13,                          // BOOL_FALSE
            STAGE_INHIBIT_OPTIMIZATION = 14,          // STAGE_INHIBIT_OPTIMIZATION
            STAGE_LIMIT = 15,                         // STAGE_LIMIT
            STAGE_PROJECT = 16,                       // STAGE_PROJECT
            STAGE_SAMPLE = 17,                        // STAGE_SAMPLE
            STAGE_SKIP = 18,                          // STAGE_SKIP
            STAGE_UNION_WITH = 19,                    // STAGE_UNION_WITH
            STAGE_UNWIND = 20,                        // STAGE_UNWIND
            COLL_ARG = 21,                            // COLL_ARG
            PIPELINE_ARG = 22,                        // PIPELINE_ARG
            SIZE_ARG = 23,                            // SIZE_ARG
            PATH_ARG = 24,                            // PATH_ARG
            INCLUDE_ARRAY_INDEX_ARG = 25,             // INCLUDE_ARRAY_INDEX_ARG
            PRESERVE_NULL_AND_EMPTY_ARRAYS_ARG = 26,  // PRESERVE_NULL_AND_EMPTY_ARRAYS_ARG
            ADD = 27,                                 // ADD
            ATAN2 = 28,                               // ATAN2
            AND = 29,                                 // AND
            CONST_EXPR = 30,                          // CONST_EXPR
            LITERAL = 31,                             // LITERAL
            OR = 32,                                  // OR
            NOT = 33,                                 // NOT
            CMP = 34,                                 // CMP
            EQ = 35,                                  // EQ
            GT = 36,                                  // GT
            GTE = 37,                                 // GTE
            LT = 38,                                  // LT
            LTE = 39,                                 // LTE
            NE = 40,                                  // NE
            CONVERT = 41,                             // CONVERT
            TO_BOOL = 42,                             // TO_BOOL
            TO_DATE = 43,                             // TO_DATE
            TO_DECIMAL = 44,                          // TO_DECIMAL
            TO_DOUBLE = 45,                           // TO_DOUBLE
            TO_INT = 46,                              // TO_INT
            TO_LONG = 47,                             // TO_LONG
            TO_OBJECT_ID = 48,                        // TO_OBJECT_ID
            TO_STRING = 49,                           // TO_STRING
            TYPE = 50,                                // TYPE
            ABS = 51,                                 // ABS
            CEIL = 52,                                // CEIL
            DIVIDE = 53,                              // DIVIDE
            EXPONENT = 54,                            // EXPONENT
            FLOOR = 55,                               // FLOOR
            LN = 56,                                  // LN
            LOG = 57,                                 // LOG
            LOGTEN = 58,                              // LOGTEN
            MOD = 59,                                 // MOD
            MULTIPLY = 60,                            // MULTIPLY
            POW = 61,                                 // POW
            ROUND = 62,                               // ROUND
            SQRT = 63,                                // SQRT
            SUBTRACT = 64,                            // SUBTRACT
            TRUNC = 65,                               // TRUNC
            INPUT_ARG = 66,                           // INPUT_ARG
            TO_ARG = 67,                              // TO_ARG
            ON_ERROR_ARG = 68,                        // ON_ERROR_ARG
            ON_NULL_ARG = 69,                         // ON_NULL_ARG
            FIELDNAME = 70,                           // FIELDNAME
            NONEMPTY_STRING = 71,                     // NONEMPTY_STRING
            BINARY = 72,                              // BINARY
            UNDEFINED = 73,                           // UNDEFINED
            OBJECT_ID = 74,                           // OBJECT_ID
            DATE_LITERAL = 75,                        // DATE_LITERAL
            JSNULL = 76,                              // JSNULL
            REGEX = 77,                               // REGEX
            DB_POINTER = 78,                          // DB_POINTER
            JAVASCRIPT = 79,                          // JAVASCRIPT
            SYMBOL = 80,                              // SYMBOL
            JAVASCRIPT_W_SCOPE = 81,                  // JAVASCRIPT_W_SCOPE
            INT_NON_ZERO = 82,                        // INT_NON_ZERO
            TIMESTAMP = 83,                           // TIMESTAMP
            LONG_NON_ZERO = 84,                       // LONG_NON_ZERO
            DOUBLE_NON_ZERO = 85,                     // DOUBLE_NON_ZERO
            DECIMAL_NON_ZERO = 86,                    // DECIMAL_NON_ZERO
            MIN_KEY = 87,                             // MIN_KEY
            MAX_KEY = 88,                             // MAX_KEY
            START_PIPELINE = 89,                      // START_PIPELINE
            START_MATCH = 90,                         // START_MATCH
            DOLLAR_STRING = 91,                       // "a $-prefixed string"
            EMPTY_STRING = 92                         // "an empty string"
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
            YYNTOKENS = 93,  ///< Number of tokens.
            S_YYEMPTY = -2,
            S_YYEOF = 0,                                // "EOF"
            S_YYerror = 1,                              // error
            S_YYUNDEF = 2,                              // "invalid token"
            S_START_OBJECT = 3,                         // START_OBJECT
            S_END_OBJECT = 4,                           // END_OBJECT
            S_START_ARRAY = 5,                          // START_ARRAY
            S_END_ARRAY = 6,                            // END_ARRAY
            S_ID = 7,                                   // ID
            S_INT_ZERO = 8,                             // INT_ZERO
            S_LONG_ZERO = 9,                            // LONG_ZERO
            S_DOUBLE_ZERO = 10,                         // DOUBLE_ZERO
            S_DECIMAL_ZERO = 11,                        // DECIMAL_ZERO
            S_BOOL_TRUE = 12,                           // BOOL_TRUE
            S_BOOL_FALSE = 13,                          // BOOL_FALSE
            S_STAGE_INHIBIT_OPTIMIZATION = 14,          // STAGE_INHIBIT_OPTIMIZATION
            S_STAGE_LIMIT = 15,                         // STAGE_LIMIT
            S_STAGE_PROJECT = 16,                       // STAGE_PROJECT
            S_STAGE_SAMPLE = 17,                        // STAGE_SAMPLE
            S_STAGE_SKIP = 18,                          // STAGE_SKIP
            S_STAGE_UNION_WITH = 19,                    // STAGE_UNION_WITH
            S_STAGE_UNWIND = 20,                        // STAGE_UNWIND
            S_COLL_ARG = 21,                            // COLL_ARG
            S_PIPELINE_ARG = 22,                        // PIPELINE_ARG
            S_SIZE_ARG = 23,                            // SIZE_ARG
            S_PATH_ARG = 24,                            // PATH_ARG
            S_INCLUDE_ARRAY_INDEX_ARG = 25,             // INCLUDE_ARRAY_INDEX_ARG
            S_PRESERVE_NULL_AND_EMPTY_ARRAYS_ARG = 26,  // PRESERVE_NULL_AND_EMPTY_ARRAYS_ARG
            S_ADD = 27,                                 // ADD
            S_ATAN2 = 28,                               // ATAN2
            S_AND = 29,                                 // AND
            S_CONST_EXPR = 30,                          // CONST_EXPR
            S_LITERAL = 31,                             // LITERAL
            S_OR = 32,                                  // OR
            S_NOT = 33,                                 // NOT
            S_CMP = 34,                                 // CMP
            S_EQ = 35,                                  // EQ
            S_GT = 36,                                  // GT
            S_GTE = 37,                                 // GTE
            S_LT = 38,                                  // LT
            S_LTE = 39,                                 // LTE
            S_NE = 40,                                  // NE
            S_CONVERT = 41,                             // CONVERT
            S_TO_BOOL = 42,                             // TO_BOOL
            S_TO_DATE = 43,                             // TO_DATE
            S_TO_DECIMAL = 44,                          // TO_DECIMAL
            S_TO_DOUBLE = 45,                           // TO_DOUBLE
            S_TO_INT = 46,                              // TO_INT
            S_TO_LONG = 47,                             // TO_LONG
            S_TO_OBJECT_ID = 48,                        // TO_OBJECT_ID
            S_TO_STRING = 49,                           // TO_STRING
            S_TYPE = 50,                                // TYPE
            S_ABS = 51,                                 // ABS
            S_CEIL = 52,                                // CEIL
            S_DIVIDE = 53,                              // DIVIDE
            S_EXPONENT = 54,                            // EXPONENT
            S_FLOOR = 55,                               // FLOOR
            S_LN = 56,                                  // LN
            S_LOG = 57,                                 // LOG
            S_LOGTEN = 58,                              // LOGTEN
            S_MOD = 59,                                 // MOD
            S_MULTIPLY = 60,                            // MULTIPLY
            S_POW = 61,                                 // POW
            S_ROUND = 62,                               // ROUND
            S_SQRT = 63,                                // SQRT
            S_SUBTRACT = 64,                            // SUBTRACT
            S_TRUNC = 65,                               // TRUNC
            S_INPUT_ARG = 66,                           // INPUT_ARG
            S_TO_ARG = 67,                              // TO_ARG
            S_ON_ERROR_ARG = 68,                        // ON_ERROR_ARG
            S_ON_NULL_ARG = 69,                         // ON_NULL_ARG
            S_FIELDNAME = 70,                           // FIELDNAME
            S_NONEMPTY_STRING = 71,                     // NONEMPTY_STRING
            S_BINARY = 72,                              // BINARY
            S_UNDEFINED = 73,                           // UNDEFINED
            S_OBJECT_ID = 74,                           // OBJECT_ID
            S_DATE_LITERAL = 75,                        // DATE_LITERAL
            S_JSNULL = 76,                              // JSNULL
            S_REGEX = 77,                               // REGEX
            S_DB_POINTER = 78,                          // DB_POINTER
            S_JAVASCRIPT = 79,                          // JAVASCRIPT
            S_SYMBOL = 80,                              // SYMBOL
            S_JAVASCRIPT_W_SCOPE = 81,                  // JAVASCRIPT_W_SCOPE
            S_INT_NON_ZERO = 82,                        // INT_NON_ZERO
            S_TIMESTAMP = 83,                           // TIMESTAMP
            S_LONG_NON_ZERO = 84,                       // LONG_NON_ZERO
            S_DOUBLE_NON_ZERO = 85,                     // DOUBLE_NON_ZERO
            S_DECIMAL_NON_ZERO = 86,                    // DECIMAL_NON_ZERO
            S_MIN_KEY = 87,                             // MIN_KEY
            S_MAX_KEY = 88,                             // MAX_KEY
            S_START_PIPELINE = 89,                      // START_PIPELINE
            S_START_MATCH = 90,                         // START_MATCH
            S_DOLLAR_STRING = 91,                       // "a $-prefixed string"
            S_EMPTY_STRING = 92,                        // "an empty string"
            S_YYACCEPT = 93,                            // $accept
            S_projectionFieldname = 94,                 // projectionFieldname
            S_expressionFieldname = 95,                 // expressionFieldname
            S_stageAsUserFieldname = 96,                // stageAsUserFieldname
            S_filterFieldname = 97,                     // filterFieldname
            S_argAsUserFieldname = 98,                  // argAsUserFieldname
            S_aggExprAsUserFieldname = 99,              // aggExprAsUserFieldname
            S_invariableUserFieldname = 100,            // invariableUserFieldname
            S_idAsUserFieldname = 101,                  // idAsUserFieldname
            S_valueFieldname = 102,                     // valueFieldname
            S_projectField = 103,                       // projectField
            S_expressionField = 104,                    // expressionField
            S_valueField = 105,                         // valueField
            S_filterField = 106,                        // filterField
            S_dbPointer = 107,                          // dbPointer
            S_javascript = 108,                         // javascript
            S_symbol = 109,                             // symbol
            S_javascriptWScope = 110,                   // javascriptWScope
            S_int = 111,                                // int
            S_timestamp = 112,                          // timestamp
            S_long = 113,                               // long
            S_double = 114,                             // double
            S_decimal = 115,                            // decimal
            S_minKey = 116,                             // minKey
            S_maxKey = 117,                             // maxKey
            S_value = 118,                              // value
            S_string = 119,                             // string
            S_binary = 120,                             // binary
            S_undefined = 121,                          // undefined
            S_objectId = 122,                           // objectId
            S_bool = 123,                               // bool
            S_date = 124,                               // date
            S_null = 125,                               // null
            S_regex = 126,                              // regex
            S_simpleValue = 127,                        // simpleValue
            S_compoundValue = 128,                      // compoundValue
            S_valueArray = 129,                         // valueArray
            S_valueObject = 130,                        // valueObject
            S_valueFields = 131,                        // valueFields
            S_dollarString = 132,                       // dollarString
            S_nonDollarString = 133,                    // nonDollarString
            S_stageList = 134,                          // stageList
            S_stage = 135,                              // stage
            S_inhibitOptimization = 136,                // inhibitOptimization
            S_unionWith = 137,                          // unionWith
            S_skip = 138,                               // skip
            S_limit = 139,                              // limit
            S_project = 140,                            // project
            S_sample = 141,                             // sample
            S_unwind = 142,                             // unwind
            S_projectFields = 143,                      // projectFields
            S_projection = 144,                         // projection
            S_num = 145,                                // num
            S_includeArrayIndexArg = 146,               // includeArrayIndexArg
            S_preserveNullAndEmptyArraysArg = 147,      // preserveNullAndEmptyArraysArg
            S_expression = 148,                         // expression
            S_compoundExpression = 149,                 // compoundExpression
            S_exprFixedTwoArg = 150,                    // exprFixedTwoArg
            S_expressionArray = 151,                    // expressionArray
            S_expressionObject = 152,                   // expressionObject
            S_expressionFields = 153,                   // expressionFields
            S_maths = 154,                              // maths
            S_add = 155,                                // add
            S_atan2 = 156,                              // atan2
            S_boolExps = 157,                           // boolExps
            S_and = 158,                                // and
            S_or = 159,                                 // or
            S_not = 160,                                // not
            S_literalEscapes = 161,                     // literalEscapes
            S_const = 162,                              // const
            S_literal = 163,                            // literal
            S_compExprs = 164,                          // compExprs
            S_cmp = 165,                                // cmp
            S_eq = 166,                                 // eq
            S_gt = 167,                                 // gt
            S_gte = 168,                                // gte
            S_lt = 169,                                 // lt
            S_lte = 170,                                // lte
            S_ne = 171,                                 // ne
            S_typeExpression = 172,                     // typeExpression
            S_typeValue = 173,                          // typeValue
            S_convert = 174,                            // convert
            S_toBool = 175,                             // toBool
            S_toDate = 176,                             // toDate
            S_toDecimal = 177,                          // toDecimal
            S_toDouble = 178,                           // toDouble
            S_toInt = 179,                              // toInt
            S_toLong = 180,                             // toLong
            S_toObjectId = 181,                         // toObjectId
            S_toString = 182,                           // toString
            S_type = 183,                               // type
            S_abs = 184,                                // abs
            S_ceil = 185,                               // ceil
            S_divide = 186,                             // divide
            S_exponent = 187,                           // exponent
            S_floor = 188,                              // floor
            S_ln = 189,                                 // ln
            S_log = 190,                                // log
            S_logten = 191,                             // logten
            S_mod = 192,                                // mod
            S_multiply = 193,                           // multiply
            S_pow = 194,                                // pow
            S_round = 195,                              // round
            S_sqrt = 196,                               // sqrt
            S_subtract = 197,                           // subtract
            S_trunc = 198,                              // trunc
            S_onErrorArg = 199,                         // onErrorArg
            S_onNullArg = 200,                          // onNullArg
            S_expressions = 201,                        // expressions
            S_values = 202,                             // values
            S_matchExpression = 203,                    // matchExpression
            S_filterFields = 204,                       // filterFields
            S_filterVal = 205,                          // filterVal
            S_start = 206,                              // start
            S_pipeline = 207,                           // pipeline
            S_START_ORDERED_OBJECT = 208,               // START_ORDERED_OBJECT
            S_209_1 = 209                               // $@1
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
                case 72:  // BINARY
                    value.move<BSONBinData>(std::move(that.value));
                    break;

                case 79:  // JAVASCRIPT
                    value.move<BSONCode>(std::move(that.value));
                    break;

                case 81:  // JAVASCRIPT_W_SCOPE
                    value.move<BSONCodeWScope>(std::move(that.value));
                    break;

                case 78:  // DB_POINTER
                    value.move<BSONDBRef>(std::move(that.value));
                    break;

                case 77:  // REGEX
                    value.move<BSONRegEx>(std::move(that.value));
                    break;

                case 80:  // SYMBOL
                    value.move<BSONSymbol>(std::move(that.value));
                    break;

                case 107:  // dbPointer
                case 108:  // javascript
                case 109:  // symbol
                case 110:  // javascriptWScope
                case 111:  // int
                case 112:  // timestamp
                case 113:  // long
                case 114:  // double
                case 115:  // decimal
                case 116:  // minKey
                case 117:  // maxKey
                case 118:  // value
                case 119:  // string
                case 120:  // binary
                case 121:  // undefined
                case 122:  // objectId
                case 123:  // bool
                case 124:  // date
                case 125:  // null
                case 126:  // regex
                case 127:  // simpleValue
                case 128:  // compoundValue
                case 129:  // valueArray
                case 130:  // valueObject
                case 131:  // valueFields
                case 132:  // dollarString
                case 133:  // nonDollarString
                case 134:  // stageList
                case 135:  // stage
                case 136:  // inhibitOptimization
                case 137:  // unionWith
                case 138:  // skip
                case 139:  // limit
                case 140:  // project
                case 141:  // sample
                case 142:  // unwind
                case 143:  // projectFields
                case 144:  // projection
                case 145:  // num
                case 148:  // expression
                case 149:  // compoundExpression
                case 150:  // exprFixedTwoArg
                case 151:  // expressionArray
                case 152:  // expressionObject
                case 153:  // expressionFields
                case 154:  // maths
                case 155:  // add
                case 156:  // atan2
                case 157:  // boolExps
                case 158:  // and
                case 159:  // or
                case 160:  // not
                case 161:  // literalEscapes
                case 162:  // const
                case 163:  // literal
                case 164:  // compExprs
                case 165:  // cmp
                case 166:  // eq
                case 167:  // gt
                case 168:  // gte
                case 169:  // lt
                case 170:  // lte
                case 171:  // ne
                case 172:  // typeExpression
                case 173:  // typeValue
                case 174:  // convert
                case 175:  // toBool
                case 176:  // toDate
                case 177:  // toDecimal
                case 178:  // toDouble
                case 179:  // toInt
                case 180:  // toLong
                case 181:  // toObjectId
                case 182:  // toString
                case 183:  // type
                case 184:  // abs
                case 185:  // ceil
                case 186:  // divide
                case 187:  // exponent
                case 188:  // floor
                case 189:  // ln
                case 190:  // log
                case 191:  // logten
                case 192:  // mod
                case 193:  // multiply
                case 194:  // pow
                case 195:  // round
                case 196:  // sqrt
                case 197:  // subtract
                case 198:  // trunc
                case 203:  // matchExpression
                case 204:  // filterFields
                case 205:  // filterVal
                    value.move<CNode>(std::move(that.value));
                    break;

                case 94:   // projectionFieldname
                case 95:   // expressionFieldname
                case 96:   // stageAsUserFieldname
                case 97:   // filterFieldname
                case 98:   // argAsUserFieldname
                case 99:   // aggExprAsUserFieldname
                case 100:  // invariableUserFieldname
                case 101:  // idAsUserFieldname
                case 102:  // valueFieldname
                    value.move<CNode::Fieldname>(std::move(that.value));
                    break;

                case 75:  // DATE_LITERAL
                    value.move<Date_t>(std::move(that.value));
                    break;

                case 86:  // DECIMAL_NON_ZERO
                    value.move<Decimal128>(std::move(that.value));
                    break;

                case 74:  // OBJECT_ID
                    value.move<OID>(std::move(that.value));
                    break;

                case 83:  // TIMESTAMP
                    value.move<Timestamp>(std::move(that.value));
                    break;

                case 88:  // MAX_KEY
                    value.move<UserMaxKey>(std::move(that.value));
                    break;

                case 87:  // MIN_KEY
                    value.move<UserMinKey>(std::move(that.value));
                    break;

                case 76:  // JSNULL
                    value.move<UserNull>(std::move(that.value));
                    break;

                case 73:  // UNDEFINED
                    value.move<UserUndefined>(std::move(that.value));
                    break;

                case 85:  // DOUBLE_NON_ZERO
                    value.move<double>(std::move(that.value));
                    break;

                case 82:  // INT_NON_ZERO
                    value.move<int>(std::move(that.value));
                    break;

                case 84:  // LONG_NON_ZERO
                    value.move<long long>(std::move(that.value));
                    break;

                case 103:  // projectField
                case 104:  // expressionField
                case 105:  // valueField
                case 106:  // filterField
                case 146:  // includeArrayIndexArg
                case 147:  // preserveNullAndEmptyArraysArg
                case 199:  // onErrorArg
                case 200:  // onNullArg
                    value.move<std::pair<CNode::Fieldname, CNode>>(std::move(that.value));
                    break;

                case 70:  // FIELDNAME
                case 71:  // NONEMPTY_STRING
                case 91:  // "a $-prefixed string"
                case 92:  // "an empty string"
                    value.move<std::string>(std::move(that.value));
                    break;

                case 201:  // expressions
                case 202:  // values
                    value.move<std::vector<CNode>>(std::move(that.value));
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
                case 72:  // BINARY
                    value.template destroy<BSONBinData>();
                    break;

                case 79:  // JAVASCRIPT
                    value.template destroy<BSONCode>();
                    break;

                case 81:  // JAVASCRIPT_W_SCOPE
                    value.template destroy<BSONCodeWScope>();
                    break;

                case 78:  // DB_POINTER
                    value.template destroy<BSONDBRef>();
                    break;

                case 77:  // REGEX
                    value.template destroy<BSONRegEx>();
                    break;

                case 80:  // SYMBOL
                    value.template destroy<BSONSymbol>();
                    break;

                case 107:  // dbPointer
                case 108:  // javascript
                case 109:  // symbol
                case 110:  // javascriptWScope
                case 111:  // int
                case 112:  // timestamp
                case 113:  // long
                case 114:  // double
                case 115:  // decimal
                case 116:  // minKey
                case 117:  // maxKey
                case 118:  // value
                case 119:  // string
                case 120:  // binary
                case 121:  // undefined
                case 122:  // objectId
                case 123:  // bool
                case 124:  // date
                case 125:  // null
                case 126:  // regex
                case 127:  // simpleValue
                case 128:  // compoundValue
                case 129:  // valueArray
                case 130:  // valueObject
                case 131:  // valueFields
                case 132:  // dollarString
                case 133:  // nonDollarString
                case 134:  // stageList
                case 135:  // stage
                case 136:  // inhibitOptimization
                case 137:  // unionWith
                case 138:  // skip
                case 139:  // limit
                case 140:  // project
                case 141:  // sample
                case 142:  // unwind
                case 143:  // projectFields
                case 144:  // projection
                case 145:  // num
                case 148:  // expression
                case 149:  // compoundExpression
                case 150:  // exprFixedTwoArg
                case 151:  // expressionArray
                case 152:  // expressionObject
                case 153:  // expressionFields
                case 154:  // maths
                case 155:  // add
                case 156:  // atan2
                case 157:  // boolExps
                case 158:  // and
                case 159:  // or
                case 160:  // not
                case 161:  // literalEscapes
                case 162:  // const
                case 163:  // literal
                case 164:  // compExprs
                case 165:  // cmp
                case 166:  // eq
                case 167:  // gt
                case 168:  // gte
                case 169:  // lt
                case 170:  // lte
                case 171:  // ne
                case 172:  // typeExpression
                case 173:  // typeValue
                case 174:  // convert
                case 175:  // toBool
                case 176:  // toDate
                case 177:  // toDecimal
                case 178:  // toDouble
                case 179:  // toInt
                case 180:  // toLong
                case 181:  // toObjectId
                case 182:  // toString
                case 183:  // type
                case 184:  // abs
                case 185:  // ceil
                case 186:  // divide
                case 187:  // exponent
                case 188:  // floor
                case 189:  // ln
                case 190:  // log
                case 191:  // logten
                case 192:  // mod
                case 193:  // multiply
                case 194:  // pow
                case 195:  // round
                case 196:  // sqrt
                case 197:  // subtract
                case 198:  // trunc
                case 203:  // matchExpression
                case 204:  // filterFields
                case 205:  // filterVal
                    value.template destroy<CNode>();
                    break;

                case 94:   // projectionFieldname
                case 95:   // expressionFieldname
                case 96:   // stageAsUserFieldname
                case 97:   // filterFieldname
                case 98:   // argAsUserFieldname
                case 99:   // aggExprAsUserFieldname
                case 100:  // invariableUserFieldname
                case 101:  // idAsUserFieldname
                case 102:  // valueFieldname
                    value.template destroy<CNode::Fieldname>();
                    break;

                case 75:  // DATE_LITERAL
                    value.template destroy<Date_t>();
                    break;

                case 86:  // DECIMAL_NON_ZERO
                    value.template destroy<Decimal128>();
                    break;

                case 74:  // OBJECT_ID
                    value.template destroy<OID>();
                    break;

                case 83:  // TIMESTAMP
                    value.template destroy<Timestamp>();
                    break;

                case 88:  // MAX_KEY
                    value.template destroy<UserMaxKey>();
                    break;

                case 87:  // MIN_KEY
                    value.template destroy<UserMinKey>();
                    break;

                case 76:  // JSNULL
                    value.template destroy<UserNull>();
                    break;

                case 73:  // UNDEFINED
                    value.template destroy<UserUndefined>();
                    break;

                case 85:  // DOUBLE_NON_ZERO
                    value.template destroy<double>();
                    break;

                case 82:  // INT_NON_ZERO
                    value.template destroy<int>();
                    break;

                case 84:  // LONG_NON_ZERO
                    value.template destroy<long long>();
                    break;

                case 103:  // projectField
                case 104:  // expressionField
                case 105:  // valueField
                case 106:  // filterField
                case 146:  // includeArrayIndexArg
                case 147:  // preserveNullAndEmptyArraysArg
                case 199:  // onErrorArg
                case 200:  // onNullArg
                    value.template destroy<std::pair<CNode::Fieldname, CNode>>();
                    break;

                case 70:  // FIELDNAME
                case 71:  // NONEMPTY_STRING
                case 91:  // "a $-prefixed string"
                case 92:  // "an empty string"
                    value.template destroy<std::string>();
                    break;

                case 201:  // expressions
                case 202:  // values
                    value.template destroy<std::vector<CNode>>();
                    break;

                default:
                    break;
            }

            Base::clear();
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
                tok == token::START_OBJECT || tok == token::END_OBJECT ||
                tok == token::START_ARRAY || tok == token::END_ARRAY || tok == token::ID ||
                tok == token::INT_ZERO || tok == token::LONG_ZERO || tok == token::DOUBLE_ZERO ||
                tok == token::DECIMAL_ZERO || tok == token::BOOL_TRUE || tok == token::BOOL_FALSE ||
                tok == token::STAGE_INHIBIT_OPTIMIZATION || tok == token::STAGE_LIMIT ||
                tok == token::STAGE_PROJECT || tok == token::STAGE_SAMPLE ||
                tok == token::STAGE_SKIP || tok == token::STAGE_UNION_WITH ||
                tok == token::STAGE_UNWIND || tok == token::COLL_ARG ||
                tok == token::PIPELINE_ARG || tok == token::SIZE_ARG || tok == token::PATH_ARG ||
                tok == token::INCLUDE_ARRAY_INDEX_ARG ||
                tok == token::PRESERVE_NULL_AND_EMPTY_ARRAYS_ARG || tok == token::ADD ||
                tok == token::ATAN2 || tok == token::AND || tok == token::CONST_EXPR ||
                tok == token::LITERAL || tok == token::OR || tok == token::NOT ||
                tok == token::CMP || tok == token::EQ || tok == token::GT || tok == token::GTE ||
                tok == token::LT || tok == token::LTE || tok == token::NE ||
                tok == token::CONVERT || tok == token::TO_BOOL || tok == token::TO_DATE ||
                tok == token::TO_DECIMAL || tok == token::TO_DOUBLE || tok == token::TO_INT ||
                tok == token::TO_LONG || tok == token::TO_OBJECT_ID || tok == token::TO_STRING ||
                tok == token::TYPE || tok == token::ABS || tok == token::CEIL ||
                tok == token::DIVIDE || tok == token::EXPONENT || tok == token::FLOOR ||
                tok == token::LN || tok == token::LOG || tok == token::LOGTEN ||
                tok == token::MOD || tok == token::MULTIPLY || tok == token::POW ||
                tok == token::ROUND || tok == token::SQRT || tok == token::SUBTRACT ||
                tok == token::TRUNC || tok == token::INPUT_ARG || tok == token::TO_ARG ||
                tok == token::ON_ERROR_ARG || tok == token::ON_NULL_ARG ||
                tok == token::START_PIPELINE || tok == token::START_MATCH);
        }
#else
        symbol_type(int tok, const location_type& l) : super_type(token_type(tok), l) {
            YY_ASSERT(
                tok == token::END_OF_FILE || tok == token::YYerror || tok == token::YYUNDEF ||
                tok == token::START_OBJECT || tok == token::END_OBJECT ||
                tok == token::START_ARRAY || tok == token::END_ARRAY || tok == token::ID ||
                tok == token::INT_ZERO || tok == token::LONG_ZERO || tok == token::DOUBLE_ZERO ||
                tok == token::DECIMAL_ZERO || tok == token::BOOL_TRUE || tok == token::BOOL_FALSE ||
                tok == token::STAGE_INHIBIT_OPTIMIZATION || tok == token::STAGE_LIMIT ||
                tok == token::STAGE_PROJECT || tok == token::STAGE_SAMPLE ||
                tok == token::STAGE_SKIP || tok == token::STAGE_UNION_WITH ||
                tok == token::STAGE_UNWIND || tok == token::COLL_ARG ||
                tok == token::PIPELINE_ARG || tok == token::SIZE_ARG || tok == token::PATH_ARG ||
                tok == token::INCLUDE_ARRAY_INDEX_ARG ||
                tok == token::PRESERVE_NULL_AND_EMPTY_ARRAYS_ARG || tok == token::ADD ||
                tok == token::ATAN2 || tok == token::AND || tok == token::CONST_EXPR ||
                tok == token::LITERAL || tok == token::OR || tok == token::NOT ||
                tok == token::CMP || tok == token::EQ || tok == token::GT || tok == token::GTE ||
                tok == token::LT || tok == token::LTE || tok == token::NE ||
                tok == token::CONVERT || tok == token::TO_BOOL || tok == token::TO_DATE ||
                tok == token::TO_DECIMAL || tok == token::TO_DOUBLE || tok == token::TO_INT ||
                tok == token::TO_LONG || tok == token::TO_OBJECT_ID || tok == token::TO_STRING ||
                tok == token::TYPE || tok == token::ABS || tok == token::CEIL ||
                tok == token::DIVIDE || tok == token::EXPONENT || tok == token::FLOOR ||
                tok == token::LN || tok == token::LOG || tok == token::LOGTEN ||
                tok == token::MOD || tok == token::MULTIPLY || tok == token::POW ||
                tok == token::ROUND || tok == token::SQRT || tok == token::SUBTRACT ||
                tok == token::TRUNC || tok == token::INPUT_ARG || tok == token::TO_ARG ||
                tok == token::ON_ERROR_ARG || tok == token::ON_NULL_ARG ||
                tok == token::START_PIPELINE || tok == token::START_MATCH);
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
            YY_ASSERT(tok == token::FIELDNAME || tok == token::NONEMPTY_STRING ||
                      tok == token::DOLLAR_STRING || tok == token::EMPTY_STRING);
        }
#else
        symbol_type(int tok, const std::string& v, const location_type& l)
            : super_type(token_type(tok), v, l) {
            YY_ASSERT(tok == token::FIELDNAME || tok == token::NONEMPTY_STRING ||
                      tok == token::DOLLAR_STRING || tok == token::EMPTY_STRING);
        }
#endif
    };

    /// Build a parser object.
    PipelineParserGen(BSONLexer& lexer_yyarg, CNode* cst_yyarg);
    virtual ~PipelineParserGen();

#if 201103L <= YY_CPLUSPLUS
    /// Non copyable.
    PipelineParserGen(const PipelineParserGen&) = delete;
    /// Non copyable.
    PipelineParserGen& operator=(const PipelineParserGen&) = delete;
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
    static symbol_type make_STAGE_UNWIND(location_type l) {
        return symbol_type(token::STAGE_UNWIND, std::move(l));
    }
#else
    static symbol_type make_STAGE_UNWIND(const location_type& l) {
        return symbol_type(token::STAGE_UNWIND, l);
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
    static symbol_type make_PATH_ARG(location_type l) {
        return symbol_type(token::PATH_ARG, std::move(l));
    }
#else
    static symbol_type make_PATH_ARG(const location_type& l) {
        return symbol_type(token::PATH_ARG, l);
    }
#endif
#if 201103L <= YY_CPLUSPLUS
    static symbol_type make_INCLUDE_ARRAY_INDEX_ARG(location_type l) {
        return symbol_type(token::INCLUDE_ARRAY_INDEX_ARG, std::move(l));
    }
#else
    static symbol_type make_INCLUDE_ARRAY_INDEX_ARG(const location_type& l) {
        return symbol_type(token::INCLUDE_ARRAY_INDEX_ARG, l);
    }
#endif
#if 201103L <= YY_CPLUSPLUS
    static symbol_type make_PRESERVE_NULL_AND_EMPTY_ARRAYS_ARG(location_type l) {
        return symbol_type(token::PRESERVE_NULL_AND_EMPTY_ARRAYS_ARG, std::move(l));
    }
#else
    static symbol_type make_PRESERVE_NULL_AND_EMPTY_ARRAYS_ARG(const location_type& l) {
        return symbol_type(token::PRESERVE_NULL_AND_EMPTY_ARRAYS_ARG, l);
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
    static symbol_type make_CONVERT(location_type l) {
        return symbol_type(token::CONVERT, std::move(l));
    }
#else
    static symbol_type make_CONVERT(const location_type& l) {
        return symbol_type(token::CONVERT, l);
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
    static symbol_type make_TYPE(location_type l) {
        return symbol_type(token::TYPE, std::move(l));
    }
#else
    static symbol_type make_TYPE(const location_type& l) {
        return symbol_type(token::TYPE, l);
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
    static symbol_type make_CEIL(location_type l) {
        return symbol_type(token::CEIL, std::move(l));
    }
#else
    static symbol_type make_CEIL(const location_type& l) {
        return symbol_type(token::CEIL, l);
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
    static symbol_type make_POW(location_type l) {
        return symbol_type(token::POW, std::move(l));
    }
#else
    static symbol_type make_POW(const location_type& l) {
        return symbol_type(token::POW, l);
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
    static symbol_type make_SQRT(location_type l) {
        return symbol_type(token::SQRT, std::move(l));
    }
#else
    static symbol_type make_SQRT(const location_type& l) {
        return symbol_type(token::SQRT, l);
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
    static symbol_type make_TRUNC(location_type l) {
        return symbol_type(token::TRUNC, std::move(l));
    }
#else
    static symbol_type make_TRUNC(const location_type& l) {
        return symbol_type(token::TRUNC, l);
    }
#endif
#if 201103L <= YY_CPLUSPLUS
    static symbol_type make_INPUT_ARG(location_type l) {
        return symbol_type(token::INPUT_ARG, std::move(l));
    }
#else
    static symbol_type make_INPUT_ARG(const location_type& l) {
        return symbol_type(token::INPUT_ARG, l);
    }
#endif
#if 201103L <= YY_CPLUSPLUS
    static symbol_type make_TO_ARG(location_type l) {
        return symbol_type(token::TO_ARG, std::move(l));
    }
#else
    static symbol_type make_TO_ARG(const location_type& l) {
        return symbol_type(token::TO_ARG, l);
    }
#endif
#if 201103L <= YY_CPLUSPLUS
    static symbol_type make_ON_ERROR_ARG(location_type l) {
        return symbol_type(token::ON_ERROR_ARG, std::move(l));
    }
#else
    static symbol_type make_ON_ERROR_ARG(const location_type& l) {
        return symbol_type(token::ON_ERROR_ARG, l);
    }
#endif
#if 201103L <= YY_CPLUSPLUS
    static symbol_type make_ON_NULL_ARG(location_type l) {
        return symbol_type(token::ON_NULL_ARG, std::move(l));
    }
#else
    static symbol_type make_ON_NULL_ARG(const location_type& l) {
        return symbol_type(token::ON_NULL_ARG, l);
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
    static symbol_type make_NONEMPTY_STRING(std::string v, location_type l) {
        return symbol_type(token::NONEMPTY_STRING, std::move(v), std::move(l));
    }
#else
    static symbol_type make_NONEMPTY_STRING(const std::string& v, const location_type& l) {
        return symbol_type(token::NONEMPTY_STRING, v, l);
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
    static symbol_type make_DOLLAR_STRING(std::string v, location_type l) {
        return symbol_type(token::DOLLAR_STRING, std::move(v), std::move(l));
    }
#else
    static symbol_type make_DOLLAR_STRING(const std::string& v, const location_type& l) {
        return symbol_type(token::DOLLAR_STRING, v, l);
    }
#endif
#if 201103L <= YY_CPLUSPLUS
    static symbol_type make_EMPTY_STRING(std::string v, location_type l) {
        return symbol_type(token::EMPTY_STRING, std::move(v), std::move(l));
    }
#else
    static symbol_type make_EMPTY_STRING(const std::string& v, const location_type& l) {
        return symbol_type(token::EMPTY_STRING, v, l);
    }
#endif


private:
#if YY_CPLUSPLUS < 201103L
    /// Non copyable.
    PipelineParserGen(const PipelineParserGen&);
    /// Non copyable.
    PipelineParserGen& operator=(const PipelineParserGen&);
#endif


    /// Stored state numbers (used for stacks).
    typedef short state_type;

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

#if YYDEBUG || 0
    /// The user-facing name of the symbol whose (internal) number is
    /// YYSYMBOL.  No bounds checking.
    static const char* symbol_name(symbol_kind_type yysymbol);

    /// For a symbol, its name in clear.
    static const char* const yytname_[];
#endif  // #if YYDEBUG || 0


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
    static const unsigned char yystos_[];

    // YYR1[YYN] -- Symbol number of symbol that rule YYN derives.
    static const unsigned char yyr1_[];

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
        yylast_ = 656,  ///< Last index in yytable_.
        yynnts_ = 117,  ///< Number of nonterminal symbols.
        yyfinal_ = 8    ///< Termination state number.
    };


    // User arguments.
    BSONLexer& lexer;
    CNode* cst;
};

inline PipelineParserGen::symbol_kind_type PipelineParserGen::yytranslate_(int t) {
    return static_cast<symbol_kind_type>(t);
}

// basic_symbol.
template <typename Base>
PipelineParserGen::basic_symbol<Base>::basic_symbol(const basic_symbol& that)
    : Base(that), value(), location(that.location) {
    switch (this->kind()) {
        case 72:  // BINARY
            value.copy<BSONBinData>(YY_MOVE(that.value));
            break;

        case 79:  // JAVASCRIPT
            value.copy<BSONCode>(YY_MOVE(that.value));
            break;

        case 81:  // JAVASCRIPT_W_SCOPE
            value.copy<BSONCodeWScope>(YY_MOVE(that.value));
            break;

        case 78:  // DB_POINTER
            value.copy<BSONDBRef>(YY_MOVE(that.value));
            break;

        case 77:  // REGEX
            value.copy<BSONRegEx>(YY_MOVE(that.value));
            break;

        case 80:  // SYMBOL
            value.copy<BSONSymbol>(YY_MOVE(that.value));
            break;

        case 107:  // dbPointer
        case 108:  // javascript
        case 109:  // symbol
        case 110:  // javascriptWScope
        case 111:  // int
        case 112:  // timestamp
        case 113:  // long
        case 114:  // double
        case 115:  // decimal
        case 116:  // minKey
        case 117:  // maxKey
        case 118:  // value
        case 119:  // string
        case 120:  // binary
        case 121:  // undefined
        case 122:  // objectId
        case 123:  // bool
        case 124:  // date
        case 125:  // null
        case 126:  // regex
        case 127:  // simpleValue
        case 128:  // compoundValue
        case 129:  // valueArray
        case 130:  // valueObject
        case 131:  // valueFields
        case 132:  // dollarString
        case 133:  // nonDollarString
        case 134:  // stageList
        case 135:  // stage
        case 136:  // inhibitOptimization
        case 137:  // unionWith
        case 138:  // skip
        case 139:  // limit
        case 140:  // project
        case 141:  // sample
        case 142:  // unwind
        case 143:  // projectFields
        case 144:  // projection
        case 145:  // num
        case 148:  // expression
        case 149:  // compoundExpression
        case 150:  // exprFixedTwoArg
        case 151:  // expressionArray
        case 152:  // expressionObject
        case 153:  // expressionFields
        case 154:  // maths
        case 155:  // add
        case 156:  // atan2
        case 157:  // boolExps
        case 158:  // and
        case 159:  // or
        case 160:  // not
        case 161:  // literalEscapes
        case 162:  // const
        case 163:  // literal
        case 164:  // compExprs
        case 165:  // cmp
        case 166:  // eq
        case 167:  // gt
        case 168:  // gte
        case 169:  // lt
        case 170:  // lte
        case 171:  // ne
        case 172:  // typeExpression
        case 173:  // typeValue
        case 174:  // convert
        case 175:  // toBool
        case 176:  // toDate
        case 177:  // toDecimal
        case 178:  // toDouble
        case 179:  // toInt
        case 180:  // toLong
        case 181:  // toObjectId
        case 182:  // toString
        case 183:  // type
        case 184:  // abs
        case 185:  // ceil
        case 186:  // divide
        case 187:  // exponent
        case 188:  // floor
        case 189:  // ln
        case 190:  // log
        case 191:  // logten
        case 192:  // mod
        case 193:  // multiply
        case 194:  // pow
        case 195:  // round
        case 196:  // sqrt
        case 197:  // subtract
        case 198:  // trunc
        case 203:  // matchExpression
        case 204:  // filterFields
        case 205:  // filterVal
            value.copy<CNode>(YY_MOVE(that.value));
            break;

        case 94:   // projectionFieldname
        case 95:   // expressionFieldname
        case 96:   // stageAsUserFieldname
        case 97:   // filterFieldname
        case 98:   // argAsUserFieldname
        case 99:   // aggExprAsUserFieldname
        case 100:  // invariableUserFieldname
        case 101:  // idAsUserFieldname
        case 102:  // valueFieldname
            value.copy<CNode::Fieldname>(YY_MOVE(that.value));
            break;

        case 75:  // DATE_LITERAL
            value.copy<Date_t>(YY_MOVE(that.value));
            break;

        case 86:  // DECIMAL_NON_ZERO
            value.copy<Decimal128>(YY_MOVE(that.value));
            break;

        case 74:  // OBJECT_ID
            value.copy<OID>(YY_MOVE(that.value));
            break;

        case 83:  // TIMESTAMP
            value.copy<Timestamp>(YY_MOVE(that.value));
            break;

        case 88:  // MAX_KEY
            value.copy<UserMaxKey>(YY_MOVE(that.value));
            break;

        case 87:  // MIN_KEY
            value.copy<UserMinKey>(YY_MOVE(that.value));
            break;

        case 76:  // JSNULL
            value.copy<UserNull>(YY_MOVE(that.value));
            break;

        case 73:  // UNDEFINED
            value.copy<UserUndefined>(YY_MOVE(that.value));
            break;

        case 85:  // DOUBLE_NON_ZERO
            value.copy<double>(YY_MOVE(that.value));
            break;

        case 82:  // INT_NON_ZERO
            value.copy<int>(YY_MOVE(that.value));
            break;

        case 84:  // LONG_NON_ZERO
            value.copy<long long>(YY_MOVE(that.value));
            break;

        case 103:  // projectField
        case 104:  // expressionField
        case 105:  // valueField
        case 106:  // filterField
        case 146:  // includeArrayIndexArg
        case 147:  // preserveNullAndEmptyArraysArg
        case 199:  // onErrorArg
        case 200:  // onNullArg
            value.copy<std::pair<CNode::Fieldname, CNode>>(YY_MOVE(that.value));
            break;

        case 70:  // FIELDNAME
        case 71:  // NONEMPTY_STRING
        case 91:  // "a $-prefixed string"
        case 92:  // "an empty string"
            value.copy<std::string>(YY_MOVE(that.value));
            break;

        case 201:  // expressions
        case 202:  // values
            value.copy<std::vector<CNode>>(YY_MOVE(that.value));
            break;

        default:
            break;
    }
}


template <typename Base>
PipelineParserGen::symbol_kind_type PipelineParserGen::basic_symbol<Base>::type_get() const
    YY_NOEXCEPT {
    return this->kind();
}

template <typename Base>
bool PipelineParserGen::basic_symbol<Base>::empty() const YY_NOEXCEPT {
    return this->kind() == symbol_kind::S_YYEMPTY;
}

template <typename Base>
void PipelineParserGen::basic_symbol<Base>::move(basic_symbol& s) {
    super_type::move(s);
    switch (this->kind()) {
        case 72:  // BINARY
            value.move<BSONBinData>(YY_MOVE(s.value));
            break;

        case 79:  // JAVASCRIPT
            value.move<BSONCode>(YY_MOVE(s.value));
            break;

        case 81:  // JAVASCRIPT_W_SCOPE
            value.move<BSONCodeWScope>(YY_MOVE(s.value));
            break;

        case 78:  // DB_POINTER
            value.move<BSONDBRef>(YY_MOVE(s.value));
            break;

        case 77:  // REGEX
            value.move<BSONRegEx>(YY_MOVE(s.value));
            break;

        case 80:  // SYMBOL
            value.move<BSONSymbol>(YY_MOVE(s.value));
            break;

        case 107:  // dbPointer
        case 108:  // javascript
        case 109:  // symbol
        case 110:  // javascriptWScope
        case 111:  // int
        case 112:  // timestamp
        case 113:  // long
        case 114:  // double
        case 115:  // decimal
        case 116:  // minKey
        case 117:  // maxKey
        case 118:  // value
        case 119:  // string
        case 120:  // binary
        case 121:  // undefined
        case 122:  // objectId
        case 123:  // bool
        case 124:  // date
        case 125:  // null
        case 126:  // regex
        case 127:  // simpleValue
        case 128:  // compoundValue
        case 129:  // valueArray
        case 130:  // valueObject
        case 131:  // valueFields
        case 132:  // dollarString
        case 133:  // nonDollarString
        case 134:  // stageList
        case 135:  // stage
        case 136:  // inhibitOptimization
        case 137:  // unionWith
        case 138:  // skip
        case 139:  // limit
        case 140:  // project
        case 141:  // sample
        case 142:  // unwind
        case 143:  // projectFields
        case 144:  // projection
        case 145:  // num
        case 148:  // expression
        case 149:  // compoundExpression
        case 150:  // exprFixedTwoArg
        case 151:  // expressionArray
        case 152:  // expressionObject
        case 153:  // expressionFields
        case 154:  // maths
        case 155:  // add
        case 156:  // atan2
        case 157:  // boolExps
        case 158:  // and
        case 159:  // or
        case 160:  // not
        case 161:  // literalEscapes
        case 162:  // const
        case 163:  // literal
        case 164:  // compExprs
        case 165:  // cmp
        case 166:  // eq
        case 167:  // gt
        case 168:  // gte
        case 169:  // lt
        case 170:  // lte
        case 171:  // ne
        case 172:  // typeExpression
        case 173:  // typeValue
        case 174:  // convert
        case 175:  // toBool
        case 176:  // toDate
        case 177:  // toDecimal
        case 178:  // toDouble
        case 179:  // toInt
        case 180:  // toLong
        case 181:  // toObjectId
        case 182:  // toString
        case 183:  // type
        case 184:  // abs
        case 185:  // ceil
        case 186:  // divide
        case 187:  // exponent
        case 188:  // floor
        case 189:  // ln
        case 190:  // log
        case 191:  // logten
        case 192:  // mod
        case 193:  // multiply
        case 194:  // pow
        case 195:  // round
        case 196:  // sqrt
        case 197:  // subtract
        case 198:  // trunc
        case 203:  // matchExpression
        case 204:  // filterFields
        case 205:  // filterVal
            value.move<CNode>(YY_MOVE(s.value));
            break;

        case 94:   // projectionFieldname
        case 95:   // expressionFieldname
        case 96:   // stageAsUserFieldname
        case 97:   // filterFieldname
        case 98:   // argAsUserFieldname
        case 99:   // aggExprAsUserFieldname
        case 100:  // invariableUserFieldname
        case 101:  // idAsUserFieldname
        case 102:  // valueFieldname
            value.move<CNode::Fieldname>(YY_MOVE(s.value));
            break;

        case 75:  // DATE_LITERAL
            value.move<Date_t>(YY_MOVE(s.value));
            break;

        case 86:  // DECIMAL_NON_ZERO
            value.move<Decimal128>(YY_MOVE(s.value));
            break;

        case 74:  // OBJECT_ID
            value.move<OID>(YY_MOVE(s.value));
            break;

        case 83:  // TIMESTAMP
            value.move<Timestamp>(YY_MOVE(s.value));
            break;

        case 88:  // MAX_KEY
            value.move<UserMaxKey>(YY_MOVE(s.value));
            break;

        case 87:  // MIN_KEY
            value.move<UserMinKey>(YY_MOVE(s.value));
            break;

        case 76:  // JSNULL
            value.move<UserNull>(YY_MOVE(s.value));
            break;

        case 73:  // UNDEFINED
            value.move<UserUndefined>(YY_MOVE(s.value));
            break;

        case 85:  // DOUBLE_NON_ZERO
            value.move<double>(YY_MOVE(s.value));
            break;

        case 82:  // INT_NON_ZERO
            value.move<int>(YY_MOVE(s.value));
            break;

        case 84:  // LONG_NON_ZERO
            value.move<long long>(YY_MOVE(s.value));
            break;

        case 103:  // projectField
        case 104:  // expressionField
        case 105:  // valueField
        case 106:  // filterField
        case 146:  // includeArrayIndexArg
        case 147:  // preserveNullAndEmptyArraysArg
        case 199:  // onErrorArg
        case 200:  // onNullArg
            value.move<std::pair<CNode::Fieldname, CNode>>(YY_MOVE(s.value));
            break;

        case 70:  // FIELDNAME
        case 71:  // NONEMPTY_STRING
        case 91:  // "a $-prefixed string"
        case 92:  // "an empty string"
            value.move<std::string>(YY_MOVE(s.value));
            break;

        case 201:  // expressions
        case 202:  // values
            value.move<std::vector<CNode>>(YY_MOVE(s.value));
            break;

        default:
            break;
    }

    location = YY_MOVE(s.location);
}

// by_kind.
inline PipelineParserGen::by_kind::by_kind() : kind_(symbol_kind::S_YYEMPTY) {}

#if 201103L <= YY_CPLUSPLUS
inline PipelineParserGen::by_kind::by_kind(by_kind&& that) : kind_(that.kind_) {
    that.clear();
}
#endif

inline PipelineParserGen::by_kind::by_kind(const by_kind& that) : kind_(that.kind_) {}

inline PipelineParserGen::by_kind::by_kind(token_kind_type t) : kind_(yytranslate_(t)) {}

inline void PipelineParserGen::by_kind::clear() {
    kind_ = symbol_kind::S_YYEMPTY;
}

inline void PipelineParserGen::by_kind::move(by_kind& that) {
    kind_ = that.kind_;
    that.clear();
}

inline PipelineParserGen::symbol_kind_type PipelineParserGen::by_kind::kind() const YY_NOEXCEPT {
    return kind_;
}

inline PipelineParserGen::symbol_kind_type PipelineParserGen::by_kind::type_get() const
    YY_NOEXCEPT {
    return this->kind();
}

#line 58 "src/mongo/db/cst/pipeline_grammar.yy"
}  // namespace mongo
#line 4315 "src/mongo/db/cst/pipeline_parser_gen.hpp"


#endif  // !YY_YY_SRC_MONGO_DB_CST_PIPELINE_PARSER_GEN_HPP_INCLUDED
