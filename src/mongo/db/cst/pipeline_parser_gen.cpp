// A Bison parser, made by GNU Bison 3.6.3.

// Skeleton implementation for Bison LALR(1) parsers in C++

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

// DO NOT RELY ON FEATURES THAT ARE NOT DOCUMENTED in the manual,
// especially those whose name start with YY_ or yy_.  They are
// private implementation details that can be changed or removed.


#include "pipeline_parser_gen.hpp"


// Unqualified %code blocks.
#line 83 "pipeline_grammar.yy"

#include "mongo/db/cst/bson_lexer.h"
#include "mongo/platform/decimal128.h"

namespace mongo {
// Mandatory error function.
void PipelineParserGen::error(const PipelineParserGen::location_type& loc, const std::string& msg) {
    uasserted(ErrorCodes::FailedToParse,
              str::stream() << msg << " at location " << loc.begin.line << ":" << loc.begin.column
                            << " of input BSON. Lexer produced token of type "
                            << lexer[loc.begin.column].type_get() << ".");
}
}  // namespace mongo

#line 63 "pipeline_parser_gen.cpp"


#ifndef YY_
#if defined YYENABLE_NLS && YYENABLE_NLS
#if ENABLE_NLS
#include <libintl.h>  // FIXME: INFRINGES ON USER NAME SPACE.
#define YY_(msgid) dgettext("bison-runtime", msgid)
#endif
#endif
#ifndef YY_
#define YY_(msgid) msgid
#endif
#endif


// Whether we are compiled with exception support.
#ifndef YY_EXCEPTIONS
#if defined __GNUC__ && !defined __EXCEPTIONS
#define YY_EXCEPTIONS 0
#else
#define YY_EXCEPTIONS 1
#endif
#endif

#define YYRHSLOC(Rhs, K) ((Rhs)[K].location)
/* YYLLOC_DEFAULT -- Set CURRENT to span from RHS[1] to RHS[N].
   If N is 0, then set CURRENT to the empty location which ends
   the previous symbol: RHS[0] (always defined).  */

#ifndef YYLLOC_DEFAULT
#define YYLLOC_DEFAULT(Current, Rhs, N)                             \
    do                                                              \
        if (N) {                                                    \
            (Current).begin = YYRHSLOC(Rhs, 1).begin;               \
            (Current).end = YYRHSLOC(Rhs, N).end;                   \
        } else {                                                    \
            (Current).begin = (Current).end = YYRHSLOC(Rhs, 0).end; \
        }                                                           \
    while (false)
#endif


// Enable debugging if requested.
#if YYDEBUG

// A pseudo ostream that takes yydebug_ into account.
#define YYCDEBUG  \
    if (yydebug_) \
    (*yycdebug_)

#define YY_SYMBOL_PRINT(Title, Symbol)     \
    do {                                   \
        if (yydebug_) {                    \
            *yycdebug_ << Title << ' ';    \
            yy_print_(*yycdebug_, Symbol); \
            *yycdebug_ << '\n';            \
        }                                  \
    } while (false)

#define YY_REDUCE_PRINT(Rule)       \
    do {                            \
        if (yydebug_)               \
            yy_reduce_print_(Rule); \
    } while (false)

#define YY_STACK_PRINT()       \
    do {                       \
        if (yydebug_)          \
            yy_stack_print_(); \
    } while (false)

#else  // !YYDEBUG

#define YYCDEBUG \
    if (false)   \
    std::cerr
#define YY_SYMBOL_PRINT(Title, Symbol) YYUSE(Symbol)
#define YY_REDUCE_PRINT(Rule) static_cast<void>(0)
#define YY_STACK_PRINT() static_cast<void>(0)

#endif  // !YYDEBUG

#define yyerrok (yyerrstatus_ = 0)
#define yyclearin (yyla.clear())

#define YYACCEPT goto yyacceptlab
#define YYABORT goto yyabortlab
#define YYERROR goto yyerrorlab
#define YYRECOVERING() (!!yyerrstatus_)

#line 58 "pipeline_grammar.yy"
namespace mongo {
#line 156 "pipeline_parser_gen.cpp"

/// Build a parser object.
PipelineParserGen::PipelineParserGen(BSONLexer& lexer_yyarg, CNode* cst_yyarg)
#if YYDEBUG
    : yydebug_(false),
      yycdebug_(&std::cerr),
#else
    :
#endif
      lexer(lexer_yyarg),
      cst(cst_yyarg) {
}

PipelineParserGen::~PipelineParserGen() {}

PipelineParserGen::syntax_error::~syntax_error() YY_NOEXCEPT YY_NOTHROW {}

/*---------------.
| symbol kinds.  |
`---------------*/


// by_state.
PipelineParserGen::by_state::by_state() YY_NOEXCEPT : state(empty_state) {}

PipelineParserGen::by_state::by_state(const by_state& that) YY_NOEXCEPT : state(that.state) {}

void PipelineParserGen::by_state::clear() YY_NOEXCEPT {
    state = empty_state;
}

void PipelineParserGen::by_state::move(by_state& that) {
    state = that.state;
    that.clear();
}

PipelineParserGen::by_state::by_state(state_type s) YY_NOEXCEPT : state(s) {}

PipelineParserGen::symbol_kind_type PipelineParserGen::by_state::kind() const YY_NOEXCEPT {
    if (state == empty_state)
        return symbol_kind::S_YYEMPTY;
    else
        return YY_CAST(symbol_kind_type, yystos_[+state]);
}

PipelineParserGen::stack_symbol_type::stack_symbol_type() {}

PipelineParserGen::stack_symbol_type::stack_symbol_type(YY_RVREF(stack_symbol_type) that)
    : super_type(YY_MOVE(that.state), YY_MOVE(that.location)) {
    switch (that.kind()) {
        case 68:  // BINARY
            value.YY_MOVE_OR_COPY<BSONBinData>(YY_MOVE(that.value));
            break;

        case 75:  // JAVASCRIPT
            value.YY_MOVE_OR_COPY<BSONCode>(YY_MOVE(that.value));
            break;

        case 77:  // JAVASCRIPT_W_SCOPE
            value.YY_MOVE_OR_COPY<BSONCodeWScope>(YY_MOVE(that.value));
            break;

        case 74:  // DB_POINTER
            value.YY_MOVE_OR_COPY<BSONDBRef>(YY_MOVE(that.value));
            break;

        case 73:  // REGEX
            value.YY_MOVE_OR_COPY<BSONRegEx>(YY_MOVE(that.value));
            break;

        case 76:  // SYMBOL
            value.YY_MOVE_OR_COPY<BSONSymbol>(YY_MOVE(that.value));
            break;

        case 97:   // dbPointer
        case 98:   // javascript
        case 99:   // symbol
        case 100:  // javascriptWScope
        case 101:  // int
        case 102:  // timestamp
        case 103:  // long
        case 104:  // double
        case 105:  // decimal
        case 106:  // minKey
        case 107:  // maxKey
        case 108:  // value
        case 109:  // string
        case 110:  // binary
        case 111:  // undefined
        case 112:  // objectId
        case 113:  // bool
        case 114:  // date
        case 115:  // null
        case 116:  // regex
        case 117:  // simpleValue
        case 118:  // compoundValue
        case 119:  // valueArray
        case 120:  // valueObject
        case 121:  // valueFields
        case 122:  // stageList
        case 123:  // stage
        case 124:  // inhibitOptimization
        case 125:  // unionWith
        case 126:  // skip
        case 127:  // limit
        case 128:  // project
        case 129:  // sample
        case 130:  // projectFields
        case 131:  // projection
        case 132:  // num
        case 133:  // expression
        case 134:  // compoundExpression
        case 135:  // exprFixedTwoArg
        case 136:  // expressionArray
        case 137:  // expressionObject
        case 138:  // expressionFields
        case 139:  // maths
        case 140:  // add
        case 141:  // atan2
        case 142:  // boolExps
        case 143:  // and
        case 144:  // or
        case 145:  // not
        case 146:  // literalEscapes
        case 147:  // const
        case 148:  // literal
        case 149:  // compExprs
        case 150:  // cmp
        case 151:  // eq
        case 152:  // gt
        case 153:  // gte
        case 154:  // lt
        case 155:  // lte
        case 156:  // ne
        case 157:  // typeExpression
        case 158:  // typeValue
        case 159:  // convert
        case 160:  // toBool
        case 161:  // toDate
        case 162:  // toDecimal
        case 163:  // toDouble
        case 164:  // toInt
        case 165:  // toLong
        case 166:  // toObjectId
        case 167:  // toString
        case 168:  // type
        case 169:  // abs
        case 170:  // ceil
        case 171:  // divide
        case 172:  // exponent
        case 173:  // floor
        case 174:  // ln
        case 175:  // log
        case 176:  // logten
        case 177:  // mod
        case 178:  // multiply
        case 179:  // pow
        case 180:  // round
        case 181:  // sqrt
        case 182:  // subtract
        case 183:  // trunc
            value.YY_MOVE_OR_COPY<CNode>(YY_MOVE(that.value));
            break;

        case 86:  // projectionFieldname
        case 87:  // expressionFieldname
        case 88:  // stageAsUserFieldname
        case 89:  // argAsUserFieldname
        case 90:  // aggExprAsUserFieldname
        case 91:  // invariableUserFieldname
        case 92:  // idAsUserFieldname
        case 93:  // valueFieldname
            value.YY_MOVE_OR_COPY<CNode::Fieldname>(YY_MOVE(that.value));
            break;

        case 71:  // DATE_LITERAL
            value.YY_MOVE_OR_COPY<Date_t>(YY_MOVE(that.value));
            break;

        case 82:  // DECIMAL_NON_ZERO
            value.YY_MOVE_OR_COPY<Decimal128>(YY_MOVE(that.value));
            break;

        case 70:  // OBJECT_ID
            value.YY_MOVE_OR_COPY<OID>(YY_MOVE(that.value));
            break;

        case 79:  // TIMESTAMP
            value.YY_MOVE_OR_COPY<Timestamp>(YY_MOVE(that.value));
            break;

        case 84:  // MAX_KEY
            value.YY_MOVE_OR_COPY<UserMaxKey>(YY_MOVE(that.value));
            break;

        case 83:  // MIN_KEY
            value.YY_MOVE_OR_COPY<UserMinKey>(YY_MOVE(that.value));
            break;

        case 72:  // JSNULL
            value.YY_MOVE_OR_COPY<UserNull>(YY_MOVE(that.value));
            break;

        case 69:  // UNDEFINED
            value.YY_MOVE_OR_COPY<UserUndefined>(YY_MOVE(that.value));
            break;

        case 81:  // DOUBLE_NON_ZERO
            value.YY_MOVE_OR_COPY<double>(YY_MOVE(that.value));
            break;

        case 78:  // INT_NON_ZERO
            value.YY_MOVE_OR_COPY<int>(YY_MOVE(that.value));
            break;

        case 80:  // LONG_NON_ZERO
            value.YY_MOVE_OR_COPY<long long>(YY_MOVE(that.value));
            break;

        case 94:   // projectField
        case 95:   // expressionField
        case 96:   // valueField
        case 184:  // onErrorArg
        case 185:  // onNullArg
            value.YY_MOVE_OR_COPY<std::pair<CNode::Fieldname, CNode>>(YY_MOVE(that.value));
            break;

        case 66:  // FIELDNAME
        case 67:  // STRING
            value.YY_MOVE_OR_COPY<std::string>(YY_MOVE(that.value));
            break;

        case 186:  // expressions
        case 187:  // values
            value.YY_MOVE_OR_COPY<std::vector<CNode>>(YY_MOVE(that.value));
            break;

        default:
            break;
    }

#if 201103L <= YY_CPLUSPLUS
    // that is emptied.
    that.state = empty_state;
#endif
}

PipelineParserGen::stack_symbol_type::stack_symbol_type(state_type s, YY_MOVE_REF(symbol_type) that)
    : super_type(s, YY_MOVE(that.location)) {
    switch (that.kind()) {
        case 68:  // BINARY
            value.move<BSONBinData>(YY_MOVE(that.value));
            break;

        case 75:  // JAVASCRIPT
            value.move<BSONCode>(YY_MOVE(that.value));
            break;

        case 77:  // JAVASCRIPT_W_SCOPE
            value.move<BSONCodeWScope>(YY_MOVE(that.value));
            break;

        case 74:  // DB_POINTER
            value.move<BSONDBRef>(YY_MOVE(that.value));
            break;

        case 73:  // REGEX
            value.move<BSONRegEx>(YY_MOVE(that.value));
            break;

        case 76:  // SYMBOL
            value.move<BSONSymbol>(YY_MOVE(that.value));
            break;

        case 97:   // dbPointer
        case 98:   // javascript
        case 99:   // symbol
        case 100:  // javascriptWScope
        case 101:  // int
        case 102:  // timestamp
        case 103:  // long
        case 104:  // double
        case 105:  // decimal
        case 106:  // minKey
        case 107:  // maxKey
        case 108:  // value
        case 109:  // string
        case 110:  // binary
        case 111:  // undefined
        case 112:  // objectId
        case 113:  // bool
        case 114:  // date
        case 115:  // null
        case 116:  // regex
        case 117:  // simpleValue
        case 118:  // compoundValue
        case 119:  // valueArray
        case 120:  // valueObject
        case 121:  // valueFields
        case 122:  // stageList
        case 123:  // stage
        case 124:  // inhibitOptimization
        case 125:  // unionWith
        case 126:  // skip
        case 127:  // limit
        case 128:  // project
        case 129:  // sample
        case 130:  // projectFields
        case 131:  // projection
        case 132:  // num
        case 133:  // expression
        case 134:  // compoundExpression
        case 135:  // exprFixedTwoArg
        case 136:  // expressionArray
        case 137:  // expressionObject
        case 138:  // expressionFields
        case 139:  // maths
        case 140:  // add
        case 141:  // atan2
        case 142:  // boolExps
        case 143:  // and
        case 144:  // or
        case 145:  // not
        case 146:  // literalEscapes
        case 147:  // const
        case 148:  // literal
        case 149:  // compExprs
        case 150:  // cmp
        case 151:  // eq
        case 152:  // gt
        case 153:  // gte
        case 154:  // lt
        case 155:  // lte
        case 156:  // ne
        case 157:  // typeExpression
        case 158:  // typeValue
        case 159:  // convert
        case 160:  // toBool
        case 161:  // toDate
        case 162:  // toDecimal
        case 163:  // toDouble
        case 164:  // toInt
        case 165:  // toLong
        case 166:  // toObjectId
        case 167:  // toString
        case 168:  // type
        case 169:  // abs
        case 170:  // ceil
        case 171:  // divide
        case 172:  // exponent
        case 173:  // floor
        case 174:  // ln
        case 175:  // log
        case 176:  // logten
        case 177:  // mod
        case 178:  // multiply
        case 179:  // pow
        case 180:  // round
        case 181:  // sqrt
        case 182:  // subtract
        case 183:  // trunc
            value.move<CNode>(YY_MOVE(that.value));
            break;

        case 86:  // projectionFieldname
        case 87:  // expressionFieldname
        case 88:  // stageAsUserFieldname
        case 89:  // argAsUserFieldname
        case 90:  // aggExprAsUserFieldname
        case 91:  // invariableUserFieldname
        case 92:  // idAsUserFieldname
        case 93:  // valueFieldname
            value.move<CNode::Fieldname>(YY_MOVE(that.value));
            break;

        case 71:  // DATE_LITERAL
            value.move<Date_t>(YY_MOVE(that.value));
            break;

        case 82:  // DECIMAL_NON_ZERO
            value.move<Decimal128>(YY_MOVE(that.value));
            break;

        case 70:  // OBJECT_ID
            value.move<OID>(YY_MOVE(that.value));
            break;

        case 79:  // TIMESTAMP
            value.move<Timestamp>(YY_MOVE(that.value));
            break;

        case 84:  // MAX_KEY
            value.move<UserMaxKey>(YY_MOVE(that.value));
            break;

        case 83:  // MIN_KEY
            value.move<UserMinKey>(YY_MOVE(that.value));
            break;

        case 72:  // JSNULL
            value.move<UserNull>(YY_MOVE(that.value));
            break;

        case 69:  // UNDEFINED
            value.move<UserUndefined>(YY_MOVE(that.value));
            break;

        case 81:  // DOUBLE_NON_ZERO
            value.move<double>(YY_MOVE(that.value));
            break;

        case 78:  // INT_NON_ZERO
            value.move<int>(YY_MOVE(that.value));
            break;

        case 80:  // LONG_NON_ZERO
            value.move<long long>(YY_MOVE(that.value));
            break;

        case 94:   // projectField
        case 95:   // expressionField
        case 96:   // valueField
        case 184:  // onErrorArg
        case 185:  // onNullArg
            value.move<std::pair<CNode::Fieldname, CNode>>(YY_MOVE(that.value));
            break;

        case 66:  // FIELDNAME
        case 67:  // STRING
            value.move<std::string>(YY_MOVE(that.value));
            break;

        case 186:  // expressions
        case 187:  // values
            value.move<std::vector<CNode>>(YY_MOVE(that.value));
            break;

        default:
            break;
    }

    // that is emptied.
    that.kind_ = symbol_kind::S_YYEMPTY;
}

#if YY_CPLUSPLUS < 201103L
PipelineParserGen::stack_symbol_type& PipelineParserGen::stack_symbol_type::operator=(
    const stack_symbol_type& that) {
    state = that.state;
    switch (that.kind()) {
        case 68:  // BINARY
            value.copy<BSONBinData>(that.value);
            break;

        case 75:  // JAVASCRIPT
            value.copy<BSONCode>(that.value);
            break;

        case 77:  // JAVASCRIPT_W_SCOPE
            value.copy<BSONCodeWScope>(that.value);
            break;

        case 74:  // DB_POINTER
            value.copy<BSONDBRef>(that.value);
            break;

        case 73:  // REGEX
            value.copy<BSONRegEx>(that.value);
            break;

        case 76:  // SYMBOL
            value.copy<BSONSymbol>(that.value);
            break;

        case 97:   // dbPointer
        case 98:   // javascript
        case 99:   // symbol
        case 100:  // javascriptWScope
        case 101:  // int
        case 102:  // timestamp
        case 103:  // long
        case 104:  // double
        case 105:  // decimal
        case 106:  // minKey
        case 107:  // maxKey
        case 108:  // value
        case 109:  // string
        case 110:  // binary
        case 111:  // undefined
        case 112:  // objectId
        case 113:  // bool
        case 114:  // date
        case 115:  // null
        case 116:  // regex
        case 117:  // simpleValue
        case 118:  // compoundValue
        case 119:  // valueArray
        case 120:  // valueObject
        case 121:  // valueFields
        case 122:  // stageList
        case 123:  // stage
        case 124:  // inhibitOptimization
        case 125:  // unionWith
        case 126:  // skip
        case 127:  // limit
        case 128:  // project
        case 129:  // sample
        case 130:  // projectFields
        case 131:  // projection
        case 132:  // num
        case 133:  // expression
        case 134:  // compoundExpression
        case 135:  // exprFixedTwoArg
        case 136:  // expressionArray
        case 137:  // expressionObject
        case 138:  // expressionFields
        case 139:  // maths
        case 140:  // add
        case 141:  // atan2
        case 142:  // boolExps
        case 143:  // and
        case 144:  // or
        case 145:  // not
        case 146:  // literalEscapes
        case 147:  // const
        case 148:  // literal
        case 149:  // compExprs
        case 150:  // cmp
        case 151:  // eq
        case 152:  // gt
        case 153:  // gte
        case 154:  // lt
        case 155:  // lte
        case 156:  // ne
        case 157:  // typeExpression
        case 158:  // typeValue
        case 159:  // convert
        case 160:  // toBool
        case 161:  // toDate
        case 162:  // toDecimal
        case 163:  // toDouble
        case 164:  // toInt
        case 165:  // toLong
        case 166:  // toObjectId
        case 167:  // toString
        case 168:  // type
        case 169:  // abs
        case 170:  // ceil
        case 171:  // divide
        case 172:  // exponent
        case 173:  // floor
        case 174:  // ln
        case 175:  // log
        case 176:  // logten
        case 177:  // mod
        case 178:  // multiply
        case 179:  // pow
        case 180:  // round
        case 181:  // sqrt
        case 182:  // subtract
        case 183:  // trunc
            value.copy<CNode>(that.value);
            break;

        case 86:  // projectionFieldname
        case 87:  // expressionFieldname
        case 88:  // stageAsUserFieldname
        case 89:  // argAsUserFieldname
        case 90:  // aggExprAsUserFieldname
        case 91:  // invariableUserFieldname
        case 92:  // idAsUserFieldname
        case 93:  // valueFieldname
            value.copy<CNode::Fieldname>(that.value);
            break;

        case 71:  // DATE_LITERAL
            value.copy<Date_t>(that.value);
            break;

        case 82:  // DECIMAL_NON_ZERO
            value.copy<Decimal128>(that.value);
            break;

        case 70:  // OBJECT_ID
            value.copy<OID>(that.value);
            break;

        case 79:  // TIMESTAMP
            value.copy<Timestamp>(that.value);
            break;

        case 84:  // MAX_KEY
            value.copy<UserMaxKey>(that.value);
            break;

        case 83:  // MIN_KEY
            value.copy<UserMinKey>(that.value);
            break;

        case 72:  // JSNULL
            value.copy<UserNull>(that.value);
            break;

        case 69:  // UNDEFINED
            value.copy<UserUndefined>(that.value);
            break;

        case 81:  // DOUBLE_NON_ZERO
            value.copy<double>(that.value);
            break;

        case 78:  // INT_NON_ZERO
            value.copy<int>(that.value);
            break;

        case 80:  // LONG_NON_ZERO
            value.copy<long long>(that.value);
            break;

        case 94:   // projectField
        case 95:   // expressionField
        case 96:   // valueField
        case 184:  // onErrorArg
        case 185:  // onNullArg
            value.copy<std::pair<CNode::Fieldname, CNode>>(that.value);
            break;

        case 66:  // FIELDNAME
        case 67:  // STRING
            value.copy<std::string>(that.value);
            break;

        case 186:  // expressions
        case 187:  // values
            value.copy<std::vector<CNode>>(that.value);
            break;

        default:
            break;
    }

    location = that.location;
    return *this;
}

PipelineParserGen::stack_symbol_type& PipelineParserGen::stack_symbol_type::operator=(
    stack_symbol_type& that) {
    state = that.state;
    switch (that.kind()) {
        case 68:  // BINARY
            value.move<BSONBinData>(that.value);
            break;

        case 75:  // JAVASCRIPT
            value.move<BSONCode>(that.value);
            break;

        case 77:  // JAVASCRIPT_W_SCOPE
            value.move<BSONCodeWScope>(that.value);
            break;

        case 74:  // DB_POINTER
            value.move<BSONDBRef>(that.value);
            break;

        case 73:  // REGEX
            value.move<BSONRegEx>(that.value);
            break;

        case 76:  // SYMBOL
            value.move<BSONSymbol>(that.value);
            break;

        case 97:   // dbPointer
        case 98:   // javascript
        case 99:   // symbol
        case 100:  // javascriptWScope
        case 101:  // int
        case 102:  // timestamp
        case 103:  // long
        case 104:  // double
        case 105:  // decimal
        case 106:  // minKey
        case 107:  // maxKey
        case 108:  // value
        case 109:  // string
        case 110:  // binary
        case 111:  // undefined
        case 112:  // objectId
        case 113:  // bool
        case 114:  // date
        case 115:  // null
        case 116:  // regex
        case 117:  // simpleValue
        case 118:  // compoundValue
        case 119:  // valueArray
        case 120:  // valueObject
        case 121:  // valueFields
        case 122:  // stageList
        case 123:  // stage
        case 124:  // inhibitOptimization
        case 125:  // unionWith
        case 126:  // skip
        case 127:  // limit
        case 128:  // project
        case 129:  // sample
        case 130:  // projectFields
        case 131:  // projection
        case 132:  // num
        case 133:  // expression
        case 134:  // compoundExpression
        case 135:  // exprFixedTwoArg
        case 136:  // expressionArray
        case 137:  // expressionObject
        case 138:  // expressionFields
        case 139:  // maths
        case 140:  // add
        case 141:  // atan2
        case 142:  // boolExps
        case 143:  // and
        case 144:  // or
        case 145:  // not
        case 146:  // literalEscapes
        case 147:  // const
        case 148:  // literal
        case 149:  // compExprs
        case 150:  // cmp
        case 151:  // eq
        case 152:  // gt
        case 153:  // gte
        case 154:  // lt
        case 155:  // lte
        case 156:  // ne
        case 157:  // typeExpression
        case 158:  // typeValue
        case 159:  // convert
        case 160:  // toBool
        case 161:  // toDate
        case 162:  // toDecimal
        case 163:  // toDouble
        case 164:  // toInt
        case 165:  // toLong
        case 166:  // toObjectId
        case 167:  // toString
        case 168:  // type
        case 169:  // abs
        case 170:  // ceil
        case 171:  // divide
        case 172:  // exponent
        case 173:  // floor
        case 174:  // ln
        case 175:  // log
        case 176:  // logten
        case 177:  // mod
        case 178:  // multiply
        case 179:  // pow
        case 180:  // round
        case 181:  // sqrt
        case 182:  // subtract
        case 183:  // trunc
            value.move<CNode>(that.value);
            break;

        case 86:  // projectionFieldname
        case 87:  // expressionFieldname
        case 88:  // stageAsUserFieldname
        case 89:  // argAsUserFieldname
        case 90:  // aggExprAsUserFieldname
        case 91:  // invariableUserFieldname
        case 92:  // idAsUserFieldname
        case 93:  // valueFieldname
            value.move<CNode::Fieldname>(that.value);
            break;

        case 71:  // DATE_LITERAL
            value.move<Date_t>(that.value);
            break;

        case 82:  // DECIMAL_NON_ZERO
            value.move<Decimal128>(that.value);
            break;

        case 70:  // OBJECT_ID
            value.move<OID>(that.value);
            break;

        case 79:  // TIMESTAMP
            value.move<Timestamp>(that.value);
            break;

        case 84:  // MAX_KEY
            value.move<UserMaxKey>(that.value);
            break;

        case 83:  // MIN_KEY
            value.move<UserMinKey>(that.value);
            break;

        case 72:  // JSNULL
            value.move<UserNull>(that.value);
            break;

        case 69:  // UNDEFINED
            value.move<UserUndefined>(that.value);
            break;

        case 81:  // DOUBLE_NON_ZERO
            value.move<double>(that.value);
            break;

        case 78:  // INT_NON_ZERO
            value.move<int>(that.value);
            break;

        case 80:  // LONG_NON_ZERO
            value.move<long long>(that.value);
            break;

        case 94:   // projectField
        case 95:   // expressionField
        case 96:   // valueField
        case 184:  // onErrorArg
        case 185:  // onNullArg
            value.move<std::pair<CNode::Fieldname, CNode>>(that.value);
            break;

        case 66:  // FIELDNAME
        case 67:  // STRING
            value.move<std::string>(that.value);
            break;

        case 186:  // expressions
        case 187:  // values
            value.move<std::vector<CNode>>(that.value);
            break;

        default:
            break;
    }

    location = that.location;
    // that is emptied.
    that.state = empty_state;
    return *this;
}
#endif

template <typename Base>
void PipelineParserGen::yy_destroy_(const char* yymsg, basic_symbol<Base>& yysym) const {
    if (yymsg)
        YY_SYMBOL_PRINT(yymsg, yysym);
}

#if YYDEBUG
template <typename Base>
void PipelineParserGen::yy_print_(std::ostream& yyo, const basic_symbol<Base>& yysym) const {
    std::ostream& yyoutput = yyo;
    YYUSE(yyoutput);
    if (yysym.empty())
        yyo << "empty symbol";
    else {
        symbol_kind_type yykind = yysym.kind();
        yyo << (yykind < YYNTOKENS ? "token" : "nterm") << ' ' << yysym.name() << " ("
            << yysym.location << ": ";
        YYUSE(yykind);
        yyo << ')';
    }
}
#endif

void PipelineParserGen::yypush_(const char* m, YY_MOVE_REF(stack_symbol_type) sym) {
    if (m)
        YY_SYMBOL_PRINT(m, sym);
    yystack_.push(YY_MOVE(sym));
}

void PipelineParserGen::yypush_(const char* m, state_type s, YY_MOVE_REF(symbol_type) sym) {
#if 201103L <= YY_CPLUSPLUS
    yypush_(m, stack_symbol_type(s, std::move(sym)));
#else
    stack_symbol_type ss(s, sym);
    yypush_(m, ss);
#endif
}

void PipelineParserGen::yypop_(int n) {
    yystack_.pop(n);
}

#if YYDEBUG
std::ostream& PipelineParserGen::debug_stream() const {
    return *yycdebug_;
}

void PipelineParserGen::set_debug_stream(std::ostream& o) {
    yycdebug_ = &o;
}


PipelineParserGen::debug_level_type PipelineParserGen::debug_level() const {
    return yydebug_;
}

void PipelineParserGen::set_debug_level(debug_level_type l) {
    yydebug_ = l;
}
#endif  // YYDEBUG

PipelineParserGen::state_type PipelineParserGen::yy_lr_goto_state_(state_type yystate, int yysym) {
    int yyr = yypgoto_[yysym - YYNTOKENS] + yystate;
    if (0 <= yyr && yyr <= yylast_ && yycheck_[yyr] == yystate)
        return yytable_[yyr];
    else
        return yydefgoto_[yysym - YYNTOKENS];
}

bool PipelineParserGen::yy_pact_value_is_default_(int yyvalue) {
    return yyvalue == yypact_ninf_;
}

bool PipelineParserGen::yy_table_value_is_error_(int yyvalue) {
    return yyvalue == yytable_ninf_;
}

int PipelineParserGen::operator()() {
    return parse();
}

int PipelineParserGen::parse() {
    int yyn;
    /// Length of the RHS of the rule being reduced.
    int yylen = 0;

    // Error handling.
    int yynerrs_ = 0;
    int yyerrstatus_ = 0;

    /// The lookahead symbol.
    symbol_type yyla;

    /// The locations where the error started and ended.
    stack_symbol_type yyerror_range[3];

    /// The return value of parse ().
    int yyresult;

#if YY_EXCEPTIONS
    try
#endif  // YY_EXCEPTIONS
    {
        YYCDEBUG << "Starting parse\n";


        /* Initialize the stack.  The initial state will be set in
           yynewstate, since the latter expects the semantical and the
           location values to have been already stored, initialize these
           stacks with a primary value.  */
        yystack_.clear();
        yypush_(YY_NULLPTR, 0, YY_MOVE(yyla));

    /*-----------------------------------------------.
    | yynewstate -- push a new symbol on the stack.  |
    `-----------------------------------------------*/
    yynewstate:
        YYCDEBUG << "Entering state " << int(yystack_[0].state) << '\n';
        YY_STACK_PRINT();

        // Accept?
        if (yystack_[0].state == yyfinal_)
            YYACCEPT;

        goto yybackup;


    /*-----------.
    | yybackup.  |
    `-----------*/
    yybackup:
        // Try to take a decision without lookahead.
        yyn = yypact_[+yystack_[0].state];
        if (yy_pact_value_is_default_(yyn))
            goto yydefault;

        // Read a lookahead token.
        if (yyla.empty()) {
            YYCDEBUG << "Reading a token\n";
#if YY_EXCEPTIONS
            try
#endif  // YY_EXCEPTIONS
            {
                symbol_type yylookahead(yylex(lexer));
                yyla.move(yylookahead);
            }
#if YY_EXCEPTIONS
            catch (const syntax_error& yyexc) {
                YYCDEBUG << "Caught exception: " << yyexc.what() << '\n';
                error(yyexc);
                goto yyerrlab1;
            }
#endif  // YY_EXCEPTIONS
        }
        YY_SYMBOL_PRINT("Next token is", yyla);

        if (yyla.kind() == symbol_kind::S_YYerror) {
            // The scanner already issued an error message, process directly
            // to error recovery.  But do not keep the error token as
            // lookahead, it is too special and may lead us to an endless
            // loop in error recovery. */
            yyla.kind_ = symbol_kind::S_YYUNDEF;
            goto yyerrlab1;
        }

        /* If the proper action on seeing token YYLA.TYPE is to reduce or
           to detect an error, take that action.  */
        yyn += yyla.kind();
        if (yyn < 0 || yylast_ < yyn || yycheck_[yyn] != yyla.kind()) {
            goto yydefault;
        }

        // Reduce or error.
        yyn = yytable_[yyn];
        if (yyn <= 0) {
            if (yy_table_value_is_error_(yyn))
                goto yyerrlab;
            yyn = -yyn;
            goto yyreduce;
        }

        // Count tokens shifted since error; after three, turn off error status.
        if (yyerrstatus_)
            --yyerrstatus_;

        // Shift the lookahead token.
        yypush_("Shifting", state_type(yyn), YY_MOVE(yyla));
        goto yynewstate;


    /*-----------------------------------------------------------.
    | yydefault -- do the default action for the current state.  |
    `-----------------------------------------------------------*/
    yydefault:
        yyn = yydefact_[+yystack_[0].state];
        if (yyn == 0)
            goto yyerrlab;
        goto yyreduce;


    /*-----------------------------.
    | yyreduce -- do a reduction.  |
    `-----------------------------*/
    yyreduce:
        yylen = yyr2_[yyn];
        {
            stack_symbol_type yylhs;
            yylhs.state = yy_lr_goto_state_(yystack_[yylen].state, yyr1_[yyn]);
            /* Variants are always initialized to an empty instance of the
               correct type. The default '$$ = $1' action is NOT applied
               when using variants.  */
            switch (yyr1_[yyn]) {
                case 68:  // BINARY
                    yylhs.value.emplace<BSONBinData>();
                    break;

                case 75:  // JAVASCRIPT
                    yylhs.value.emplace<BSONCode>();
                    break;

                case 77:  // JAVASCRIPT_W_SCOPE
                    yylhs.value.emplace<BSONCodeWScope>();
                    break;

                case 74:  // DB_POINTER
                    yylhs.value.emplace<BSONDBRef>();
                    break;

                case 73:  // REGEX
                    yylhs.value.emplace<BSONRegEx>();
                    break;

                case 76:  // SYMBOL
                    yylhs.value.emplace<BSONSymbol>();
                    break;

                case 97:   // dbPointer
                case 98:   // javascript
                case 99:   // symbol
                case 100:  // javascriptWScope
                case 101:  // int
                case 102:  // timestamp
                case 103:  // long
                case 104:  // double
                case 105:  // decimal
                case 106:  // minKey
                case 107:  // maxKey
                case 108:  // value
                case 109:  // string
                case 110:  // binary
                case 111:  // undefined
                case 112:  // objectId
                case 113:  // bool
                case 114:  // date
                case 115:  // null
                case 116:  // regex
                case 117:  // simpleValue
                case 118:  // compoundValue
                case 119:  // valueArray
                case 120:  // valueObject
                case 121:  // valueFields
                case 122:  // stageList
                case 123:  // stage
                case 124:  // inhibitOptimization
                case 125:  // unionWith
                case 126:  // skip
                case 127:  // limit
                case 128:  // project
                case 129:  // sample
                case 130:  // projectFields
                case 131:  // projection
                case 132:  // num
                case 133:  // expression
                case 134:  // compoundExpression
                case 135:  // exprFixedTwoArg
                case 136:  // expressionArray
                case 137:  // expressionObject
                case 138:  // expressionFields
                case 139:  // maths
                case 140:  // add
                case 141:  // atan2
                case 142:  // boolExps
                case 143:  // and
                case 144:  // or
                case 145:  // not
                case 146:  // literalEscapes
                case 147:  // const
                case 148:  // literal
                case 149:  // compExprs
                case 150:  // cmp
                case 151:  // eq
                case 152:  // gt
                case 153:  // gte
                case 154:  // lt
                case 155:  // lte
                case 156:  // ne
                case 157:  // typeExpression
                case 158:  // typeValue
                case 159:  // convert
                case 160:  // toBool
                case 161:  // toDate
                case 162:  // toDecimal
                case 163:  // toDouble
                case 164:  // toInt
                case 165:  // toLong
                case 166:  // toObjectId
                case 167:  // toString
                case 168:  // type
                case 169:  // abs
                case 170:  // ceil
                case 171:  // divide
                case 172:  // exponent
                case 173:  // floor
                case 174:  // ln
                case 175:  // log
                case 176:  // logten
                case 177:  // mod
                case 178:  // multiply
                case 179:  // pow
                case 180:  // round
                case 181:  // sqrt
                case 182:  // subtract
                case 183:  // trunc
                    yylhs.value.emplace<CNode>();
                    break;

                case 86:  // projectionFieldname
                case 87:  // expressionFieldname
                case 88:  // stageAsUserFieldname
                case 89:  // argAsUserFieldname
                case 90:  // aggExprAsUserFieldname
                case 91:  // invariableUserFieldname
                case 92:  // idAsUserFieldname
                case 93:  // valueFieldname
                    yylhs.value.emplace<CNode::Fieldname>();
                    break;

                case 71:  // DATE_LITERAL
                    yylhs.value.emplace<Date_t>();
                    break;

                case 82:  // DECIMAL_NON_ZERO
                    yylhs.value.emplace<Decimal128>();
                    break;

                case 70:  // OBJECT_ID
                    yylhs.value.emplace<OID>();
                    break;

                case 79:  // TIMESTAMP
                    yylhs.value.emplace<Timestamp>();
                    break;

                case 84:  // MAX_KEY
                    yylhs.value.emplace<UserMaxKey>();
                    break;

                case 83:  // MIN_KEY
                    yylhs.value.emplace<UserMinKey>();
                    break;

                case 72:  // JSNULL
                    yylhs.value.emplace<UserNull>();
                    break;

                case 69:  // UNDEFINED
                    yylhs.value.emplace<UserUndefined>();
                    break;

                case 81:  // DOUBLE_NON_ZERO
                    yylhs.value.emplace<double>();
                    break;

                case 78:  // INT_NON_ZERO
                    yylhs.value.emplace<int>();
                    break;

                case 80:  // LONG_NON_ZERO
                    yylhs.value.emplace<long long>();
                    break;

                case 94:   // projectField
                case 95:   // expressionField
                case 96:   // valueField
                case 184:  // onErrorArg
                case 185:  // onNullArg
                    yylhs.value.emplace<std::pair<CNode::Fieldname, CNode>>();
                    break;

                case 66:  // FIELDNAME
                case 67:  // STRING
                    yylhs.value.emplace<std::string>();
                    break;

                case 186:  // expressions
                case 187:  // values
                    yylhs.value.emplace<std::vector<CNode>>();
                    break;

                default:
                    break;
            }


            // Default location.
            {
                stack_type::slice range(yystack_, yylen);
                YYLLOC_DEFAULT(yylhs.location, range, yylen);
                yyerror_range[1].location = yylhs.location;
            }

            // Perform the reduction.
            YY_REDUCE_PRINT(yyn);
#if YY_EXCEPTIONS
            try
#endif  // YY_EXCEPTIONS
            {
                switch (yyn) {
                    case 2:
#line 246 "pipeline_grammar.yy"
                    {
                        *cst = YY_MOVE(yystack_[1].value.as<CNode>());
                    }
#line 1488 "pipeline_parser_gen.cpp"
                    break;

                    case 3:
#line 252 "pipeline_grammar.yy"
                    {
                    }
#line 1494 "pipeline_parser_gen.cpp"
                    break;

                    case 4:
#line 253 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() =
                            CNode{CNode::ArrayChildren{YY_MOVE(yystack_[2].value.as<CNode>())}};
                    }
#line 1502 "pipeline_parser_gen.cpp"
                    break;

                    case 5:
#line 261 "pipeline_grammar.yy"
                    {
                        lexer.sortObjTokens();
                    }
#line 1508 "pipeline_parser_gen.cpp"
                    break;

                    case 7:
#line 264 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 1514 "pipeline_parser_gen.cpp"
                    break;

                    case 8:
#line 264 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 1520 "pipeline_parser_gen.cpp"
                    break;

                    case 9:
#line 264 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 1526 "pipeline_parser_gen.cpp"
                    break;

                    case 10:
#line 264 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 1532 "pipeline_parser_gen.cpp"
                    break;

                    case 11:
#line 264 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 1538 "pipeline_parser_gen.cpp"
                    break;

                    case 12:
#line 264 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 1544 "pipeline_parser_gen.cpp"
                    break;

                    case 13:
#line 267 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{std::pair{
                            KeyFieldname::sample,
                            CNode{CNode::ObjectChildren{
                                {KeyFieldname::sizeArg, YY_MOVE(yystack_[1].value.as<CNode>())},
                            }}}}};
                    }
#line 1556 "pipeline_parser_gen.cpp"
                    break;

                    case 14:
#line 277 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            std::pair{KeyFieldname::inhibitOptimization, CNode::noopLeaf()}}};
                    }
#line 1564 "pipeline_parser_gen.cpp"
                    break;

                    case 15:
#line 283 "pipeline_grammar.yy"
                    {
                        auto pipeline = YY_MOVE(yystack_[1].value.as<CNode>());
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{std::pair{
                            KeyFieldname::unionWith,
                            CNode{CNode::ObjectChildren{
                                {KeyFieldname::collArg, YY_MOVE(yystack_[3].value.as<CNode>())},
                                {KeyFieldname::pipelineArg, std::move(pipeline)}}}}}};
                    }
#line 1577 "pipeline_parser_gen.cpp"
                    break;

                    case 16:
#line 293 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 1583 "pipeline_parser_gen.cpp"
                    break;

                    case 17:
#line 293 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 1589 "pipeline_parser_gen.cpp"
                    break;

                    case 18:
#line 293 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 1595 "pipeline_parser_gen.cpp"
                    break;

                    case 19:
#line 293 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 1601 "pipeline_parser_gen.cpp"
                    break;

                    case 20:
#line 297 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            std::pair{KeyFieldname::skip, YY_MOVE(yystack_[0].value.as<CNode>())}}};
                    }
#line 1609 "pipeline_parser_gen.cpp"
                    break;

                    case 21:
#line 302 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{std::pair{
                            KeyFieldname::limit, YY_MOVE(yystack_[0].value.as<CNode>())}}};
                    }
#line 1617 "pipeline_parser_gen.cpp"
                    break;

                    case 22:
#line 307 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{std::pair{
                            KeyFieldname::project, YY_MOVE(yystack_[1].value.as<CNode>())}}};
                    }
#line 1625 "pipeline_parser_gen.cpp"
                    break;

                    case 23:
#line 313 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode::noopLeaf();
                    }
#line 1633 "pipeline_parser_gen.cpp"
                    break;

                    case 24:
#line 316 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[1].value.as<CNode>());
                        yylhs.value.as<CNode>().objectChildren().emplace_back(
                            YY_MOVE(yystack_[0].value.as<std::pair<CNode::Fieldname, CNode>>()));
                    }
#line 1642 "pipeline_parser_gen.cpp"
                    break;

                    case 25:
#line 323 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<std::pair<CNode::Fieldname, CNode>>() = {
                            KeyFieldname::id, YY_MOVE(yystack_[0].value.as<CNode>())};
                    }
#line 1650 "pipeline_parser_gen.cpp"
                    break;

                    case 26:
#line 326 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<std::pair<CNode::Fieldname, CNode>>() = {
                            YY_MOVE(yystack_[1].value.as<CNode::Fieldname>()),
                            YY_MOVE(yystack_[0].value.as<CNode>())};
                    }
#line 1658 "pipeline_parser_gen.cpp"
                    break;

                    case 27:
#line 332 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 1664 "pipeline_parser_gen.cpp"
                    break;

                    case 28:
#line 333 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() =
                            CNode{NonZeroKey{YY_MOVE(yystack_[0].value.as<int>())}};
                    }
#line 1672 "pipeline_parser_gen.cpp"
                    break;

                    case 29:
#line 336 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{KeyValue::intZeroKey};
                    }
#line 1680 "pipeline_parser_gen.cpp"
                    break;

                    case 30:
#line 339 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() =
                            CNode{NonZeroKey{YY_MOVE(yystack_[0].value.as<long long>())}};
                    }
#line 1688 "pipeline_parser_gen.cpp"
                    break;

                    case 31:
#line 342 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{KeyValue::longZeroKey};
                    }
#line 1696 "pipeline_parser_gen.cpp"
                    break;

                    case 32:
#line 345 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() =
                            CNode{NonZeroKey{YY_MOVE(yystack_[0].value.as<double>())}};
                    }
#line 1704 "pipeline_parser_gen.cpp"
                    break;

                    case 33:
#line 348 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{KeyValue::doubleZeroKey};
                    }
#line 1712 "pipeline_parser_gen.cpp"
                    break;

                    case 34:
#line 351 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() =
                            CNode{NonZeroKey{YY_MOVE(yystack_[0].value.as<Decimal128>())}};
                    }
#line 1720 "pipeline_parser_gen.cpp"
                    break;

                    case 35:
#line 354 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{KeyValue::decimalZeroKey};
                    }
#line 1728 "pipeline_parser_gen.cpp"
                    break;

                    case 36:
#line 357 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{KeyValue::trueKey};
                    }
#line 1736 "pipeline_parser_gen.cpp"
                    break;

                    case 37:
#line 360 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{KeyValue::falseKey};
                    }
#line 1744 "pipeline_parser_gen.cpp"
                    break;

                    case 38:
#line 363 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 1750 "pipeline_parser_gen.cpp"
                    break;

                    case 39:
#line 367 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() =
                            YY_MOVE(yystack_[0].value.as<CNode::Fieldname>());
                    }
#line 1756 "pipeline_parser_gen.cpp"
                    break;

                    case 40:
#line 367 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() =
                            YY_MOVE(yystack_[0].value.as<CNode::Fieldname>());
                    }
#line 1762 "pipeline_parser_gen.cpp"
                    break;

                    case 41:
#line 367 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() =
                            YY_MOVE(yystack_[0].value.as<CNode::Fieldname>());
                    }
#line 1768 "pipeline_parser_gen.cpp"
                    break;

                    case 42:
#line 367 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() =
                            YY_MOVE(yystack_[0].value.as<CNode::Fieldname>());
                    }
#line 1774 "pipeline_parser_gen.cpp"
                    break;

                    case 43:
#line 371 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() =
                            UserFieldname{YY_MOVE(yystack_[0].value.as<std::string>())};
                    }
#line 1782 "pipeline_parser_gen.cpp"
                    break;

                    case 44:
#line 379 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() =
                            UserFieldname{"$_internalInhibitOptimization"};
                    }
#line 1790 "pipeline_parser_gen.cpp"
                    break;

                    case 45:
#line 382 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$unionWith"};
                    }
#line 1798 "pipeline_parser_gen.cpp"
                    break;

                    case 46:
#line 385 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$skip"};
                    }
#line 1806 "pipeline_parser_gen.cpp"
                    break;

                    case 47:
#line 388 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$limit"};
                    }
#line 1814 "pipeline_parser_gen.cpp"
                    break;

                    case 48:
#line 391 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$project"};
                    }
#line 1822 "pipeline_parser_gen.cpp"
                    break;

                    case 49:
#line 394 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$sample"};
                    }
#line 1830 "pipeline_parser_gen.cpp"
                    break;

                    case 50:
#line 403 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"coll"};
                    }
#line 1838 "pipeline_parser_gen.cpp"
                    break;

                    case 51:
#line 406 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"pipeline"};
                    }
#line 1846 "pipeline_parser_gen.cpp"
                    break;

                    case 52:
#line 409 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"size"};
                    }
#line 1854 "pipeline_parser_gen.cpp"
                    break;

                    case 53:
#line 412 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"input"};
                    }
#line 1862 "pipeline_parser_gen.cpp"
                    break;

                    case 54:
#line 415 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"to"};
                    }
#line 1870 "pipeline_parser_gen.cpp"
                    break;

                    case 55:
#line 418 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"onError"};
                    }
#line 1878 "pipeline_parser_gen.cpp"
                    break;

                    case 56:
#line 421 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"onNull"};
                    }
#line 1886 "pipeline_parser_gen.cpp"
                    break;

                    case 57:
#line 429 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$add"};
                    }
#line 1894 "pipeline_parser_gen.cpp"
                    break;

                    case 58:
#line 432 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$atan2"};
                    }
#line 1902 "pipeline_parser_gen.cpp"
                    break;

                    case 59:
#line 435 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$and"};
                    }
#line 1910 "pipeline_parser_gen.cpp"
                    break;

                    case 60:
#line 438 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$const"};
                    }
#line 1918 "pipeline_parser_gen.cpp"
                    break;

                    case 61:
#line 441 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$literal"};
                    }
#line 1926 "pipeline_parser_gen.cpp"
                    break;

                    case 62:
#line 444 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$or"};
                    }
#line 1934 "pipeline_parser_gen.cpp"
                    break;

                    case 63:
#line 447 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$not"};
                    }
#line 1942 "pipeline_parser_gen.cpp"
                    break;

                    case 64:
#line 450 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$cmp"};
                    }
#line 1950 "pipeline_parser_gen.cpp"
                    break;

                    case 65:
#line 453 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$eq"};
                    }
#line 1958 "pipeline_parser_gen.cpp"
                    break;

                    case 66:
#line 456 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$gt"};
                    }
#line 1966 "pipeline_parser_gen.cpp"
                    break;

                    case 67:
#line 459 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$gte"};
                    }
#line 1974 "pipeline_parser_gen.cpp"
                    break;

                    case 68:
#line 462 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$lt"};
                    }
#line 1982 "pipeline_parser_gen.cpp"
                    break;

                    case 69:
#line 465 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$lte"};
                    }
#line 1990 "pipeline_parser_gen.cpp"
                    break;

                    case 70:
#line 468 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$ne"};
                    }
#line 1998 "pipeline_parser_gen.cpp"
                    break;

                    case 71:
#line 471 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$convert"};
                    }
#line 2006 "pipeline_parser_gen.cpp"
                    break;

                    case 72:
#line 474 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$toBool"};
                    }
#line 2014 "pipeline_parser_gen.cpp"
                    break;

                    case 73:
#line 477 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$toDate"};
                    }
#line 2022 "pipeline_parser_gen.cpp"
                    break;

                    case 74:
#line 480 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$toDecimal"};
                    }
#line 2030 "pipeline_parser_gen.cpp"
                    break;

                    case 75:
#line 483 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$toDouble"};
                    }
#line 2038 "pipeline_parser_gen.cpp"
                    break;

                    case 76:
#line 486 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$toInt"};
                    }
#line 2046 "pipeline_parser_gen.cpp"
                    break;

                    case 77:
#line 489 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$toLong"};
                    }
#line 2054 "pipeline_parser_gen.cpp"
                    break;

                    case 78:
#line 492 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$toObjectId"};
                    }
#line 2062 "pipeline_parser_gen.cpp"
                    break;

                    case 79:
#line 495 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$toString"};
                    }
#line 2070 "pipeline_parser_gen.cpp"
                    break;

                    case 80:
#line 498 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$type"};
                    }
#line 2078 "pipeline_parser_gen.cpp"
                    break;

                    case 81:
#line 501 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$abs"};
                    }
#line 2086 "pipeline_parser_gen.cpp"
                    break;

                    case 82:
#line 504 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$ceil"};
                    }
#line 2094 "pipeline_parser_gen.cpp"
                    break;

                    case 83:
#line 507 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$divide"};
                    }
#line 2102 "pipeline_parser_gen.cpp"
                    break;

                    case 84:
#line 510 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$exp"};
                    }
#line 2110 "pipeline_parser_gen.cpp"
                    break;

                    case 85:
#line 513 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$floor"};
                    }
#line 2118 "pipeline_parser_gen.cpp"
                    break;

                    case 86:
#line 516 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$ln"};
                    }
#line 2126 "pipeline_parser_gen.cpp"
                    break;

                    case 87:
#line 519 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$log"};
                    }
#line 2134 "pipeline_parser_gen.cpp"
                    break;

                    case 88:
#line 522 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$log10"};
                    }
#line 2142 "pipeline_parser_gen.cpp"
                    break;

                    case 89:
#line 525 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$mod"};
                    }
#line 2150 "pipeline_parser_gen.cpp"
                    break;

                    case 90:
#line 528 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$multiply"};
                    }
#line 2158 "pipeline_parser_gen.cpp"
                    break;

                    case 91:
#line 531 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$pow"};
                    }
#line 2166 "pipeline_parser_gen.cpp"
                    break;

                    case 92:
#line 534 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$round"};
                    }
#line 2174 "pipeline_parser_gen.cpp"
                    break;

                    case 93:
#line 537 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$sqrt"};
                    }
#line 2182 "pipeline_parser_gen.cpp"
                    break;

                    case 94:
#line 540 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$subtract"};
                    }
#line 2190 "pipeline_parser_gen.cpp"
                    break;

                    case 95:
#line 543 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$trunc"};
                    }
#line 2198 "pipeline_parser_gen.cpp"
                    break;

                    case 96:
#line 550 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() =
                            CNode{UserString{YY_MOVE(yystack_[0].value.as<std::string>())}};
                    }
#line 2206 "pipeline_parser_gen.cpp"
                    break;

                    case 97:
#line 556 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() =
                            CNode{UserBinary{YY_MOVE(yystack_[0].value.as<BSONBinData>())}};
                    }
#line 2214 "pipeline_parser_gen.cpp"
                    break;

                    case 98:
#line 562 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{UserUndefined{}};
                    }
#line 2222 "pipeline_parser_gen.cpp"
                    break;

                    case 99:
#line 568 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{UserObjectId{}};
                    }
#line 2230 "pipeline_parser_gen.cpp"
                    break;

                    case 100:
#line 574 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() =
                            CNode{UserDate{YY_MOVE(yystack_[0].value.as<Date_t>())}};
                    }
#line 2238 "pipeline_parser_gen.cpp"
                    break;

                    case 101:
#line 580 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{UserNull{}};
                    }
#line 2246 "pipeline_parser_gen.cpp"
                    break;

                    case 102:
#line 586 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() =
                            CNode{UserRegex{YY_MOVE(yystack_[0].value.as<BSONRegEx>())}};
                    }
#line 2254 "pipeline_parser_gen.cpp"
                    break;

                    case 103:
#line 592 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() =
                            CNode{UserDBPointer{YY_MOVE(yystack_[0].value.as<BSONDBRef>())}};
                    }
#line 2262 "pipeline_parser_gen.cpp"
                    break;

                    case 104:
#line 598 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() =
                            CNode{UserJavascript{YY_MOVE(yystack_[0].value.as<BSONCode>())}};
                    }
#line 2270 "pipeline_parser_gen.cpp"
                    break;

                    case 105:
#line 604 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() =
                            CNode{UserSymbol{YY_MOVE(yystack_[0].value.as<BSONSymbol>())}};
                    }
#line 2278 "pipeline_parser_gen.cpp"
                    break;

                    case 106:
#line 610 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{UserJavascriptWithScope{
                            YY_MOVE(yystack_[0].value.as<BSONCodeWScope>())}};
                    }
#line 2286 "pipeline_parser_gen.cpp"
                    break;

                    case 107:
#line 616 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() =
                            CNode{UserTimestamp{YY_MOVE(yystack_[0].value.as<Timestamp>())}};
                    }
#line 2294 "pipeline_parser_gen.cpp"
                    break;

                    case 108:
#line 622 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() =
                            CNode{UserMinKey{YY_MOVE(yystack_[0].value.as<UserMinKey>())}};
                    }
#line 2302 "pipeline_parser_gen.cpp"
                    break;

                    case 109:
#line 628 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() =
                            CNode{UserMaxKey{YY_MOVE(yystack_[0].value.as<UserMaxKey>())}};
                    }
#line 2310 "pipeline_parser_gen.cpp"
                    break;

                    case 110:
#line 634 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() =
                            CNode{UserInt{YY_MOVE(yystack_[0].value.as<int>())}};
                    }
#line 2318 "pipeline_parser_gen.cpp"
                    break;

                    case 111:
#line 637 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{UserInt{0}};
                    }
#line 2326 "pipeline_parser_gen.cpp"
                    break;

                    case 112:
#line 643 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() =
                            CNode{UserLong{YY_MOVE(yystack_[0].value.as<long long>())}};
                    }
#line 2334 "pipeline_parser_gen.cpp"
                    break;

                    case 113:
#line 646 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{UserLong{0ll}};
                    }
#line 2342 "pipeline_parser_gen.cpp"
                    break;

                    case 114:
#line 652 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() =
                            CNode{UserDouble{YY_MOVE(yystack_[0].value.as<double>())}};
                    }
#line 2350 "pipeline_parser_gen.cpp"
                    break;

                    case 115:
#line 655 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{UserDouble{0.0}};
                    }
#line 2358 "pipeline_parser_gen.cpp"
                    break;

                    case 116:
#line 661 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() =
                            CNode{UserDecimal{YY_MOVE(yystack_[0].value.as<Decimal128>())}};
                    }
#line 2366 "pipeline_parser_gen.cpp"
                    break;

                    case 117:
#line 664 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{UserDecimal{0.0}};
                    }
#line 2374 "pipeline_parser_gen.cpp"
                    break;

                    case 118:
#line 670 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{UserBoolean{true}};
                    }
#line 2382 "pipeline_parser_gen.cpp"
                    break;

                    case 119:
#line 673 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{UserBoolean{false}};
                    }
#line 2390 "pipeline_parser_gen.cpp"
                    break;

                    case 120:
#line 679 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 2396 "pipeline_parser_gen.cpp"
                    break;

                    case 121:
#line 680 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 2402 "pipeline_parser_gen.cpp"
                    break;

                    case 122:
#line 681 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 2408 "pipeline_parser_gen.cpp"
                    break;

                    case 123:
#line 682 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 2414 "pipeline_parser_gen.cpp"
                    break;

                    case 124:
#line 683 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 2420 "pipeline_parser_gen.cpp"
                    break;

                    case 125:
#line 684 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 2426 "pipeline_parser_gen.cpp"
                    break;

                    case 126:
#line 685 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 2432 "pipeline_parser_gen.cpp"
                    break;

                    case 127:
#line 686 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 2438 "pipeline_parser_gen.cpp"
                    break;

                    case 128:
#line 687 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 2444 "pipeline_parser_gen.cpp"
                    break;

                    case 129:
#line 688 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 2450 "pipeline_parser_gen.cpp"
                    break;

                    case 130:
#line 689 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 2456 "pipeline_parser_gen.cpp"
                    break;

                    case 131:
#line 690 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 2462 "pipeline_parser_gen.cpp"
                    break;

                    case 132:
#line 691 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 2468 "pipeline_parser_gen.cpp"
                    break;

                    case 133:
#line 692 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 2474 "pipeline_parser_gen.cpp"
                    break;

                    case 134:
#line 693 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 2480 "pipeline_parser_gen.cpp"
                    break;

                    case 135:
#line 694 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 2486 "pipeline_parser_gen.cpp"
                    break;

                    case 136:
#line 695 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 2492 "pipeline_parser_gen.cpp"
                    break;

                    case 137:
#line 696 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 2498 "pipeline_parser_gen.cpp"
                    break;

                    case 138:
#line 697 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 2504 "pipeline_parser_gen.cpp"
                    break;

                    case 139:
#line 704 "pipeline_grammar.yy"
                    {
                    }
#line 2510 "pipeline_parser_gen.cpp"
                    break;

                    case 140:
#line 705 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<std::vector<CNode>>() =
                            YY_MOVE(yystack_[0].value.as<std::vector<CNode>>());
                        yylhs.value.as<std::vector<CNode>>().emplace_back(
                            YY_MOVE(yystack_[1].value.as<CNode>()));
                    }
#line 2519 "pipeline_parser_gen.cpp"
                    break;

                    case 141:
#line 712 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 2525 "pipeline_parser_gen.cpp"
                    break;

                    case 142:
#line 712 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 2531 "pipeline_parser_gen.cpp"
                    break;

                    case 143:
#line 716 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() =
                            CNode{CNode::ArrayChildren{YY_MOVE(yystack_[2].value.as<CNode>()),
                                                       YY_MOVE(yystack_[1].value.as<CNode>())}};
                    }
#line 2539 "pipeline_parser_gen.cpp"
                    break;

                    case 144:
#line 721 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 2545 "pipeline_parser_gen.cpp"
                    break;

                    case 145:
#line 721 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 2551 "pipeline_parser_gen.cpp"
                    break;

                    case 146:
#line 721 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 2557 "pipeline_parser_gen.cpp"
                    break;

                    case 147:
#line 721 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 2563 "pipeline_parser_gen.cpp"
                    break;

                    case 148:
#line 721 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 2569 "pipeline_parser_gen.cpp"
                    break;

                    case 149:
#line 721 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 2575 "pipeline_parser_gen.cpp"
                    break;

                    case 150:
#line 722 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 2581 "pipeline_parser_gen.cpp"
                    break;

                    case 151:
#line 728 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() =
                            CNode{YY_MOVE(yystack_[1].value.as<std::vector<CNode>>())};
                    }
#line 2589 "pipeline_parser_gen.cpp"
                    break;

                    case 152:
#line 736 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[1].value.as<CNode>());
                    }
#line 2597 "pipeline_parser_gen.cpp"
                    break;

                    case 153:
#line 742 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode::noopLeaf();
                    }
#line 2605 "pipeline_parser_gen.cpp"
                    break;

                    case 154:
#line 745 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[1].value.as<CNode>());
                        yylhs.value.as<CNode>().objectChildren().emplace_back(
                            YY_MOVE(yystack_[0].value.as<std::pair<CNode::Fieldname, CNode>>()));
                    }
#line 2614 "pipeline_parser_gen.cpp"
                    break;

                    case 155:
#line 752 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<std::pair<CNode::Fieldname, CNode>>() = {
                            YY_MOVE(yystack_[1].value.as<CNode::Fieldname>()),
                            YY_MOVE(yystack_[0].value.as<CNode>())};
                    }
#line 2622 "pipeline_parser_gen.cpp"
                    break;

                    case 156:
#line 759 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() =
                            YY_MOVE(yystack_[0].value.as<CNode::Fieldname>());
                    }
#line 2628 "pipeline_parser_gen.cpp"
                    break;

                    case 157:
#line 759 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() =
                            YY_MOVE(yystack_[0].value.as<CNode::Fieldname>());
                    }
#line 2634 "pipeline_parser_gen.cpp"
                    break;

                    case 158:
#line 759 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() =
                            YY_MOVE(yystack_[0].value.as<CNode::Fieldname>());
                    }
#line 2640 "pipeline_parser_gen.cpp"
                    break;

                    case 159:
#line 759 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() =
                            YY_MOVE(yystack_[0].value.as<CNode::Fieldname>());
                    }
#line 2646 "pipeline_parser_gen.cpp"
                    break;

                    case 160:
#line 763 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"_id"};
                    }
#line 2654 "pipeline_parser_gen.cpp"
                    break;

                    case 161:
#line 769 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 2660 "pipeline_parser_gen.cpp"
                    break;

                    case 162:
#line 769 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 2666 "pipeline_parser_gen.cpp"
                    break;

                    case 163:
#line 769 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 2672 "pipeline_parser_gen.cpp"
                    break;

                    case 164:
#line 769 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 2678 "pipeline_parser_gen.cpp"
                    break;

                    case 165:
#line 769 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 2684 "pipeline_parser_gen.cpp"
                    break;

                    case 166:
#line 769 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 2690 "pipeline_parser_gen.cpp"
                    break;

                    case 167:
#line 769 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 2696 "pipeline_parser_gen.cpp"
                    break;

                    case 168:
#line 769 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 2702 "pipeline_parser_gen.cpp"
                    break;

                    case 169:
#line 769 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 2708 "pipeline_parser_gen.cpp"
                    break;

                    case 170:
#line 769 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 2714 "pipeline_parser_gen.cpp"
                    break;

                    case 171:
#line 769 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 2720 "pipeline_parser_gen.cpp"
                    break;

                    case 172:
#line 769 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 2726 "pipeline_parser_gen.cpp"
                    break;

                    case 173:
#line 769 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 2732 "pipeline_parser_gen.cpp"
                    break;

                    case 174:
#line 770 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 2738 "pipeline_parser_gen.cpp"
                    break;

                    case 175:
#line 770 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 2744 "pipeline_parser_gen.cpp"
                    break;

                    case 176:
#line 770 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 2750 "pipeline_parser_gen.cpp"
                    break;

                    case 177:
#line 770 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 2756 "pipeline_parser_gen.cpp"
                    break;

                    case 178:
#line 774 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::add,
                             CNode{CNode::ArrayChildren{YY_MOVE(yystack_[4].value.as<CNode>()),
                                                        YY_MOVE(yystack_[3].value.as<CNode>())}}}}};
                        auto&& others = YY_MOVE(yystack_[2].value.as<std::vector<CNode>>());
                        auto&& array =
                            yylhs.value.as<CNode>().objectChildren()[0].second.arrayChildren();
                        array.insert(array.end(), others.begin(), others.end());
                    }
#line 2768 "pipeline_parser_gen.cpp"
                    break;

                    case 179:
#line 784 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::atan2, YY_MOVE(yystack_[1].value.as<CNode>())}}};
                    }
#line 2777 "pipeline_parser_gen.cpp"
                    break;

                    case 180:
#line 790 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::abs, YY_MOVE(yystack_[1].value.as<CNode>())}}};
                    }
#line 2785 "pipeline_parser_gen.cpp"
                    break;

                    case 181:
#line 795 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::ceil, YY_MOVE(yystack_[1].value.as<CNode>())}}};
                    }
#line 2793 "pipeline_parser_gen.cpp"
                    break;

                    case 182:
#line 800 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::divide,
                             CNode{CNode::ArrayChildren{YY_MOVE(yystack_[3].value.as<CNode>()),
                                                        YY_MOVE(yystack_[2].value.as<CNode>())}}}}};
                    }
#line 2802 "pipeline_parser_gen.cpp"
                    break;

                    case 183:
#line 806 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::exponent, YY_MOVE(yystack_[1].value.as<CNode>())}}};
                    }
#line 2810 "pipeline_parser_gen.cpp"
                    break;

                    case 184:
#line 811 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::floor, YY_MOVE(yystack_[1].value.as<CNode>())}}};
                    }
#line 2818 "pipeline_parser_gen.cpp"
                    break;

                    case 185:
#line 816 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::ln, YY_MOVE(yystack_[1].value.as<CNode>())}}};
                    }
#line 2826 "pipeline_parser_gen.cpp"
                    break;

                    case 186:
#line 821 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::log,
                             CNode{CNode::ArrayChildren{YY_MOVE(yystack_[3].value.as<CNode>()),
                                                        YY_MOVE(yystack_[2].value.as<CNode>())}}}}};
                    }
#line 2835 "pipeline_parser_gen.cpp"
                    break;

                    case 187:
#line 827 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::logten, YY_MOVE(yystack_[1].value.as<CNode>())}}};
                    }
#line 2843 "pipeline_parser_gen.cpp"
                    break;

                    case 188:
#line 832 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::mod,
                             CNode{CNode::ArrayChildren{YY_MOVE(yystack_[3].value.as<CNode>()),
                                                        YY_MOVE(yystack_[2].value.as<CNode>())}}}}};
                    }
#line 2852 "pipeline_parser_gen.cpp"
                    break;

                    case 189:
#line 838 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::multiply,
                             CNode{CNode::ArrayChildren{YY_MOVE(yystack_[4].value.as<CNode>()),
                                                        YY_MOVE(yystack_[3].value.as<CNode>())}}}}};
                        auto&& others = YY_MOVE(yystack_[2].value.as<std::vector<CNode>>());
                        auto&& array =
                            yylhs.value.as<CNode>().objectChildren()[0].second.arrayChildren();
                        array.insert(array.end(), others.begin(), others.end());
                    }
#line 2864 "pipeline_parser_gen.cpp"
                    break;

                    case 190:
#line 847 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::pow,
                             CNode{CNode::ArrayChildren{YY_MOVE(yystack_[3].value.as<CNode>()),
                                                        YY_MOVE(yystack_[2].value.as<CNode>())}}}}};
                    }
#line 2873 "pipeline_parser_gen.cpp"
                    break;

                    case 191:
#line 853 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::round,
                             CNode{CNode::ArrayChildren{YY_MOVE(yystack_[3].value.as<CNode>()),
                                                        YY_MOVE(yystack_[2].value.as<CNode>())}}}}};
                    }
#line 2882 "pipeline_parser_gen.cpp"
                    break;

                    case 192:
#line 859 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::sqrt, YY_MOVE(yystack_[1].value.as<CNode>())}}};
                    }
#line 2890 "pipeline_parser_gen.cpp"
                    break;

                    case 193:
#line 864 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::subtract,
                             CNode{CNode::ArrayChildren{YY_MOVE(yystack_[3].value.as<CNode>()),
                                                        YY_MOVE(yystack_[2].value.as<CNode>())}}}}};
                    }
#line 2899 "pipeline_parser_gen.cpp"
                    break;

                    case 194:
#line 870 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::trunc,
                             CNode{CNode::ArrayChildren{YY_MOVE(yystack_[3].value.as<CNode>()),
                                                        YY_MOVE(yystack_[2].value.as<CNode>())}}}}};
                    }
#line 2908 "pipeline_parser_gen.cpp"
                    break;

                    case 195:
#line 876 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 2914 "pipeline_parser_gen.cpp"
                    break;

                    case 196:
#line 876 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 2920 "pipeline_parser_gen.cpp"
                    break;

                    case 197:
#line 876 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 2926 "pipeline_parser_gen.cpp"
                    break;

                    case 198:
#line 880 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::andExpr,
                             CNode{CNode::ArrayChildren{YY_MOVE(yystack_[4].value.as<CNode>()),
                                                        YY_MOVE(yystack_[3].value.as<CNode>())}}}}};
                        auto&& others = YY_MOVE(yystack_[2].value.as<std::vector<CNode>>());
                        auto&& array =
                            yylhs.value.as<CNode>().objectChildren()[0].second.arrayChildren();
                        array.insert(array.end(), others.begin(), others.end());
                    }
#line 2938 "pipeline_parser_gen.cpp"
                    break;

                    case 199:
#line 890 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::orExpr,
                             CNode{CNode::ArrayChildren{YY_MOVE(yystack_[4].value.as<CNode>()),
                                                        YY_MOVE(yystack_[3].value.as<CNode>())}}}}};
                        auto&& others = YY_MOVE(yystack_[2].value.as<std::vector<CNode>>());
                        auto&& array =
                            yylhs.value.as<CNode>().objectChildren()[0].second.arrayChildren();
                        array.insert(array.end(), others.begin(), others.end());
                    }
#line 2950 "pipeline_parser_gen.cpp"
                    break;

                    case 200:
#line 900 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::notExpr,
                             CNode{CNode::ArrayChildren{YY_MOVE(yystack_[2].value.as<CNode>())}}}}};
                    }
#line 2959 "pipeline_parser_gen.cpp"
                    break;

                    case 201:
#line 907 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 2965 "pipeline_parser_gen.cpp"
                    break;

                    case 202:
#line 907 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 2971 "pipeline_parser_gen.cpp"
                    break;

                    case 203:
#line 911 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::constExpr,
                             CNode{CNode::ArrayChildren{YY_MOVE(yystack_[2].value.as<CNode>())}}}}};
                    }
#line 2980 "pipeline_parser_gen.cpp"
                    break;

                    case 204:
#line 918 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::literal,
                             CNode{CNode::ArrayChildren{YY_MOVE(yystack_[2].value.as<CNode>())}}}}};
                    }
#line 2989 "pipeline_parser_gen.cpp"
                    break;

                    case 205:
#line 925 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 2995 "pipeline_parser_gen.cpp"
                    break;

                    case 206:
#line 925 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 3001 "pipeline_parser_gen.cpp"
                    break;

                    case 207:
#line 929 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 3007 "pipeline_parser_gen.cpp"
                    break;

                    case 208:
#line 929 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 3013 "pipeline_parser_gen.cpp"
                    break;

                    case 209:
#line 933 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() =
                            CNode{YY_MOVE(yystack_[1].value.as<std::vector<CNode>>())};
                    }
#line 3021 "pipeline_parser_gen.cpp"
                    break;

                    case 210:
#line 939 "pipeline_grammar.yy"
                    {
                    }
#line 3027 "pipeline_parser_gen.cpp"
                    break;

                    case 211:
#line 940 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<std::vector<CNode>>() =
                            YY_MOVE(yystack_[0].value.as<std::vector<CNode>>());
                        yylhs.value.as<std::vector<CNode>>().emplace_back(
                            YY_MOVE(yystack_[1].value.as<CNode>()));
                    }
#line 3036 "pipeline_parser_gen.cpp"
                    break;

                    case 212:
#line 947 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[1].value.as<CNode>());
                    }
#line 3044 "pipeline_parser_gen.cpp"
                    break;

                    case 213:
#line 953 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode::noopLeaf();
                    }
#line 3052 "pipeline_parser_gen.cpp"
                    break;

                    case 214:
#line 956 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[1].value.as<CNode>());
                        yylhs.value.as<CNode>().objectChildren().emplace_back(
                            YY_MOVE(yystack_[0].value.as<std::pair<CNode::Fieldname, CNode>>()));
                    }
#line 3061 "pipeline_parser_gen.cpp"
                    break;

                    case 215:
#line 963 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<std::pair<CNode::Fieldname, CNode>>() = {
                            YY_MOVE(yystack_[1].value.as<CNode::Fieldname>()),
                            YY_MOVE(yystack_[0].value.as<CNode>())};
                    }
#line 3069 "pipeline_parser_gen.cpp"
                    break;

                    case 216:
#line 970 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() =
                            YY_MOVE(yystack_[0].value.as<CNode::Fieldname>());
                    }
#line 3075 "pipeline_parser_gen.cpp"
                    break;

                    case 217:
#line 971 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() =
                            YY_MOVE(yystack_[0].value.as<CNode::Fieldname>());
                    }
#line 3081 "pipeline_parser_gen.cpp"
                    break;

                    case 218:
#line 972 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() =
                            YY_MOVE(yystack_[0].value.as<CNode::Fieldname>());
                    }
#line 3087 "pipeline_parser_gen.cpp"
                    break;

                    case 219:
#line 973 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() =
                            YY_MOVE(yystack_[0].value.as<CNode::Fieldname>());
                    }
#line 3093 "pipeline_parser_gen.cpp"
                    break;

                    case 220:
#line 974 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() =
                            YY_MOVE(yystack_[0].value.as<CNode::Fieldname>());
                    }
#line 3099 "pipeline_parser_gen.cpp"
                    break;

                    case 221:
#line 977 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 3105 "pipeline_parser_gen.cpp"
                    break;

                    case 222:
#line 977 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 3111 "pipeline_parser_gen.cpp"
                    break;

                    case 223:
#line 977 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 3117 "pipeline_parser_gen.cpp"
                    break;

                    case 224:
#line 977 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 3123 "pipeline_parser_gen.cpp"
                    break;

                    case 225:
#line 977 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 3129 "pipeline_parser_gen.cpp"
                    break;

                    case 226:
#line 977 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 3135 "pipeline_parser_gen.cpp"
                    break;

                    case 227:
#line 977 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 3141 "pipeline_parser_gen.cpp"
                    break;

                    case 228:
#line 979 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::cmp, YY_MOVE(yystack_[1].value.as<CNode>())}}};
                    }
#line 3150 "pipeline_parser_gen.cpp"
                    break;

                    case 229:
#line 984 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::eq, YY_MOVE(yystack_[1].value.as<CNode>())}}};
                    }
#line 3159 "pipeline_parser_gen.cpp"
                    break;

                    case 230:
#line 989 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::gt, YY_MOVE(yystack_[1].value.as<CNode>())}}};
                    }
#line 3168 "pipeline_parser_gen.cpp"
                    break;

                    case 231:
#line 994 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::gte, YY_MOVE(yystack_[1].value.as<CNode>())}}};
                    }
#line 3177 "pipeline_parser_gen.cpp"
                    break;

                    case 232:
#line 999 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::lt, YY_MOVE(yystack_[1].value.as<CNode>())}}};
                    }
#line 3186 "pipeline_parser_gen.cpp"
                    break;

                    case 233:
#line 1004 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::lte, YY_MOVE(yystack_[1].value.as<CNode>())}}};
                    }
#line 3195 "pipeline_parser_gen.cpp"
                    break;

                    case 234:
#line 1009 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::ne, YY_MOVE(yystack_[1].value.as<CNode>())}}};
                    }
#line 3204 "pipeline_parser_gen.cpp"
                    break;

                    case 235:
#line 1015 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 3210 "pipeline_parser_gen.cpp"
                    break;

                    case 236:
#line 1016 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 3216 "pipeline_parser_gen.cpp"
                    break;

                    case 237:
#line 1017 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 3222 "pipeline_parser_gen.cpp"
                    break;

                    case 238:
#line 1018 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 3228 "pipeline_parser_gen.cpp"
                    break;

                    case 239:
#line 1019 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 3234 "pipeline_parser_gen.cpp"
                    break;

                    case 240:
#line 1020 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 3240 "pipeline_parser_gen.cpp"
                    break;

                    case 241:
#line 1021 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 3246 "pipeline_parser_gen.cpp"
                    break;

                    case 242:
#line 1022 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 3252 "pipeline_parser_gen.cpp"
                    break;

                    case 243:
#line 1023 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 3258 "pipeline_parser_gen.cpp"
                    break;

                    case 244:
#line 1024 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 3264 "pipeline_parser_gen.cpp"
                    break;

                    case 245:
#line 1030 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 3270 "pipeline_parser_gen.cpp"
                    break;

                    case 246:
#line 1030 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 3276 "pipeline_parser_gen.cpp"
                    break;

                    case 247:
#line 1030 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 3282 "pipeline_parser_gen.cpp"
                    break;

                    case 248:
#line 1030 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 3288 "pipeline_parser_gen.cpp"
                    break;

                    case 249:
#line 1030 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 3294 "pipeline_parser_gen.cpp"
                    break;

                    case 250:
#line 1034 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<std::pair<CNode::Fieldname, CNode>>() =
                            std::pair{KeyFieldname::onErrorArg, CNode{KeyValue::absentKey}};
                    }
#line 3302 "pipeline_parser_gen.cpp"
                    break;

                    case 251:
#line 1037 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<std::pair<CNode::Fieldname, CNode>>() = std::pair{
                            KeyFieldname::onErrorArg, YY_MOVE(yystack_[0].value.as<CNode>())};
                    }
#line 3310 "pipeline_parser_gen.cpp"
                    break;

                    case 252:
#line 1044 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<std::pair<CNode::Fieldname, CNode>>() =
                            std::pair{KeyFieldname::onNullArg, CNode{KeyValue::absentKey}};
                    }
#line 3318 "pipeline_parser_gen.cpp"
                    break;

                    case 253:
#line 1047 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<std::pair<CNode::Fieldname, CNode>>() = std::pair{
                            KeyFieldname::onNullArg, YY_MOVE(yystack_[0].value.as<CNode>())};
                    }
#line 3326 "pipeline_parser_gen.cpp"
                    break;

                    case 254:
#line 1053 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::convert,
                             CNode{CNode::ObjectChildren{
                                 {KeyFieldname::inputArg, YY_MOVE(yystack_[6].value.as<CNode>())},
                                 {KeyFieldname::toArg, YY_MOVE(yystack_[4].value.as<CNode>())},
                                 YY_MOVE(
                                     yystack_[3].value.as<std::pair<CNode::Fieldname, CNode>>()),
                                 YY_MOVE(yystack_[2]
                                             .value.as<std::pair<CNode::Fieldname, CNode>>())}}}}};
                    }
#line 3337 "pipeline_parser_gen.cpp"
                    break;

                    case 255:
#line 1062 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::toBool, YY_MOVE(yystack_[1].value.as<CNode>())}}};
                    }
#line 3345 "pipeline_parser_gen.cpp"
                    break;

                    case 256:
#line 1067 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::toDate, YY_MOVE(yystack_[1].value.as<CNode>())}}};
                    }
#line 3353 "pipeline_parser_gen.cpp"
                    break;

                    case 257:
#line 1072 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::toDecimal, YY_MOVE(yystack_[1].value.as<CNode>())}}};
                    }
#line 3361 "pipeline_parser_gen.cpp"
                    break;

                    case 258:
#line 1077 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::toDouble, YY_MOVE(yystack_[1].value.as<CNode>())}}};
                    }
#line 3369 "pipeline_parser_gen.cpp"
                    break;

                    case 259:
#line 1082 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::toInt, YY_MOVE(yystack_[1].value.as<CNode>())}}};
                    }
#line 3377 "pipeline_parser_gen.cpp"
                    break;

                    case 260:
#line 1087 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::toLong, YY_MOVE(yystack_[1].value.as<CNode>())}}};
                    }
#line 3385 "pipeline_parser_gen.cpp"
                    break;

                    case 261:
#line 1092 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::toObjectId, YY_MOVE(yystack_[1].value.as<CNode>())}}};
                    }
#line 3393 "pipeline_parser_gen.cpp"
                    break;

                    case 262:
#line 1097 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::toString, YY_MOVE(yystack_[1].value.as<CNode>())}}};
                    }
#line 3401 "pipeline_parser_gen.cpp"
                    break;

                    case 263:
#line 1102 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::type, YY_MOVE(yystack_[1].value.as<CNode>())}}};
                    }
#line 3409 "pipeline_parser_gen.cpp"
                    break;


#line 3413 "pipeline_parser_gen.cpp"

                    default:
                        break;
                }
            }
#if YY_EXCEPTIONS
            catch (const syntax_error& yyexc) {
                YYCDEBUG << "Caught exception: " << yyexc.what() << '\n';
                error(yyexc);
                YYERROR;
            }
#endif  // YY_EXCEPTIONS
            YY_SYMBOL_PRINT("-> $$ =", yylhs);
            yypop_(yylen);
            yylen = 0;

            // Shift the result of the reduction.
            yypush_(YY_NULLPTR, YY_MOVE(yylhs));
        }
        goto yynewstate;


    /*--------------------------------------.
    | yyerrlab -- here on detecting error.  |
    `--------------------------------------*/
    yyerrlab:
        // If not already recovering from an error, report this error.
        if (!yyerrstatus_) {
            ++yynerrs_;
            std::string msg = YY_("syntax error");
            error(yyla.location, YY_MOVE(msg));
        }


        yyerror_range[1].location = yyla.location;
        if (yyerrstatus_ == 3) {
            /* If just tried and failed to reuse lookahead token after an
               error, discard it.  */

            // Return failure if at end of input.
            if (yyla.kind() == symbol_kind::S_YYEOF)
                YYABORT;
            else if (!yyla.empty()) {
                yy_destroy_("Error: discarding", yyla);
                yyla.clear();
            }
        }

        // Else will try to reuse lookahead token after shifting the error token.
        goto yyerrlab1;


    /*---------------------------------------------------.
    | yyerrorlab -- error raised explicitly by YYERROR.  |
    `---------------------------------------------------*/
    yyerrorlab:
        /* Pacify compilers when the user code never invokes YYERROR and
           the label yyerrorlab therefore never appears in user code.  */
        if (false)
            YYERROR;

        /* Do not reclaim the symbols of the rule whose action triggered
           this YYERROR.  */
        yypop_(yylen);
        yylen = 0;
        YY_STACK_PRINT();
        goto yyerrlab1;


    /*-------------------------------------------------------------.
    | yyerrlab1 -- common code for both syntax error and YYERROR.  |
    `-------------------------------------------------------------*/
    yyerrlab1:
        yyerrstatus_ = 3;  // Each real token shifted decrements this.
        // Pop stack until we find a state that shifts the error token.
        for (;;) {
            yyn = yypact_[+yystack_[0].state];
            if (!yy_pact_value_is_default_(yyn)) {
                yyn += symbol_kind::S_YYerror;
                if (0 <= yyn && yyn <= yylast_ && yycheck_[yyn] == symbol_kind::S_YYerror) {
                    yyn = yytable_[yyn];
                    if (0 < yyn)
                        break;
                }
            }

            // Pop the current state because it cannot handle the error token.
            if (yystack_.size() == 1)
                YYABORT;

            yyerror_range[1].location = yystack_[0].location;
            yy_destroy_("Error: popping", yystack_[0]);
            yypop_();
            YY_STACK_PRINT();
        }
        {
            stack_symbol_type error_token;

            yyerror_range[2].location = yyla.location;
            YYLLOC_DEFAULT(error_token.location, yyerror_range, 2);

            // Shift the error token.
            error_token.state = state_type(yyn);
            yypush_("Shifting", YY_MOVE(error_token));
        }
        goto yynewstate;


    /*-------------------------------------.
    | yyacceptlab -- YYACCEPT comes here.  |
    `-------------------------------------*/
    yyacceptlab:
        yyresult = 0;
        goto yyreturn;


    /*-----------------------------------.
    | yyabortlab -- YYABORT comes here.  |
    `-----------------------------------*/
    yyabortlab:
        yyresult = 1;
        goto yyreturn;


    /*-----------------------------------------------------.
    | yyreturn -- parsing is finished, return the result.  |
    `-----------------------------------------------------*/
    yyreturn:
        if (!yyla.empty())                                       // NOLINT(bugprone-use-after-move)
            yy_destroy_("Cleanup: discarding lookahead", yyla);  // NOLINT(bugprone-use-after-move)

        /* Do not reclaim the symbols of the rule whose action triggered
           this YYABORT or YYACCEPT.  */
        yypop_(yylen);
        YY_STACK_PRINT();
        while (1 < yystack_.size()) {
            yy_destroy_("Cleanup: popping", yystack_[0]);
            yypop_();
        }

        return yyresult;
    }
#if YY_EXCEPTIONS
    catch (...) {
        YYCDEBUG << "Exception caught: cleaning lookahead and stack\n";
        // Do not try to display the values of the reclaimed symbols,
        // as their printers might throw an exception.
        if (!yyla.empty())
            yy_destroy_(YY_NULLPTR, yyla);

        while (1 < yystack_.size()) {
            yy_destroy_(YY_NULLPTR, yystack_[0]);
            yypop_();
        }
        throw;
    }
#endif  // YY_EXCEPTIONS
}

void PipelineParserGen::error(const syntax_error& yyexc) {
    error(yyexc.location, yyexc.what());
}

#if YYDEBUG || 0
const char* PipelineParserGen::symbol_name(symbol_kind_type yysymbol) {
    return yytname_[yysymbol];
}
#endif  // #if YYDEBUG || 0


const short PipelineParserGen::yypact_ninf_ = -226;

const signed char PipelineParserGen::yytable_ninf_ = -1;

const short PipelineParserGen::yypact_[] = {
    35,   39,   44,   77,   41,   -226, 43,   61,   51,   52,   61,   -226, 60,   -226, -226, -226,
    -226, -226, -226, -226, 62,   -226, -226, -226, -226, -226, -226, -226, -226, -226, -226, -226,
    -226, -226, -226, 45,   -226, 48,   70,   39,   -226, 278,  61,   7,    -226, -226, -226, 40,
    -226, -226, -226, -226, -226, -226, -226, -226, -226, -226, -226, -226, -226, -226, -226, -226,
    -226, -226, -226, -226, -226, -226, -226, -226, -226, -226, -226, -226, -226, -226, -226, -226,
    -226, -226, -226, -226, -226, -226, -226, -226, -226, -226, -226, -226, -226, -226, -226, -226,
    -226, -226, -226, -226, -226, 40,   -226, -226, -226, -226, -226, 71,   -226, 55,   487,  179,
    -226, -226, -226, -226, -226, -226, -226, -226, -226, -226, -226, -226, -226, -226, -226, -226,
    -226, -226, -226, -226, -226, -226, -226, -226, -226, -226, -226, -226, -226, -226, -226, -226,
    -226, -226, -226, -226, -226, -226, -226, -226, -226, -226, -226, -226, -226, -226, -226, -226,
    -226, -226, -226, -226, -226, -226, -226, -226, -226, -226, -226, -226, -226, -2,   76,   81,
    82,   83,   84,   92,   93,   81,   81,   81,   81,   81,   81,   81,   -226, 179,  179,  179,
    179,  179,  179,  179,  179,  179,  179,  179,  94,   179,  179,  179,  95,   179,  96,   97,
    98,   100,  179,  101,  104,  443,  -226, -226, -226, -226, -226, -226, -226, -226, -226, -226,
    -226, -226, -226, -226, -226, -226, -226, -226, -226, -226, -226, -226, -226, -226, -226, -226,
    -226, -226, -226, -226, -226, -226, -226, -226, -226, 179,  -226, 108,  78,   179,  179,  113,
    179,  197,  197,  179,  179,  119,  129,  130,  131,  133,  141,  142,  85,   144,  146,  148,
    151,  152,  153,  154,  155,  158,  159,  160,  179,  170,  171,  172,  179,  173,  179,  179,
    179,  179,  174,  179,  179,  -226, -226, 179,  -226, -226, -226, -226, -226, -226, -226, -226,
    179,  179,  -226, 179,  -226, 197,  175,  -226, -226, -226, -226, 180,  179,  187,  -226, -226,
    -226, -226, -226, -226, -226, 179,  -226, -226, -226, -226, -226, -226, -226, -226, -226, -226,
    -226, 179,  -226, -226, -226, 179,  -226, 179,  179,  179,  179,  -226, 179,  179,  -226, 179,
    189,  179,  380,  197,  193,  176,  181,  179,  199,  138,  198,  205,  206,  179,  207,  208,
    209,  210,  211,  -226, 212,  -226, -226, -226, -226, -226, -226, 197,  -226, -226, -226, -226,
    -226, 213,  -226, 116,  216,  217,  218,  219,  220,  222,  223,  224,  225,  226,  -226, 227,
    -226, -226, -226, -226, -226, 115,  -226, -226, -226, 228,  -226, -226, -226, -226, -226, -226,
    -226, 179,  168,  -226, -226, 179,  230,  -226, 231,  -226};

const short PipelineParserGen::yydefact_[] = {
    0,   3,   0,   0,   0,   1,   0,   0,   0,   0,   0,   5,   0,   7,   8,   9,   10,  11,  12,
    2,   0,   111, 113, 115, 117, 110, 112, 114, 116, 16,  17,  18,  19,  21,  23,  0,   20,  0,
    0,   3,   14,  0,   0,   0,   6,   4,   22,  0,   44,  47,  48,  49,  46,  45,  50,  51,  52,
    57,  58,  59,  60,  61,  62,  63,  64,  65,  66,  67,  68,  69,  70,  71,  72,  73,  74,  75,
    76,  77,  78,  79,  80,  81,  82,  83,  84,  85,  86,  87,  88,  89,  90,  91,  92,  93,  94,
    95,  53,  54,  55,  56,  43,  0,   40,  41,  42,  39,  24,  0,   96,  0,   153, 139, 29,  31,
    33,  35,  36,  37,  28,  30,  32,  34,  27,  25,  38,  144, 145, 146, 161, 162, 147, 195, 196,
    197, 148, 201, 202, 149, 221, 222, 223, 224, 225, 226, 227, 150, 235, 236, 237, 238, 239, 240,
    241, 242, 243, 244, 163, 164, 165, 166, 167, 168, 169, 170, 171, 172, 173, 174, 175, 176, 177,
    26,  13,  0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   5,   0,
    0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,
    0,   0,   0,   0,   0,   118, 119, 97,  98,  99,  100, 101, 102, 103, 104, 105, 106, 107, 108,
    109, 127, 128, 129, 130, 131, 136, 132, 133, 134, 137, 138, 120, 121, 122, 123, 135, 124, 125,
    126, 141, 139, 142, 0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,
    0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,
    0,   0,   0,   0,   0,   0,   0,   0,   152, 160, 0,   157, 158, 156, 159, 154, 140, 151, 15,
    0,   0,   179, 0,   213, 210, 0,   205, 206, 207, 208, 0,   0,   0,   228, 229, 230, 231, 232,
    233, 234, 0,   255, 256, 257, 258, 259, 260, 261, 262, 263, 180, 181, 0,   183, 184, 185, 0,
    187, 0,   0,   0,   0,   192, 0,   0,   155, 139, 0,   139, 0,   210, 0,   0,   0,   139, 0,
    0,   0,   0,   0,   139, 0,   0,   0,   0,   0,   143, 0,   212, 217, 218, 219, 216, 220, 0,
    214, 211, 209, 203, 204, 0,   200, 0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   215,
    0,   246, 247, 248, 249, 245, 250, 182, 186, 188, 0,   190, 191, 193, 194, 178, 198, 199, 0,
    252, 189, 251, 0,   0,   253, 0,   254};

const short PipelineParserGen::yypgoto_[] = {
    -226, -226, -226, -203, -201, -131, -183, -118, -226, -226, -226, -226, -226, -226, -226, -226,
    -6,   -226, -5,   -7,   -1,   -226, -226, -225, -36,  -226, -226, -226, -226, -226, -226, -226,
    -219, -226, -226, -226, -226, 200,  -226, -226, -226, -226, -226, -226, -226, -226, 136,  14,
    -176, -16,  -124, -226, -226, -226, -226, -226, -226, -226, -226, -226, -226, -226, -226, -226,
    -226, -226, -226, -226, -226, -226, -226, -226, -226, -226, -226, -226, -226, -226, -226, -226,
    -226, -226, -226, -226, -226, -226, -226, -226, -226, -226, -226, -226, -226, -226, -226, -226,
    -226, -226, -226, -226, -226, -221, -117, -226, 54,   -226};

const short PipelineParserGen::yydefgoto_[] = {
    -1,  101, 295, 102, 103, 104, 105, 299, 379, 106, 300, 380, 229, 230, 231, 232, 233, 234,
    235, 236, 237, 238, 239, 355, 240, 241, 242, 243, 244, 245, 246, 247, 248, 312, 313, 314,
    354, 4,   12,  13,  14,  15,  16,  17,  18,  41,  123, 33,  249, 250, 255, 125, 126, 213,
    127, 128, 129, 130, 131, 132, 133, 134, 135, 136, 137, 138, 139, 140, 141, 142, 143, 144,
    145, 405, 146, 147, 148, 149, 150, 151, 152, 153, 154, 155, 156, 157, 158, 159, 160, 161,
    162, 163, 164, 165, 166, 167, 168, 169, 170, 418, 422, 251, 356, 2,   37,  38};

const short PipelineParserGen::yytable_[] = {
    31,  29,  30,  31,  29,  30,  32,  109, 23,  32,  296, 122, 297, 269, 270, 271, 272, 273, 274,
    275, 276, 277, 278, 279, 36,  281, 282, 283, 301, 285, 298, 124, 310, 315, 290, 31,  29,  30,
    311, 311, 1,   32,  3,   110, 5,   111, 20,  19,  112, 113, 114, 115, 116, 117, 34,  35,  107,
    261, 262, 263, 264, 265, 266, 267, 39,  122, 40,  42,  43,  21,  22,  23,  24,  44,  108, 172,
    173, 304, 305, 27,  307, 253, 303, 316, 317, 124, 254, 256, 257, 258, 311, 6,   7,   8,   9,
    10,  11,  259, 260, 280, 284, 286, 287, 288, 337, 289, 291, 108, 341, 292, 343, 344, 345, 346,
    302, 348, 349, 306, 118, 350, 119, 120, 121, 318, 21,  22,  23,  24,  351, 352, 370, 353, 372,
    319, 320, 321, 311, 322, 385, 25,  359, 26,  27,  28,  391, 323, 324, 325, 326, 361, 327, 374,
    328, 375, 398, 329, 330, 331, 332, 333, 311, 362, 334, 335, 336, 363, 252, 364, 365, 366, 367,
    377, 368, 369, 338, 339, 340, 342, 347, 417, 383, 357, 110, 108, 111, 384, 358, 21,  22,  23,
    24,  214, 215, 360, 25,  371, 26,  27,  28,  382, 308, 387, 309, 386, 388, 21,  22,  23,  24,
    214, 215, 389, 390, 392, 393, 394, 395, 396, 397, 399, 406, 407, 408, 376, 410, 409, 411, 412,
    413, 414, 415, 416, 419, 421, 424, 425, 378, 171, 381, 45,  0,   420, 268, 0,   0,   423, 108,
    216, 217, 218, 219, 220, 221, 222, 223, 224, 225, 25,  226, 26,  27,  28,  227, 228, 108, 216,
    217, 218, 219, 220, 221, 222, 223, 224, 225, 25,  226, 26,  27,  28,  227, 228, 46,  0,   0,
    47,  0,   0,   0,   0,   0,   0,   48,  49,  50,  51,  52,  53,  54,  55,  56,  57,  58,  59,
    60,  61,  62,  63,  64,  65,  66,  67,  68,  69,  70,  71,  72,  73,  74,  75,  76,  77,  78,
    79,  80,  81,  82,  83,  84,  85,  86,  87,  88,  89,  90,  91,  92,  93,  94,  95,  96,  97,
    98,  99,  100, 0,   0,   0,   0,   0,   0,   404, 0,   0,   0,   0,   0,   0,   0,   0,   0,
    0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,
    402, 400, 401, 0,   373, 0,   403, 294, 0,   0,   0,   0,   0,   0,   48,  49,  50,  51,  52,
    53,  54,  55,  56,  57,  58,  59,  60,  61,  62,  63,  64,  65,  66,  67,  68,  69,  70,  71,
    72,  73,  74,  75,  76,  77,  78,  79,  80,  81,  82,  83,  84,  85,  86,  87,  88,  89,  90,
    91,  92,  93,  94,  95,  96,  97,  98,  99,  100, 293, 0,   0,   294, 0,   0,   0,   0,   0,
    0,   48,  49,  50,  51,  52,  53,  54,  55,  56,  0,   0,   0,   0,   0,   0,   0,   0,   0,
    0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,
    0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   96,  97,  98,  99,  100, 174, 175, 176,
    177, 178, 179, 180, 181, 182, 183, 184, 185, 186, 187, 188, 189, 190, 191, 192, 193, 194, 195,
    196, 197, 198, 199, 200, 201, 202, 203, 204, 205, 206, 207, 208, 209, 210, 211, 212};

const short PipelineParserGen::yycheck_[] = {
    7,   7,   7,   10,  10,  10,  7,   43,  10,  10,  213, 47,  213, 189, 190, 191, 192, 193, 194,
    195, 196, 197, 198, 199, 10,  201, 202, 203, 249, 205, 213, 47,  257, 258, 210, 42,  42,  42,
    257, 258, 5,   42,  3,   3,   0,   5,   3,   6,   8,   9,   10,  11,  12,  13,  3,   3,   42,
    181, 182, 183, 184, 185, 186, 187, 4,   101, 4,   22,  20,  8,   9,   10,  11,  3,   67,  4,
    21,  253, 254, 81,  256, 5,   4,   259, 260, 101, 5,   5,   5,   5,   309, 14,  15,  16,  17,
    18,  19,  5,   5,   5,   5,   5,   5,   5,   280, 5,   5,   67,  284, 5,   286, 287, 288, 289,
    6,   291, 292, 4,   78,  295, 80,  81,  82,  4,   8,   9,   10,  11,  304, 305, 351, 307, 353,
    4,   4,   4,   355, 4,   359, 78,  316, 80,  81,  82,  365, 4,   4,   62,  4,   325, 4,   354,
    4,   354, 379, 4,   4,   4,   4,   4,   379, 337, 4,   4,   4,   341, 173, 343, 344, 345, 346,
    354, 348, 349, 4,   4,   4,   4,   4,   64,  4,   6,   3,   67,  5,   4,   6,   8,   9,   10,
    11,  12,  13,  6,   78,  6,   80,  81,  82,  6,   3,   63,  5,   4,   6,   8,   9,   10,  11,
    12,  13,  6,   6,   6,   6,   6,   6,   6,   6,   6,   4,   4,   4,   354, 4,   6,   4,   4,
    4,   4,   4,   4,   4,   65,  4,   4,   354, 101, 355, 39,  -1,  417, 188, -1,  -1,  421, 67,
    68,  69,  70,  71,  72,  73,  74,  75,  76,  77,  78,  79,  80,  81,  82,  83,  84,  67,  68,
    69,  70,  71,  72,  73,  74,  75,  76,  77,  78,  79,  80,  81,  82,  83,  84,  4,   -1,  -1,
    7,   -1,  -1,  -1,  -1,  -1,  -1,  14,  15,  16,  17,  18,  19,  20,  21,  22,  23,  24,  25,
    26,  27,  28,  29,  30,  31,  32,  33,  34,  35,  36,  37,  38,  39,  40,  41,  42,  43,  44,
    45,  46,  47,  48,  49,  50,  51,  52,  53,  54,  55,  56,  57,  58,  59,  60,  61,  62,  63,
    64,  65,  66,  -1,  -1,  -1,  -1,  -1,  -1,  387, -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,
    -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,
    387, 387, 387, -1,  4,   -1,  387, 7,   -1,  -1,  -1,  -1,  -1,  -1,  14,  15,  16,  17,  18,
    19,  20,  21,  22,  23,  24,  25,  26,  27,  28,  29,  30,  31,  32,  33,  34,  35,  36,  37,
    38,  39,  40,  41,  42,  43,  44,  45,  46,  47,  48,  49,  50,  51,  52,  53,  54,  55,  56,
    57,  58,  59,  60,  61,  62,  63,  64,  65,  66,  4,   -1,  -1,  7,   -1,  -1,  -1,  -1,  -1,
    -1,  14,  15,  16,  17,  18,  19,  20,  21,  22,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,
    -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,
    -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  62,  63,  64,  65,  66,  23,  24,  25,
    26,  27,  28,  29,  30,  31,  32,  33,  34,  35,  36,  37,  38,  39,  40,  41,  42,  43,  44,
    45,  46,  47,  48,  49,  50,  51,  52,  53,  54,  55,  56,  57,  58,  59,  60,  61};

const unsigned char PipelineParserGen::yystos_[] = {
    0,   5,   188, 3,   122, 0,   14,  15,  16,  17,  18,  19,  123, 124, 125, 126, 127, 128, 129,
    6,   3,   8,   9,   10,  11,  78,  80,  81,  82,  101, 103, 104, 105, 132, 3,   3,   132, 189,
    190, 4,   4,   130, 22,  20,  3,   122, 4,   7,   14,  15,  16,  17,  18,  19,  20,  21,  22,
    23,  24,  25,  26,  27,  28,  29,  30,  31,  32,  33,  34,  35,  36,  37,  38,  39,  40,  41,
    42,  43,  44,  45,  46,  47,  48,  49,  50,  51,  52,  53,  54,  55,  56,  57,  58,  59,  60,
    61,  62,  63,  64,  65,  66,  86,  88,  89,  90,  91,  94,  132, 67,  109, 3,   5,   8,   9,
    10,  11,  12,  13,  78,  80,  81,  82,  109, 131, 134, 136, 137, 139, 140, 141, 142, 143, 144,
    145, 146, 147, 148, 149, 150, 151, 152, 153, 154, 155, 156, 157, 159, 160, 161, 162, 163, 164,
    165, 166, 167, 168, 169, 170, 171, 172, 173, 174, 175, 176, 177, 178, 179, 180, 181, 182, 183,
    131, 4,   21,  23,  24,  25,  26,  27,  28,  29,  30,  31,  32,  33,  34,  35,  36,  37,  38,
    39,  40,  41,  42,  43,  44,  45,  46,  47,  48,  49,  50,  51,  52,  53,  54,  55,  56,  57,
    58,  59,  60,  61,  138, 12,  13,  68,  69,  70,  71,  72,  73,  74,  75,  76,  77,  79,  83,
    84,  97,  98,  99,  100, 101, 102, 103, 104, 105, 106, 107, 109, 110, 111, 112, 113, 114, 115,
    116, 117, 133, 134, 186, 104, 5,   5,   135, 5,   5,   5,   5,   5,   135, 135, 135, 135, 135,
    135, 135, 189, 133, 133, 133, 133, 133, 133, 133, 133, 133, 133, 133, 5,   133, 133, 133, 5,
    133, 5,   5,   5,   5,   133, 5,   5,   4,   7,   87,  88,  89,  91,  92,  95,  186, 6,   4,
    133, 133, 4,   133, 3,   5,   108, 117, 118, 119, 120, 108, 133, 133, 4,   4,   4,   4,   4,
    4,   4,   62,  4,   4,   4,   4,   4,   4,   4,   4,   4,   4,   4,   133, 4,   4,   4,   133,
    4,   133, 133, 133, 133, 4,   133, 133, 133, 133, 133, 133, 121, 108, 187, 6,   6,   133, 6,
    133, 133, 133, 133, 133, 133, 133, 133, 133, 186, 6,   186, 4,   88,  89,  90,  91,  92,  93,
    96,  187, 6,   4,   4,   186, 4,   63,  6,   6,   6,   186, 6,   6,   6,   6,   6,   6,   108,
    6,   101, 103, 104, 105, 109, 158, 4,   4,   4,   6,   4,   4,   4,   4,   4,   4,   4,   64,
    184, 4,   133, 65,  185, 133, 4,   4};

const unsigned char PipelineParserGen::yyr1_[] = {
    0,   85,  188, 122, 122, 190, 189, 123, 123, 123, 123, 123, 123, 129, 124, 125, 132, 132, 132,
    132, 126, 127, 128, 130, 130, 94,  94,  131, 131, 131, 131, 131, 131, 131, 131, 131, 131, 131,
    131, 86,  86,  86,  86,  91,  88,  88,  88,  88,  88,  88,  89,  89,  89,  89,  89,  89,  89,
    90,  90,  90,  90,  90,  90,  90,  90,  90,  90,  90,  90,  90,  90,  90,  90,  90,  90,  90,
    90,  90,  90,  90,  90,  90,  90,  90,  90,  90,  90,  90,  90,  90,  90,  90,  90,  90,  90,
    90,  109, 110, 111, 112, 114, 115, 116, 97,  98,  99,  100, 102, 106, 107, 101, 101, 103, 103,
    104, 104, 105, 105, 113, 113, 117, 117, 117, 117, 117, 117, 117, 117, 117, 117, 117, 117, 117,
    117, 117, 117, 117, 117, 117, 186, 186, 133, 133, 135, 134, 134, 134, 134, 134, 134, 134, 136,
    137, 138, 138, 95,  87,  87,  87,  87,  92,  139, 139, 139, 139, 139, 139, 139, 139, 139, 139,
    139, 139, 139, 139, 139, 139, 139, 140, 141, 169, 170, 171, 172, 173, 174, 175, 176, 177, 178,
    179, 180, 181, 182, 183, 142, 142, 142, 143, 144, 145, 146, 146, 147, 148, 108, 108, 118, 118,
    119, 187, 187, 120, 121, 121, 96,  93,  93,  93,  93,  93,  149, 149, 149, 149, 149, 149, 149,
    150, 151, 152, 153, 154, 155, 156, 157, 157, 157, 157, 157, 157, 157, 157, 157, 157, 158, 158,
    158, 158, 158, 184, 184, 185, 185, 159, 160, 161, 162, 163, 164, 165, 166, 167, 168};

const signed char PipelineParserGen::yyr2_[] = {
    0, 2, 3, 0, 4, 0, 2, 1, 1, 1, 1, 1, 1, 5, 3,  7, 1, 1, 1, 1, 2, 2, 4, 0, 2, 2, 2, 1, 1, 1,
    1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,  1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,
    1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,  1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,
    1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,  1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,
    1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,  1, 1, 1, 1, 0, 2, 1, 1, 4, 1, 1, 1, 1, 1, 1,
    1, 3, 3, 0, 2, 2, 1, 1, 1, 1, 1, 1, 1, 1, 1,  1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 8, 4,
    4, 4, 7, 4, 4, 4, 7, 4, 7, 8, 7, 7, 4, 7, 7,  1, 1, 1, 8, 8, 6, 1, 1, 6, 6, 1, 1, 1, 1, 3,
    0, 2, 3, 0, 2, 2, 1, 1, 1, 1, 1, 1, 1, 1, 1,  1, 1, 1, 4, 4, 4, 4, 4, 4, 4, 1, 1, 1, 1, 1,
    1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 0, 2, 0, 2, 11, 4, 4, 4, 4, 4, 4, 4, 4, 4};


#if YYDEBUG
// YYTNAME[SYMBOL-NUM] -- String name of the symbol SYMBOL-NUM.
// First, the terminals, then, starting at \a YYNTOKENS, nonterminals.
const char* const PipelineParserGen::yytname_[] = {"\"EOF\"",
                                                   "error",
                                                   "\"invalid token\"",
                                                   "START_OBJECT",
                                                   "END_OBJECT",
                                                   "START_ARRAY",
                                                   "END_ARRAY",
                                                   "ID",
                                                   "INT_ZERO",
                                                   "LONG_ZERO",
                                                   "DOUBLE_ZERO",
                                                   "DECIMAL_ZERO",
                                                   "BOOL_TRUE",
                                                   "BOOL_FALSE",
                                                   "STAGE_INHIBIT_OPTIMIZATION",
                                                   "STAGE_LIMIT",
                                                   "STAGE_PROJECT",
                                                   "STAGE_SAMPLE",
                                                   "STAGE_SKIP",
                                                   "STAGE_UNION_WITH",
                                                   "COLL_ARG",
                                                   "PIPELINE_ARG",
                                                   "SIZE_ARG",
                                                   "ADD",
                                                   "ATAN2",
                                                   "AND",
                                                   "CONST_EXPR",
                                                   "LITERAL",
                                                   "OR",
                                                   "NOT",
                                                   "CMP",
                                                   "EQ",
                                                   "GT",
                                                   "GTE",
                                                   "LT",
                                                   "LTE",
                                                   "NE",
                                                   "CONVERT",
                                                   "TO_BOOL",
                                                   "TO_DATE",
                                                   "TO_DECIMAL",
                                                   "TO_DOUBLE",
                                                   "TO_INT",
                                                   "TO_LONG",
                                                   "TO_OBJECT_ID",
                                                   "TO_STRING",
                                                   "TYPE",
                                                   "ABS",
                                                   "CEIL",
                                                   "DIVIDE",
                                                   "EXPONENT",
                                                   "FLOOR",
                                                   "LN",
                                                   "LOG",
                                                   "LOGTEN",
                                                   "MOD",
                                                   "MULTIPLY",
                                                   "POW",
                                                   "ROUND",
                                                   "SQRT",
                                                   "SUBTRACT",
                                                   "TRUNC",
                                                   "INPUT_ARG",
                                                   "TO_ARG",
                                                   "ON_ERROR_ARG",
                                                   "ON_NULL_ARG",
                                                   "FIELDNAME",
                                                   "STRING",
                                                   "BINARY",
                                                   "UNDEFINED",
                                                   "OBJECT_ID",
                                                   "DATE_LITERAL",
                                                   "JSNULL",
                                                   "REGEX",
                                                   "DB_POINTER",
                                                   "JAVASCRIPT",
                                                   "SYMBOL",
                                                   "JAVASCRIPT_W_SCOPE",
                                                   "INT_NON_ZERO",
                                                   "TIMESTAMP",
                                                   "LONG_NON_ZERO",
                                                   "DOUBLE_NON_ZERO",
                                                   "DECIMAL_NON_ZERO",
                                                   "MIN_KEY",
                                                   "MAX_KEY",
                                                   "$accept",
                                                   "projectionFieldname",
                                                   "expressionFieldname",
                                                   "stageAsUserFieldname",
                                                   "argAsUserFieldname",
                                                   "aggExprAsUserFieldname",
                                                   "invariableUserFieldname",
                                                   "idAsUserFieldname",
                                                   "valueFieldname",
                                                   "projectField",
                                                   "expressionField",
                                                   "valueField",
                                                   "dbPointer",
                                                   "javascript",
                                                   "symbol",
                                                   "javascriptWScope",
                                                   "int",
                                                   "timestamp",
                                                   "long",
                                                   "double",
                                                   "decimal",
                                                   "minKey",
                                                   "maxKey",
                                                   "value",
                                                   "string",
                                                   "binary",
                                                   "undefined",
                                                   "objectId",
                                                   "bool",
                                                   "date",
                                                   "null",
                                                   "regex",
                                                   "simpleValue",
                                                   "compoundValue",
                                                   "valueArray",
                                                   "valueObject",
                                                   "valueFields",
                                                   "stageList",
                                                   "stage",
                                                   "inhibitOptimization",
                                                   "unionWith",
                                                   "skip",
                                                   "limit",
                                                   "project",
                                                   "sample",
                                                   "projectFields",
                                                   "projection",
                                                   "num",
                                                   "expression",
                                                   "compoundExpression",
                                                   "exprFixedTwoArg",
                                                   "expressionArray",
                                                   "expressionObject",
                                                   "expressionFields",
                                                   "maths",
                                                   "add",
                                                   "atan2",
                                                   "boolExps",
                                                   "and",
                                                   "or",
                                                   "not",
                                                   "literalEscapes",
                                                   "const",
                                                   "literal",
                                                   "compExprs",
                                                   "cmp",
                                                   "eq",
                                                   "gt",
                                                   "gte",
                                                   "lt",
                                                   "lte",
                                                   "ne",
                                                   "typeExpression",
                                                   "typeValue",
                                                   "convert",
                                                   "toBool",
                                                   "toDate",
                                                   "toDecimal",
                                                   "toDouble",
                                                   "toInt",
                                                   "toLong",
                                                   "toObjectId",
                                                   "toString",
                                                   "type",
                                                   "abs",
                                                   "ceil",
                                                   "divide",
                                                   "exponent",
                                                   "floor",
                                                   "ln",
                                                   "log",
                                                   "logten",
                                                   "mod",
                                                   "multiply",
                                                   "pow",
                                                   "round",
                                                   "sqrt",
                                                   "subtract",
                                                   "trunc",
                                                   "onErrorArg",
                                                   "onNullArg",
                                                   "expressions",
                                                   "values",
                                                   "pipeline",
                                                   "START_ORDERED_OBJECT",
                                                   "$@1",
                                                   YY_NULLPTR};
#endif


#if YYDEBUG
const short PipelineParserGen::yyrline_[] = {
    0,    246,  246,  252,  253,  261,  261,  264,  264,  264,  264,  264,  264,  267,  277,  283,
    293,  293,  293,  293,  297,  302,  307,  313,  316,  323,  326,  332,  333,  336,  339,  342,
    345,  348,  351,  354,  357,  360,  363,  367,  367,  367,  367,  371,  379,  382,  385,  388,
    391,  394,  403,  406,  409,  412,  415,  418,  421,  429,  432,  435,  438,  441,  444,  447,
    450,  453,  456,  459,  462,  465,  468,  471,  474,  477,  480,  483,  486,  489,  492,  495,
    498,  501,  504,  507,  510,  513,  516,  519,  522,  525,  528,  531,  534,  537,  540,  543,
    550,  556,  562,  568,  574,  580,  586,  592,  598,  604,  610,  616,  622,  628,  634,  637,
    643,  646,  652,  655,  661,  664,  670,  673,  679,  680,  681,  682,  683,  684,  685,  686,
    687,  688,  689,  690,  691,  692,  693,  694,  695,  696,  697,  704,  705,  712,  712,  716,
    721,  721,  721,  721,  721,  721,  722,  728,  736,  742,  745,  752,  759,  759,  759,  759,
    763,  769,  769,  769,  769,  769,  769,  769,  769,  769,  769,  769,  769,  769,  770,  770,
    770,  770,  774,  784,  790,  795,  800,  806,  811,  816,  821,  827,  832,  838,  847,  853,
    859,  864,  870,  876,  876,  876,  880,  890,  900,  907,  907,  911,  918,  925,  925,  929,
    929,  933,  939,  940,  947,  953,  956,  963,  970,  971,  972,  973,  974,  977,  977,  977,
    977,  977,  977,  977,  979,  984,  989,  994,  999,  1004, 1009, 1015, 1016, 1017, 1018, 1019,
    1020, 1021, 1022, 1023, 1024, 1030, 1030, 1030, 1030, 1030, 1034, 1037, 1044, 1047, 1053, 1062,
    1067, 1072, 1077, 1082, 1087, 1092, 1097, 1102};

void PipelineParserGen::yy_stack_print_() const {
    *yycdebug_ << "Stack now";
    for (stack_type::const_iterator i = yystack_.begin(), i_end = yystack_.end(); i != i_end; ++i)
        *yycdebug_ << ' ' << int(i->state);
    *yycdebug_ << '\n';
}

void PipelineParserGen::yy_reduce_print_(int yyrule) const {
    int yylno = yyrline_[yyrule];
    int yynrhs = yyr2_[yyrule];
    // Print the symbols being reduced, and their result.
    *yycdebug_ << "Reducing stack by rule " << yyrule - 1 << " (line " << yylno << "):\n";
    // The symbols being reduced.
    for (int yyi = 0; yyi < yynrhs; yyi++)
        YY_SYMBOL_PRINT("   $" << yyi + 1 << " =", yystack_[(yynrhs) - (yyi + 1)]);
}
#endif  // YYDEBUG


#line 58 "pipeline_grammar.yy"
}  // namespace mongo
#line 4075 "pipeline_parser_gen.cpp"

#line 1106 "pipeline_grammar.yy"
