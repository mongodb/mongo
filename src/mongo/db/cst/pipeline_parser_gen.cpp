// A Bison parser, made by GNU Bison 3.6.

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
#line 83 "src/mongo/db/cst/pipeline_grammar.yy"

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

#line 63 "src/mongo/db/cst/pipeline_parser_gen.cpp"


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

#line 58 "src/mongo/db/cst/pipeline_grammar.yy"
namespace mongo {
#line 156 "src/mongo/db/cst/pipeline_parser_gen.cpp"

#if YYDEBUG || 0
const char* PipelineParserGen::symbol_name(symbol_kind_type yysymbol) {
    return yytname_[yysymbol];
}
#endif  // #if YYDEBUG || 0


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

        case 101:  // dbPointer
        case 102:  // javascript
        case 103:  // symbol
        case 104:  // javascriptWScope
        case 105:  // int
        case 106:  // timestamp
        case 107:  // long
        case 108:  // double
        case 109:  // decimal
        case 110:  // minKey
        case 111:  // maxKey
        case 112:  // value
        case 113:  // string
        case 114:  // binary
        case 115:  // undefined
        case 116:  // objectId
        case 117:  // bool
        case 118:  // date
        case 119:  // null
        case 120:  // regex
        case 121:  // simpleValue
        case 122:  // compoundValue
        case 123:  // valueArray
        case 124:  // valueObject
        case 125:  // valueFields
        case 126:  // stageList
        case 127:  // stage
        case 128:  // inhibitOptimization
        case 129:  // unionWith
        case 130:  // skip
        case 131:  // limit
        case 132:  // project
        case 133:  // sample
        case 134:  // projectFields
        case 135:  // projection
        case 136:  // num
        case 137:  // expression
        case 138:  // compoundExpression
        case 139:  // exprFixedTwoArg
        case 140:  // expressionArray
        case 141:  // expressionObject
        case 142:  // expressionFields
        case 143:  // maths
        case 144:  // add
        case 145:  // atan2
        case 146:  // boolExps
        case 147:  // and
        case 148:  // or
        case 149:  // not
        case 150:  // literalEscapes
        case 151:  // const
        case 152:  // literal
        case 153:  // compExprs
        case 154:  // cmp
        case 155:  // eq
        case 156:  // gt
        case 157:  // gte
        case 158:  // lt
        case 159:  // lte
        case 160:  // ne
        case 161:  // typeExpression
        case 162:  // typeValue
        case 163:  // convert
        case 164:  // toBool
        case 165:  // toDate
        case 166:  // toDecimal
        case 167:  // toDouble
        case 168:  // toInt
        case 169:  // toLong
        case 170:  // toObjectId
        case 171:  // toString
        case 172:  // type
        case 173:  // abs
        case 174:  // ceil
        case 175:  // divide
        case 176:  // exponent
        case 177:  // floor
        case 178:  // ln
        case 179:  // log
        case 180:  // logten
        case 181:  // mod
        case 182:  // multiply
        case 183:  // pow
        case 184:  // round
        case 185:  // sqrt
        case 186:  // subtract
        case 187:  // trunc
        case 192:  // matchExpression
        case 193:  // filterFields
        case 194:  // filterVal
            value.YY_MOVE_OR_COPY<CNode>(YY_MOVE(that.value));
            break;

        case 88:  // projectionFieldname
        case 89:  // expressionFieldname
        case 90:  // stageAsUserFieldname
        case 91:  // filterFieldname
        case 92:  // argAsUserFieldname
        case 93:  // aggExprAsUserFieldname
        case 94:  // invariableUserFieldname
        case 95:  // idAsUserFieldname
        case 96:  // valueFieldname
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

        case 97:   // projectField
        case 98:   // expressionField
        case 99:   // valueField
        case 100:  // filterField
        case 188:  // onErrorArg
        case 189:  // onNullArg
            value.YY_MOVE_OR_COPY<std::pair<CNode::Fieldname, CNode>>(YY_MOVE(that.value));
            break;

        case 66:  // FIELDNAME
        case 67:  // STRING
            value.YY_MOVE_OR_COPY<std::string>(YY_MOVE(that.value));
            break;

        case 190:  // expressions
        case 191:  // values
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

        case 101:  // dbPointer
        case 102:  // javascript
        case 103:  // symbol
        case 104:  // javascriptWScope
        case 105:  // int
        case 106:  // timestamp
        case 107:  // long
        case 108:  // double
        case 109:  // decimal
        case 110:  // minKey
        case 111:  // maxKey
        case 112:  // value
        case 113:  // string
        case 114:  // binary
        case 115:  // undefined
        case 116:  // objectId
        case 117:  // bool
        case 118:  // date
        case 119:  // null
        case 120:  // regex
        case 121:  // simpleValue
        case 122:  // compoundValue
        case 123:  // valueArray
        case 124:  // valueObject
        case 125:  // valueFields
        case 126:  // stageList
        case 127:  // stage
        case 128:  // inhibitOptimization
        case 129:  // unionWith
        case 130:  // skip
        case 131:  // limit
        case 132:  // project
        case 133:  // sample
        case 134:  // projectFields
        case 135:  // projection
        case 136:  // num
        case 137:  // expression
        case 138:  // compoundExpression
        case 139:  // exprFixedTwoArg
        case 140:  // expressionArray
        case 141:  // expressionObject
        case 142:  // expressionFields
        case 143:  // maths
        case 144:  // add
        case 145:  // atan2
        case 146:  // boolExps
        case 147:  // and
        case 148:  // or
        case 149:  // not
        case 150:  // literalEscapes
        case 151:  // const
        case 152:  // literal
        case 153:  // compExprs
        case 154:  // cmp
        case 155:  // eq
        case 156:  // gt
        case 157:  // gte
        case 158:  // lt
        case 159:  // lte
        case 160:  // ne
        case 161:  // typeExpression
        case 162:  // typeValue
        case 163:  // convert
        case 164:  // toBool
        case 165:  // toDate
        case 166:  // toDecimal
        case 167:  // toDouble
        case 168:  // toInt
        case 169:  // toLong
        case 170:  // toObjectId
        case 171:  // toString
        case 172:  // type
        case 173:  // abs
        case 174:  // ceil
        case 175:  // divide
        case 176:  // exponent
        case 177:  // floor
        case 178:  // ln
        case 179:  // log
        case 180:  // logten
        case 181:  // mod
        case 182:  // multiply
        case 183:  // pow
        case 184:  // round
        case 185:  // sqrt
        case 186:  // subtract
        case 187:  // trunc
        case 192:  // matchExpression
        case 193:  // filterFields
        case 194:  // filterVal
            value.move<CNode>(YY_MOVE(that.value));
            break;

        case 88:  // projectionFieldname
        case 89:  // expressionFieldname
        case 90:  // stageAsUserFieldname
        case 91:  // filterFieldname
        case 92:  // argAsUserFieldname
        case 93:  // aggExprAsUserFieldname
        case 94:  // invariableUserFieldname
        case 95:  // idAsUserFieldname
        case 96:  // valueFieldname
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

        case 97:   // projectField
        case 98:   // expressionField
        case 99:   // valueField
        case 100:  // filterField
        case 188:  // onErrorArg
        case 189:  // onNullArg
            value.move<std::pair<CNode::Fieldname, CNode>>(YY_MOVE(that.value));
            break;

        case 66:  // FIELDNAME
        case 67:  // STRING
            value.move<std::string>(YY_MOVE(that.value));
            break;

        case 190:  // expressions
        case 191:  // values
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

        case 101:  // dbPointer
        case 102:  // javascript
        case 103:  // symbol
        case 104:  // javascriptWScope
        case 105:  // int
        case 106:  // timestamp
        case 107:  // long
        case 108:  // double
        case 109:  // decimal
        case 110:  // minKey
        case 111:  // maxKey
        case 112:  // value
        case 113:  // string
        case 114:  // binary
        case 115:  // undefined
        case 116:  // objectId
        case 117:  // bool
        case 118:  // date
        case 119:  // null
        case 120:  // regex
        case 121:  // simpleValue
        case 122:  // compoundValue
        case 123:  // valueArray
        case 124:  // valueObject
        case 125:  // valueFields
        case 126:  // stageList
        case 127:  // stage
        case 128:  // inhibitOptimization
        case 129:  // unionWith
        case 130:  // skip
        case 131:  // limit
        case 132:  // project
        case 133:  // sample
        case 134:  // projectFields
        case 135:  // projection
        case 136:  // num
        case 137:  // expression
        case 138:  // compoundExpression
        case 139:  // exprFixedTwoArg
        case 140:  // expressionArray
        case 141:  // expressionObject
        case 142:  // expressionFields
        case 143:  // maths
        case 144:  // add
        case 145:  // atan2
        case 146:  // boolExps
        case 147:  // and
        case 148:  // or
        case 149:  // not
        case 150:  // literalEscapes
        case 151:  // const
        case 152:  // literal
        case 153:  // compExprs
        case 154:  // cmp
        case 155:  // eq
        case 156:  // gt
        case 157:  // gte
        case 158:  // lt
        case 159:  // lte
        case 160:  // ne
        case 161:  // typeExpression
        case 162:  // typeValue
        case 163:  // convert
        case 164:  // toBool
        case 165:  // toDate
        case 166:  // toDecimal
        case 167:  // toDouble
        case 168:  // toInt
        case 169:  // toLong
        case 170:  // toObjectId
        case 171:  // toString
        case 172:  // type
        case 173:  // abs
        case 174:  // ceil
        case 175:  // divide
        case 176:  // exponent
        case 177:  // floor
        case 178:  // ln
        case 179:  // log
        case 180:  // logten
        case 181:  // mod
        case 182:  // multiply
        case 183:  // pow
        case 184:  // round
        case 185:  // sqrt
        case 186:  // subtract
        case 187:  // trunc
        case 192:  // matchExpression
        case 193:  // filterFields
        case 194:  // filterVal
            value.copy<CNode>(that.value);
            break;

        case 88:  // projectionFieldname
        case 89:  // expressionFieldname
        case 90:  // stageAsUserFieldname
        case 91:  // filterFieldname
        case 92:  // argAsUserFieldname
        case 93:  // aggExprAsUserFieldname
        case 94:  // invariableUserFieldname
        case 95:  // idAsUserFieldname
        case 96:  // valueFieldname
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

        case 97:   // projectField
        case 98:   // expressionField
        case 99:   // valueField
        case 100:  // filterField
        case 188:  // onErrorArg
        case 189:  // onNullArg
            value.copy<std::pair<CNode::Fieldname, CNode>>(that.value);
            break;

        case 66:  // FIELDNAME
        case 67:  // STRING
            value.copy<std::string>(that.value);
            break;

        case 190:  // expressions
        case 191:  // values
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

        case 101:  // dbPointer
        case 102:  // javascript
        case 103:  // symbol
        case 104:  // javascriptWScope
        case 105:  // int
        case 106:  // timestamp
        case 107:  // long
        case 108:  // double
        case 109:  // decimal
        case 110:  // minKey
        case 111:  // maxKey
        case 112:  // value
        case 113:  // string
        case 114:  // binary
        case 115:  // undefined
        case 116:  // objectId
        case 117:  // bool
        case 118:  // date
        case 119:  // null
        case 120:  // regex
        case 121:  // simpleValue
        case 122:  // compoundValue
        case 123:  // valueArray
        case 124:  // valueObject
        case 125:  // valueFields
        case 126:  // stageList
        case 127:  // stage
        case 128:  // inhibitOptimization
        case 129:  // unionWith
        case 130:  // skip
        case 131:  // limit
        case 132:  // project
        case 133:  // sample
        case 134:  // projectFields
        case 135:  // projection
        case 136:  // num
        case 137:  // expression
        case 138:  // compoundExpression
        case 139:  // exprFixedTwoArg
        case 140:  // expressionArray
        case 141:  // expressionObject
        case 142:  // expressionFields
        case 143:  // maths
        case 144:  // add
        case 145:  // atan2
        case 146:  // boolExps
        case 147:  // and
        case 148:  // or
        case 149:  // not
        case 150:  // literalEscapes
        case 151:  // const
        case 152:  // literal
        case 153:  // compExprs
        case 154:  // cmp
        case 155:  // eq
        case 156:  // gt
        case 157:  // gte
        case 158:  // lt
        case 159:  // lte
        case 160:  // ne
        case 161:  // typeExpression
        case 162:  // typeValue
        case 163:  // convert
        case 164:  // toBool
        case 165:  // toDate
        case 166:  // toDecimal
        case 167:  // toDouble
        case 168:  // toInt
        case 169:  // toLong
        case 170:  // toObjectId
        case 171:  // toString
        case 172:  // type
        case 173:  // abs
        case 174:  // ceil
        case 175:  // divide
        case 176:  // exponent
        case 177:  // floor
        case 178:  // ln
        case 179:  // log
        case 180:  // logten
        case 181:  // mod
        case 182:  // multiply
        case 183:  // pow
        case 184:  // round
        case 185:  // sqrt
        case 186:  // subtract
        case 187:  // trunc
        case 192:  // matchExpression
        case 193:  // filterFields
        case 194:  // filterVal
            value.move<CNode>(that.value);
            break;

        case 88:  // projectionFieldname
        case 89:  // expressionFieldname
        case 90:  // stageAsUserFieldname
        case 91:  // filterFieldname
        case 92:  // argAsUserFieldname
        case 93:  // aggExprAsUserFieldname
        case 94:  // invariableUserFieldname
        case 95:  // idAsUserFieldname
        case 96:  // valueFieldname
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

        case 97:   // projectField
        case 98:   // expressionField
        case 99:   // valueField
        case 100:  // filterField
        case 188:  // onErrorArg
        case 189:  // onNullArg
            value.move<std::pair<CNode::Fieldname, CNode>>(that.value);
            break;

        case 66:  // FIELDNAME
        case 67:  // STRING
            value.move<std::string>(that.value);
            break;

        case 190:  // expressions
        case 191:  // values
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
        yyo << (yykind < YYNTOKENS ? "token" : "nterm") << ' ' << symbol_name(yykind) << " ("
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

                case 101:  // dbPointer
                case 102:  // javascript
                case 103:  // symbol
                case 104:  // javascriptWScope
                case 105:  // int
                case 106:  // timestamp
                case 107:  // long
                case 108:  // double
                case 109:  // decimal
                case 110:  // minKey
                case 111:  // maxKey
                case 112:  // value
                case 113:  // string
                case 114:  // binary
                case 115:  // undefined
                case 116:  // objectId
                case 117:  // bool
                case 118:  // date
                case 119:  // null
                case 120:  // regex
                case 121:  // simpleValue
                case 122:  // compoundValue
                case 123:  // valueArray
                case 124:  // valueObject
                case 125:  // valueFields
                case 126:  // stageList
                case 127:  // stage
                case 128:  // inhibitOptimization
                case 129:  // unionWith
                case 130:  // skip
                case 131:  // limit
                case 132:  // project
                case 133:  // sample
                case 134:  // projectFields
                case 135:  // projection
                case 136:  // num
                case 137:  // expression
                case 138:  // compoundExpression
                case 139:  // exprFixedTwoArg
                case 140:  // expressionArray
                case 141:  // expressionObject
                case 142:  // expressionFields
                case 143:  // maths
                case 144:  // add
                case 145:  // atan2
                case 146:  // boolExps
                case 147:  // and
                case 148:  // or
                case 149:  // not
                case 150:  // literalEscapes
                case 151:  // const
                case 152:  // literal
                case 153:  // compExprs
                case 154:  // cmp
                case 155:  // eq
                case 156:  // gt
                case 157:  // gte
                case 158:  // lt
                case 159:  // lte
                case 160:  // ne
                case 161:  // typeExpression
                case 162:  // typeValue
                case 163:  // convert
                case 164:  // toBool
                case 165:  // toDate
                case 166:  // toDecimal
                case 167:  // toDouble
                case 168:  // toInt
                case 169:  // toLong
                case 170:  // toObjectId
                case 171:  // toString
                case 172:  // type
                case 173:  // abs
                case 174:  // ceil
                case 175:  // divide
                case 176:  // exponent
                case 177:  // floor
                case 178:  // ln
                case 179:  // log
                case 180:  // logten
                case 181:  // mod
                case 182:  // multiply
                case 183:  // pow
                case 184:  // round
                case 185:  // sqrt
                case 186:  // subtract
                case 187:  // trunc
                case 192:  // matchExpression
                case 193:  // filterFields
                case 194:  // filterVal
                    yylhs.value.emplace<CNode>();
                    break;

                case 88:  // projectionFieldname
                case 89:  // expressionFieldname
                case 90:  // stageAsUserFieldname
                case 91:  // filterFieldname
                case 92:  // argAsUserFieldname
                case 93:  // aggExprAsUserFieldname
                case 94:  // invariableUserFieldname
                case 95:  // idAsUserFieldname
                case 96:  // valueFieldname
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

                case 97:   // projectField
                case 98:   // expressionField
                case 99:   // valueField
                case 100:  // filterField
                case 188:  // onErrorArg
                case 189:  // onNullArg
                    yylhs.value.emplace<std::pair<CNode::Fieldname, CNode>>();
                    break;

                case 66:  // FIELDNAME
                case 67:  // STRING
                    yylhs.value.emplace<std::string>();
                    break;

                case 190:  // expressions
                case 191:  // values
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
                    case 3:
#line 249 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        *cst = CNode{YY_MOVE(yystack_[0].value.as<CNode>())};
                    }
#line 1522 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 4:
#line 256 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        *cst = YY_MOVE(yystack_[1].value.as<CNode>());
                    }
#line 1530 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 5:
#line 262 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                    }
#line 1536 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 6:
#line 263 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() =
                            CNode{CNode::ArrayChildren{YY_MOVE(yystack_[2].value.as<CNode>())}};
                    }
#line 1544 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 7:
#line 271 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        lexer.sortObjTokens();
                    }
#line 1550 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 9:
#line 274 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 1556 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 10:
#line 274 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 1562 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 11:
#line 274 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 1568 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 12:
#line 274 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 1574 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 13:
#line 274 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 1580 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 14:
#line 274 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 1586 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 15:
#line 277 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{std::pair{
                            KeyFieldname::sample,
                            CNode{CNode::ObjectChildren{
                                {KeyFieldname::sizeArg, YY_MOVE(yystack_[1].value.as<CNode>())},
                            }}}}};
                    }
#line 1598 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 16:
#line 287 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            std::pair{KeyFieldname::inhibitOptimization, CNode::noopLeaf()}}};
                    }
#line 1606 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 17:
#line 293 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        auto pipeline = YY_MOVE(yystack_[1].value.as<CNode>());
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{std::pair{
                            KeyFieldname::unionWith,
                            CNode{CNode::ObjectChildren{
                                {KeyFieldname::collArg, YY_MOVE(yystack_[3].value.as<CNode>())},
                                {KeyFieldname::pipelineArg, std::move(pipeline)}}}}}};
                    }
#line 1619 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 18:
#line 303 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 1625 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 19:
#line 303 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 1631 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 20:
#line 303 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 1637 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 21:
#line 303 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 1643 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 22:
#line 307 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            std::pair{KeyFieldname::skip, YY_MOVE(yystack_[0].value.as<CNode>())}}};
                    }
#line 1651 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 23:
#line 312 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{std::pair{
                            KeyFieldname::limit, YY_MOVE(yystack_[0].value.as<CNode>())}}};
                    }
#line 1659 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 24:
#line 317 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{std::pair{
                            KeyFieldname::project, YY_MOVE(yystack_[1].value.as<CNode>())}}};
                    }
#line 1667 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 25:
#line 323 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode::noopLeaf();
                    }
#line 1675 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 26:
#line 326 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[1].value.as<CNode>());
                        yylhs.value.as<CNode>().objectChildren().emplace_back(
                            YY_MOVE(yystack_[0].value.as<std::pair<CNode::Fieldname, CNode>>()));
                    }
#line 1684 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 27:
#line 333 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<std::pair<CNode::Fieldname, CNode>>() = {
                            KeyFieldname::id, YY_MOVE(yystack_[0].value.as<CNode>())};
                    }
#line 1692 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 28:
#line 336 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<std::pair<CNode::Fieldname, CNode>>() = {
                            YY_MOVE(yystack_[1].value.as<CNode::Fieldname>()),
                            YY_MOVE(yystack_[0].value.as<CNode>())};
                    }
#line 1700 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 29:
#line 342 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 1706 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 30:
#line 343 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() =
                            CNode{NonZeroKey{YY_MOVE(yystack_[0].value.as<int>())}};
                    }
#line 1714 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 31:
#line 346 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{KeyValue::intZeroKey};
                    }
#line 1722 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 32:
#line 349 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() =
                            CNode{NonZeroKey{YY_MOVE(yystack_[0].value.as<long long>())}};
                    }
#line 1730 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 33:
#line 352 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{KeyValue::longZeroKey};
                    }
#line 1738 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 34:
#line 355 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() =
                            CNode{NonZeroKey{YY_MOVE(yystack_[0].value.as<double>())}};
                    }
#line 1746 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 35:
#line 358 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{KeyValue::doubleZeroKey};
                    }
#line 1754 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 36:
#line 361 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() =
                            CNode{NonZeroKey{YY_MOVE(yystack_[0].value.as<Decimal128>())}};
                    }
#line 1762 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 37:
#line 364 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{KeyValue::decimalZeroKey};
                    }
#line 1770 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 38:
#line 367 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{KeyValue::trueKey};
                    }
#line 1778 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 39:
#line 370 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{KeyValue::falseKey};
                    }
#line 1786 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 40:
#line 373 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 1792 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 41:
#line 377 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() =
                            YY_MOVE(yystack_[0].value.as<CNode::Fieldname>());
                    }
#line 1798 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 42:
#line 377 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() =
                            YY_MOVE(yystack_[0].value.as<CNode::Fieldname>());
                    }
#line 1804 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 43:
#line 377 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() =
                            YY_MOVE(yystack_[0].value.as<CNode::Fieldname>());
                    }
#line 1810 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 44:
#line 377 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() =
                            YY_MOVE(yystack_[0].value.as<CNode::Fieldname>());
                    }
#line 1816 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 45:
#line 381 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{std::pair{
                            KeyFieldname::match, YY_MOVE(yystack_[1].value.as<CNode>())}}};
                    }
#line 1824 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 46:
#line 387 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode::noopLeaf();
                    }
#line 1832 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 47:
#line 390 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[1].value.as<CNode>());
                        yylhs.value.as<CNode>().objectChildren().emplace_back(
                            YY_MOVE(yystack_[0].value.as<std::pair<CNode::Fieldname, CNode>>()));
                    }
#line 1841 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 48:
#line 397 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<std::pair<CNode::Fieldname, CNode>>() = {
                            KeyFieldname::id, YY_MOVE(yystack_[0].value.as<CNode>())};
                    }
#line 1849 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 49:
#line 400 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<std::pair<CNode::Fieldname, CNode>>() = {
                            YY_MOVE(yystack_[1].value.as<CNode::Fieldname>()),
                            YY_MOVE(yystack_[0].value.as<CNode>())};
                    }
#line 1857 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 50:
#line 406 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 1863 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 51:
#line 410 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() =
                            YY_MOVE(yystack_[0].value.as<CNode::Fieldname>());
                    }
#line 1869 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 52:
#line 410 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() =
                            YY_MOVE(yystack_[0].value.as<CNode::Fieldname>());
                    }
#line 1875 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 53:
#line 410 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() =
                            YY_MOVE(yystack_[0].value.as<CNode::Fieldname>());
                    }
#line 1881 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 54:
#line 410 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() =
                            YY_MOVE(yystack_[0].value.as<CNode::Fieldname>());
                    }
#line 1887 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 55:
#line 414 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() =
                            UserFieldname{YY_MOVE(yystack_[0].value.as<std::string>())};
                    }
#line 1895 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 56:
#line 422 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() =
                            UserFieldname{"$_internalInhibitOptimization"};
                    }
#line 1903 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 57:
#line 425 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$unionWith"};
                    }
#line 1911 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 58:
#line 428 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$skip"};
                    }
#line 1919 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 59:
#line 431 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$limit"};
                    }
#line 1927 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 60:
#line 434 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$project"};
                    }
#line 1935 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 61:
#line 437 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$sample"};
                    }
#line 1943 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 62:
#line 446 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"coll"};
                    }
#line 1951 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 63:
#line 449 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"pipeline"};
                    }
#line 1959 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 64:
#line 452 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"size"};
                    }
#line 1967 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 65:
#line 455 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"input"};
                    }
#line 1975 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 66:
#line 458 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"to"};
                    }
#line 1983 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 67:
#line 461 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"onError"};
                    }
#line 1991 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 68:
#line 464 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"onNull"};
                    }
#line 1999 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 69:
#line 472 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$add"};
                    }
#line 2007 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 70:
#line 475 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$atan2"};
                    }
#line 2015 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 71:
#line 478 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$and"};
                    }
#line 2023 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 72:
#line 481 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$const"};
                    }
#line 2031 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 73:
#line 484 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$literal"};
                    }
#line 2039 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 74:
#line 487 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$or"};
                    }
#line 2047 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 75:
#line 490 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$not"};
                    }
#line 2055 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 76:
#line 493 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$cmp"};
                    }
#line 2063 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 77:
#line 496 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$eq"};
                    }
#line 2071 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 78:
#line 499 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$gt"};
                    }
#line 2079 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 79:
#line 502 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$gte"};
                    }
#line 2087 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 80:
#line 505 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$lt"};
                    }
#line 2095 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 81:
#line 508 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$lte"};
                    }
#line 2103 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 82:
#line 511 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$ne"};
                    }
#line 2111 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 83:
#line 514 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$convert"};
                    }
#line 2119 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 84:
#line 517 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$toBool"};
                    }
#line 2127 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 85:
#line 520 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$toDate"};
                    }
#line 2135 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 86:
#line 523 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$toDecimal"};
                    }
#line 2143 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 87:
#line 526 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$toDouble"};
                    }
#line 2151 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 88:
#line 529 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$toInt"};
                    }
#line 2159 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 89:
#line 532 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$toLong"};
                    }
#line 2167 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 90:
#line 535 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$toObjectId"};
                    }
#line 2175 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 91:
#line 538 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$toString"};
                    }
#line 2183 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 92:
#line 541 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$type"};
                    }
#line 2191 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 93:
#line 544 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$abs"};
                    }
#line 2199 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 94:
#line 547 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$ceil"};
                    }
#line 2207 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 95:
#line 550 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$divide"};
                    }
#line 2215 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 96:
#line 553 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$exp"};
                    }
#line 2223 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 97:
#line 556 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$floor"};
                    }
#line 2231 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 98:
#line 559 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$ln"};
                    }
#line 2239 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 99:
#line 562 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$log"};
                    }
#line 2247 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 100:
#line 565 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$log10"};
                    }
#line 2255 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 101:
#line 568 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$mod"};
                    }
#line 2263 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 102:
#line 571 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$multiply"};
                    }
#line 2271 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 103:
#line 574 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$pow"};
                    }
#line 2279 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 104:
#line 577 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$round"};
                    }
#line 2287 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 105:
#line 580 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$sqrt"};
                    }
#line 2295 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 106:
#line 583 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$subtract"};
                    }
#line 2303 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 107:
#line 586 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$trunc"};
                    }
#line 2311 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 108:
#line 593 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() =
                            CNode{UserString{YY_MOVE(yystack_[0].value.as<std::string>())}};
                    }
#line 2319 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 109:
#line 599 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() =
                            CNode{UserBinary{YY_MOVE(yystack_[0].value.as<BSONBinData>())}};
                    }
#line 2327 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 110:
#line 605 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{UserUndefined{}};
                    }
#line 2335 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 111:
#line 611 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{UserObjectId{}};
                    }
#line 2343 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 112:
#line 617 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() =
                            CNode{UserDate{YY_MOVE(yystack_[0].value.as<Date_t>())}};
                    }
#line 2351 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 113:
#line 623 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{UserNull{}};
                    }
#line 2359 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 114:
#line 629 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() =
                            CNode{UserRegex{YY_MOVE(yystack_[0].value.as<BSONRegEx>())}};
                    }
#line 2367 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 115:
#line 635 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() =
                            CNode{UserDBPointer{YY_MOVE(yystack_[0].value.as<BSONDBRef>())}};
                    }
#line 2375 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 116:
#line 641 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() =
                            CNode{UserJavascript{YY_MOVE(yystack_[0].value.as<BSONCode>())}};
                    }
#line 2383 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 117:
#line 647 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() =
                            CNode{UserSymbol{YY_MOVE(yystack_[0].value.as<BSONSymbol>())}};
                    }
#line 2391 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 118:
#line 653 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{UserJavascriptWithScope{
                            YY_MOVE(yystack_[0].value.as<BSONCodeWScope>())}};
                    }
#line 2399 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 119:
#line 659 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() =
                            CNode{UserTimestamp{YY_MOVE(yystack_[0].value.as<Timestamp>())}};
                    }
#line 2407 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 120:
#line 665 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() =
                            CNode{UserMinKey{YY_MOVE(yystack_[0].value.as<UserMinKey>())}};
                    }
#line 2415 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 121:
#line 671 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() =
                            CNode{UserMaxKey{YY_MOVE(yystack_[0].value.as<UserMaxKey>())}};
                    }
#line 2423 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 122:
#line 677 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() =
                            CNode{UserInt{YY_MOVE(yystack_[0].value.as<int>())}};
                    }
#line 2431 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 123:
#line 680 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{UserInt{0}};
                    }
#line 2439 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 124:
#line 686 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() =
                            CNode{UserLong{YY_MOVE(yystack_[0].value.as<long long>())}};
                    }
#line 2447 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 125:
#line 689 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{UserLong{0ll}};
                    }
#line 2455 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 126:
#line 695 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() =
                            CNode{UserDouble{YY_MOVE(yystack_[0].value.as<double>())}};
                    }
#line 2463 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 127:
#line 698 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{UserDouble{0.0}};
                    }
#line 2471 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 128:
#line 704 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() =
                            CNode{UserDecimal{YY_MOVE(yystack_[0].value.as<Decimal128>())}};
                    }
#line 2479 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 129:
#line 707 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{UserDecimal{0.0}};
                    }
#line 2487 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 130:
#line 713 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{UserBoolean{true}};
                    }
#line 2495 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 131:
#line 716 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{UserBoolean{false}};
                    }
#line 2503 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 132:
#line 722 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 2509 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 133:
#line 723 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 2515 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 134:
#line 724 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 2521 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 135:
#line 725 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 2527 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 136:
#line 726 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 2533 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 137:
#line 727 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 2539 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 138:
#line 728 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 2545 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 139:
#line 729 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 2551 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 140:
#line 730 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 2557 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 141:
#line 731 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 2563 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 142:
#line 732 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 2569 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 143:
#line 733 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 2575 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 144:
#line 734 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 2581 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 145:
#line 735 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 2587 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 146:
#line 736 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 2593 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 147:
#line 737 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 2599 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 148:
#line 738 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 2605 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 149:
#line 739 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 2611 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 150:
#line 740 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 2617 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 151:
#line 747 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                    }
#line 2623 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 152:
#line 748 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<std::vector<CNode>>() =
                            YY_MOVE(yystack_[0].value.as<std::vector<CNode>>());
                        yylhs.value.as<std::vector<CNode>>().emplace_back(
                            YY_MOVE(yystack_[1].value.as<CNode>()));
                    }
#line 2632 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 153:
#line 755 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 2638 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 154:
#line 755 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 2644 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 155:
#line 759 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() =
                            CNode{CNode::ArrayChildren{YY_MOVE(yystack_[2].value.as<CNode>()),
                                                       YY_MOVE(yystack_[1].value.as<CNode>())}};
                    }
#line 2652 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 156:
#line 764 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 2658 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 157:
#line 764 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 2664 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 158:
#line 764 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 2670 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 159:
#line 764 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 2676 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 160:
#line 764 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 2682 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 161:
#line 764 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 2688 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 162:
#line 765 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 2694 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 163:
#line 771 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() =
                            CNode{YY_MOVE(yystack_[1].value.as<std::vector<CNode>>())};
                    }
#line 2702 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 164:
#line 779 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[1].value.as<CNode>());
                    }
#line 2710 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 165:
#line 785 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode::noopLeaf();
                    }
#line 2718 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 166:
#line 788 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[1].value.as<CNode>());
                        yylhs.value.as<CNode>().objectChildren().emplace_back(
                            YY_MOVE(yystack_[0].value.as<std::pair<CNode::Fieldname, CNode>>()));
                    }
#line 2727 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 167:
#line 795 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<std::pair<CNode::Fieldname, CNode>>() = {
                            YY_MOVE(yystack_[1].value.as<CNode::Fieldname>()),
                            YY_MOVE(yystack_[0].value.as<CNode>())};
                    }
#line 2735 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 168:
#line 802 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() =
                            YY_MOVE(yystack_[0].value.as<CNode::Fieldname>());
                    }
#line 2741 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 169:
#line 802 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() =
                            YY_MOVE(yystack_[0].value.as<CNode::Fieldname>());
                    }
#line 2747 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 170:
#line 802 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() =
                            YY_MOVE(yystack_[0].value.as<CNode::Fieldname>());
                    }
#line 2753 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 171:
#line 802 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() =
                            YY_MOVE(yystack_[0].value.as<CNode::Fieldname>());
                    }
#line 2759 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 172:
#line 806 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"_id"};
                    }
#line 2767 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 173:
#line 812 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 2773 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 174:
#line 812 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 2779 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 175:
#line 812 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 2785 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 176:
#line 812 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 2791 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 177:
#line 812 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 2797 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 178:
#line 812 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 2803 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 179:
#line 812 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 2809 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 180:
#line 812 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 2815 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 181:
#line 812 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 2821 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 182:
#line 812 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 2827 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 183:
#line 812 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 2833 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 184:
#line 812 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 2839 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 185:
#line 812 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 2845 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 186:
#line 813 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 2851 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 187:
#line 813 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 2857 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 188:
#line 813 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 2863 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 189:
#line 813 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 2869 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 190:
#line 817 "src/mongo/db/cst/pipeline_grammar.yy"
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
#line 2881 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 191:
#line 827 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::atan2, YY_MOVE(yystack_[1].value.as<CNode>())}}};
                    }
#line 2890 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 192:
#line 833 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::abs, YY_MOVE(yystack_[1].value.as<CNode>())}}};
                    }
#line 2898 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 193:
#line 838 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::ceil, YY_MOVE(yystack_[1].value.as<CNode>())}}};
                    }
#line 2906 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 194:
#line 843 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::divide,
                             CNode{CNode::ArrayChildren{YY_MOVE(yystack_[3].value.as<CNode>()),
                                                        YY_MOVE(yystack_[2].value.as<CNode>())}}}}};
                    }
#line 2915 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 195:
#line 849 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::exponent, YY_MOVE(yystack_[1].value.as<CNode>())}}};
                    }
#line 2923 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 196:
#line 854 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::floor, YY_MOVE(yystack_[1].value.as<CNode>())}}};
                    }
#line 2931 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 197:
#line 859 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::ln, YY_MOVE(yystack_[1].value.as<CNode>())}}};
                    }
#line 2939 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 198:
#line 864 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::log,
                             CNode{CNode::ArrayChildren{YY_MOVE(yystack_[3].value.as<CNode>()),
                                                        YY_MOVE(yystack_[2].value.as<CNode>())}}}}};
                    }
#line 2948 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 199:
#line 870 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::logten, YY_MOVE(yystack_[1].value.as<CNode>())}}};
                    }
#line 2956 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 200:
#line 875 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::mod,
                             CNode{CNode::ArrayChildren{YY_MOVE(yystack_[3].value.as<CNode>()),
                                                        YY_MOVE(yystack_[2].value.as<CNode>())}}}}};
                    }
#line 2965 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 201:
#line 881 "src/mongo/db/cst/pipeline_grammar.yy"
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
#line 2977 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 202:
#line 890 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::pow,
                             CNode{CNode::ArrayChildren{YY_MOVE(yystack_[3].value.as<CNode>()),
                                                        YY_MOVE(yystack_[2].value.as<CNode>())}}}}};
                    }
#line 2986 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 203:
#line 896 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::round,
                             CNode{CNode::ArrayChildren{YY_MOVE(yystack_[3].value.as<CNode>()),
                                                        YY_MOVE(yystack_[2].value.as<CNode>())}}}}};
                    }
#line 2995 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 204:
#line 902 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::sqrt, YY_MOVE(yystack_[1].value.as<CNode>())}}};
                    }
#line 3003 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 205:
#line 907 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::subtract,
                             CNode{CNode::ArrayChildren{YY_MOVE(yystack_[3].value.as<CNode>()),
                                                        YY_MOVE(yystack_[2].value.as<CNode>())}}}}};
                    }
#line 3012 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 206:
#line 913 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::trunc,
                             CNode{CNode::ArrayChildren{YY_MOVE(yystack_[3].value.as<CNode>()),
                                                        YY_MOVE(yystack_[2].value.as<CNode>())}}}}};
                    }
#line 3021 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 207:
#line 919 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 3027 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 208:
#line 919 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 3033 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 209:
#line 919 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 3039 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 210:
#line 923 "src/mongo/db/cst/pipeline_grammar.yy"
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
#line 3051 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 211:
#line 933 "src/mongo/db/cst/pipeline_grammar.yy"
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
#line 3063 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 212:
#line 943 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::notExpr,
                             CNode{CNode::ArrayChildren{YY_MOVE(yystack_[2].value.as<CNode>())}}}}};
                    }
#line 3072 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 213:
#line 950 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 3078 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 214:
#line 950 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 3084 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 215:
#line 954 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::constExpr,
                             CNode{CNode::ArrayChildren{YY_MOVE(yystack_[2].value.as<CNode>())}}}}};
                    }
#line 3093 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 216:
#line 961 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::literal,
                             CNode{CNode::ArrayChildren{YY_MOVE(yystack_[2].value.as<CNode>())}}}}};
                    }
#line 3102 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 217:
#line 968 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 3108 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 218:
#line 968 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 3114 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 219:
#line 972 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 3120 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 220:
#line 972 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 3126 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 221:
#line 976 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() =
                            CNode{YY_MOVE(yystack_[1].value.as<std::vector<CNode>>())};
                    }
#line 3134 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 222:
#line 982 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                    }
#line 3140 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 223:
#line 983 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<std::vector<CNode>>() =
                            YY_MOVE(yystack_[0].value.as<std::vector<CNode>>());
                        yylhs.value.as<std::vector<CNode>>().emplace_back(
                            YY_MOVE(yystack_[1].value.as<CNode>()));
                    }
#line 3149 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 224:
#line 990 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[1].value.as<CNode>());
                    }
#line 3157 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 225:
#line 996 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode::noopLeaf();
                    }
#line 3165 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 226:
#line 999 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[1].value.as<CNode>());
                        yylhs.value.as<CNode>().objectChildren().emplace_back(
                            YY_MOVE(yystack_[0].value.as<std::pair<CNode::Fieldname, CNode>>()));
                    }
#line 3174 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 227:
#line 1006 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<std::pair<CNode::Fieldname, CNode>>() = {
                            YY_MOVE(yystack_[1].value.as<CNode::Fieldname>()),
                            YY_MOVE(yystack_[0].value.as<CNode>())};
                    }
#line 3182 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 228:
#line 1013 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() =
                            YY_MOVE(yystack_[0].value.as<CNode::Fieldname>());
                    }
#line 3188 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 229:
#line 1014 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() =
                            YY_MOVE(yystack_[0].value.as<CNode::Fieldname>());
                    }
#line 3194 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 230:
#line 1015 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() =
                            YY_MOVE(yystack_[0].value.as<CNode::Fieldname>());
                    }
#line 3200 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 231:
#line 1016 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() =
                            YY_MOVE(yystack_[0].value.as<CNode::Fieldname>());
                    }
#line 3206 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 232:
#line 1017 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() =
                            YY_MOVE(yystack_[0].value.as<CNode::Fieldname>());
                    }
#line 3212 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 233:
#line 1020 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 3218 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 234:
#line 1020 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 3224 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 235:
#line 1020 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 3230 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 236:
#line 1020 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 3236 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 237:
#line 1020 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 3242 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 238:
#line 1020 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 3248 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 239:
#line 1020 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 3254 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 240:
#line 1022 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::cmp, YY_MOVE(yystack_[1].value.as<CNode>())}}};
                    }
#line 3263 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 241:
#line 1027 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::eq, YY_MOVE(yystack_[1].value.as<CNode>())}}};
                    }
#line 3272 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 242:
#line 1032 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::gt, YY_MOVE(yystack_[1].value.as<CNode>())}}};
                    }
#line 3281 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 243:
#line 1037 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::gte, YY_MOVE(yystack_[1].value.as<CNode>())}}};
                    }
#line 3290 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 244:
#line 1042 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::lt, YY_MOVE(yystack_[1].value.as<CNode>())}}};
                    }
#line 3299 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 245:
#line 1047 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::lte, YY_MOVE(yystack_[1].value.as<CNode>())}}};
                    }
#line 3308 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 246:
#line 1052 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::ne, YY_MOVE(yystack_[1].value.as<CNode>())}}};
                    }
#line 3317 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 247:
#line 1058 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 3323 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 248:
#line 1059 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 3329 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 249:
#line 1060 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 3335 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 250:
#line 1061 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 3341 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 251:
#line 1062 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 3347 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 252:
#line 1063 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 3353 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 253:
#line 1064 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 3359 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 254:
#line 1065 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 3365 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 255:
#line 1066 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 3371 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 256:
#line 1067 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 3377 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 257:
#line 1073 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 3383 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 258:
#line 1073 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 3389 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 259:
#line 1073 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 3395 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 260:
#line 1073 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 3401 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 261:
#line 1073 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 3407 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 262:
#line 1077 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<std::pair<CNode::Fieldname, CNode>>() =
                            std::pair{KeyFieldname::onErrorArg, CNode{KeyValue::absentKey}};
                    }
#line 3415 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 263:
#line 1080 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<std::pair<CNode::Fieldname, CNode>>() = std::pair{
                            KeyFieldname::onErrorArg, YY_MOVE(yystack_[0].value.as<CNode>())};
                    }
#line 3423 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 264:
#line 1087 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<std::pair<CNode::Fieldname, CNode>>() =
                            std::pair{KeyFieldname::onNullArg, CNode{KeyValue::absentKey}};
                    }
#line 3431 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 265:
#line 1090 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<std::pair<CNode::Fieldname, CNode>>() = std::pair{
                            KeyFieldname::onNullArg, YY_MOVE(yystack_[0].value.as<CNode>())};
                    }
#line 3439 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 266:
#line 1096 "src/mongo/db/cst/pipeline_grammar.yy"
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
#line 3450 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 267:
#line 1105 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::toBool, YY_MOVE(yystack_[1].value.as<CNode>())}}};
                    }
#line 3458 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 268:
#line 1110 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::toDate, YY_MOVE(yystack_[1].value.as<CNode>())}}};
                    }
#line 3466 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 269:
#line 1115 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::toDecimal, YY_MOVE(yystack_[1].value.as<CNode>())}}};
                    }
#line 3474 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 270:
#line 1120 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::toDouble, YY_MOVE(yystack_[1].value.as<CNode>())}}};
                    }
#line 3482 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 271:
#line 1125 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::toInt, YY_MOVE(yystack_[1].value.as<CNode>())}}};
                    }
#line 3490 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 272:
#line 1130 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::toLong, YY_MOVE(yystack_[1].value.as<CNode>())}}};
                    }
#line 3498 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 273:
#line 1135 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::toObjectId, YY_MOVE(yystack_[1].value.as<CNode>())}}};
                    }
#line 3506 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 274:
#line 1140 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::toString, YY_MOVE(yystack_[1].value.as<CNode>())}}};
                    }
#line 3514 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 275:
#line 1145 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::type, YY_MOVE(yystack_[1].value.as<CNode>())}}};
                    }
#line 3522 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;


#line 3526 "src/mongo/db/cst/pipeline_parser_gen.cpp"

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


const short PipelineParserGen::yypact_ninf_ = -274;

const signed char PipelineParserGen::yytable_ninf_ = -1;

const short PipelineParserGen::yypact_[] = {
    -64,  13,   44,   51,   50,   -274, -274, -274, -274, 166,  48,   292,  52,   81,   53,   55,
    81,   -274, 57,   -274, -274, -274, -274, -274, -274, -274, -274, 165,  -274, -274, -274, -274,
    -274, -274, -274, -274, -274, -274, -274, -274, -274, -274, -274, -274, -274, -274, -274, -274,
    -274, -274, -274, -274, -274, -274, -274, -274, -274, -274, -274, -274, -274, -274, -274, -274,
    -274, -274, -274, -274, -274, -274, -274, -274, -274, -274, -274, -274, -274, -274, -274, -274,
    -274, -274, 165,  -274, -274, -274, -274, 59,   -274, -274, -274, -274, -274, -274, -274, -274,
    -274, -274, -274, -274, -274, -274, 37,   -274, 45,   61,   50,   -274, 165,  -274, -274, -274,
    -274, -274, -274, -274, -274, -274, -274, -274, -274, -274, -274, -274, -274, -274, -274, -274,
    -274, -274, -274, -274, -274, -274, -274, -274, -274, -274, -274, -274, -274, -274, -274, -274,
    -274, -274, -274, -274, -274, -274, -274, -274, 392,  81,   1,    -274, -274, 455,  165,  64,
    -274, 124,  124,  -274, -274, -274, -274, -274, 67,   54,   -274, -274, -274, -274, -274, -274,
    -274, 165,  -274, -274, -274, 562,  211,  -274, -274, -274, -274, -274, -274, -274, -274, -274,
    -274, -274, -274, -274, -274, -274, -274, -274, -274, -274, -274, -274, -274, -274, -274, -274,
    -274, -274, -274, -274, -274, -274, -274, -274, -274, -274, -274, -274, -274, -274, -274, -274,
    -274, -274, -274, -274, -274, -274, -274, -274, -274, -274, -274, -274, -274, -274, -274, -274,
    -274, -274, -274, -274, 3,    -274, 69,   71,   72,   73,   75,   76,   77,   71,   71,   71,
    71,   71,   71,   71,   -274, 211,  211,  211,  211,  211,  211,  211,  211,  211,  211,  211,
    78,   211,  211,  211,  80,   211,  82,   89,   90,   91,   211,  93,   98,   518,  -274, 211,
    -274, 100,  104,  211,  211,  106,  211,  165,  165,  211,  211,  108,  109,  111,  114,  117,
    119,  120,  24,   121,  122,  126,  135,  140,  154,  156,  160,  161,  162,  163,  211,  167,
    168,  175,  211,  182,  211,  211,  211,  211,  183,  211,  211,  -274, 211,  -274, -274, -274,
    -274, -274, -274, -274, -274, 211,  211,  -274, 211,  190,  193,  211,  194,  -274, -274, -274,
    -274, -274, -274, -274, 211,  -274, -274, -274, -274, -274, -274, -274, -274, -274, -274, -274,
    211,  -274, -274, -274, 211,  -274, 211,  211,  211,  211,  -274, 211,  211,  -274, 211,  195,
    211,  185,  186,  211,  199,  65,   201,  202,  204,  211,  205,  206,  209,  212,  219,  -274,
    220,  -274, -274, 221,  -274, 184,  213,  224,  225,  244,  226,  248,  249,  250,  251,  252,
    253,  -274, -274, -274, -274, -274, 105,  -274, -274, -274, 254,  -274, -274, -274, -274, -274,
    -274, -274, 211,  123,  -274, -274, 211,  256,  -274, 257,  -274};

const short PipelineParserGen::yydefact_[] = {
    0,   0,   0,   0,   5,   2,   46,  3,   1,   0,   0,   0,   0,   0,   0,   0,   0,   7,   0,
    9,   10,  11,  12,  13,  14,  4,   45,  0,   56,  59,  60,  61,  58,  57,  62,  63,  64,  69,
    70,  71,  72,  73,  74,  75,  76,  77,  78,  79,  80,  81,  82,  83,  84,  85,  86,  87,  88,
    89,  90,  91,  92,  93,  94,  95,  96,  97,  98,  99,  100, 101, 102, 103, 104, 105, 106, 107,
    65,  66,  67,  68,  55,  52,  0,   53,  54,  51,  47,  0,   123, 125, 127, 129, 122, 124, 126,
    128, 18,  19,  20,  21,  23,  25,  0,   22,  0,   0,   5,   225, 222, 130, 131, 108, 109, 110,
    111, 112, 113, 114, 115, 116, 117, 118, 119, 120, 121, 139, 140, 141, 142, 143, 148, 144, 145,
    146, 149, 150, 50,  132, 133, 134, 135, 147, 136, 137, 138, 217, 218, 219, 220, 48,  49,  16,
    0,   0,   0,   8,   6,   0,   222, 0,   24,  0,   0,   42,  43,  44,  41,  26,  0,   0,   224,
    172, 229, 230, 231, 228, 232, 0,   226, 223, 221, 165, 151, 31,  33,  35,  37,  38,  39,  30,
    32,  34,  36,  29,  27,  40,  156, 157, 158, 173, 174, 159, 207, 208, 209, 160, 213, 214, 161,
    233, 234, 235, 236, 237, 238, 239, 162, 247, 248, 249, 250, 251, 252, 253, 254, 255, 256, 175,
    176, 177, 178, 179, 180, 181, 182, 183, 184, 185, 186, 187, 188, 189, 28,  15,  0,   227, 0,
    0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   7,   0,   0,   0,   0,   0,
    0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,
    0,   153, 151, 154, 0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,
    0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,
    0,   0,   0,   0,   0,   0,   0,   0,   164, 0,   169, 170, 168, 171, 166, 152, 163, 17,  0,
    0,   191, 0,   0,   0,   0,   0,   240, 241, 242, 243, 244, 245, 246, 0,   267, 268, 269, 270,
    271, 272, 273, 274, 275, 192, 193, 0,   195, 196, 197, 0,   199, 0,   0,   0,   0,   204, 0,
    0,   167, 151, 0,   151, 0,   0,   151, 0,   0,   0,   0,   0,   151, 0,   0,   0,   0,   0,
    155, 0,   215, 216, 0,   212, 0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   258,
    259, 260, 261, 257, 262, 194, 198, 200, 0,   202, 203, 205, 206, 190, 210, 211, 0,   264, 201,
    263, 0,   0,   265, 0,   266};

const short PipelineParserGen::yypgoto_[] = {
    -274, -274, -274, -140, -274, -132, -133, -129, -22,  -274, -274, -274, -274, -274, -274, -274,
    -274, -274, -12,  -274, -11,  -13,  -7,   -274, -274, -98,  -146, -274, -274, -274, -274, -274,
    -274, -274, -20,  -274, -274, -274, -274, 164,  -274, -274, -274, -274, -274, -274, -274, -274,
    107,  -5,   -225, -135, -224, -274, -274, -274, -274, -274, -274, -274, -274, -274, -274, -274,
    -274, -274, -274, -274, -274, -274, -274, -274, -274, -274, -274, -274, -274, -274, -274, -274,
    -274, -274, -274, -274, -274, -274, -274, -274, -274, -274, -274, -274, -274, -274, -274, -274,
    -274, -274, -274, -274, -274, -274, -274, -273, 110,  -274, -274, 189,  -274, -274, 7,    -274};

const short PipelineParserGen::yydefgoto_[] = {
    -1,  162, 332, 81,  82,  83,  84,  85,  176, 177, 167, 337, 178, 86,  125, 126, 127, 128, 129,
    130, 131, 132, 133, 134, 135, 136, 137, 138, 139, 140, 141, 142, 143, 144, 286, 146, 147, 148,
    157, 10,  18,  19,  20,  21,  22,  23,  24,  152, 194, 100, 287, 288, 293, 196, 197, 285, 198,
    199, 200, 201, 202, 203, 204, 205, 206, 207, 208, 209, 210, 211, 212, 213, 214, 215, 216, 422,
    217, 218, 219, 220, 221, 222, 223, 224, 225, 226, 227, 228, 229, 230, 231, 232, 233, 234, 235,
    236, 237, 238, 239, 240, 241, 435, 439, 289, 159, 7,   11,  149, 3,   5,   104, 105};

const short PipelineParserGen::yytable_[] = {
    98,  96,  97,  98,  96,  97,  99,  145, 169, 99,  158, 103, 163, 90,  338, 193, 193, 172, 4,
    165, 164, 1,   2,   166, 174, 173, 195, 195, 175, 299, 300, 301, 302, 303, 304, 305, 307, 308,
    309, 310, 311, 312, 313, 314, 315, 316, 317, 6,   319, 320, 321, 8,   323, 9,   25,  87,  101,
    328, 102, 153, 158, 106, 145, 151, 155, 154, 341, 342, 111, 344, 180, 243, 347, 348, 291, 244,
    292, 294, 295, 245, 296, 297, 298, 318, 94,  322, 356, 324, 145, 88,  89,  90,  91,  368, 325,
    326, 327, 372, 329, 374, 375, 376, 377, 330, 379, 380, 339, 381, 340, 398, 343, 400, 349, 350,
    403, 351, 382, 383, 352, 384, 409, 353, 387, 354, 355, 357, 358, 181, 405, 182, 359, 389, 183,
    184, 185, 186, 187, 188, 145, 360, 98,  96,  97,  390, 361, 333, 99,  391, 168, 392, 393, 394,
    395, 334, 396, 397, 335, 145, 362, 92,  363, 93,  94,  95,  364, 365, 366, 367, 107, 434, 108,
    369, 370, 88,  89,  90,  91,  109, 110, 371, 12,  13,  14,  15,  16,  17,  373, 378, 438, 401,
    402, 111, 88,  89,  90,  91,  385, 345, 346, 386, 388, 399, 189, 404, 190, 191, 192, 406, 407,
    437, 408, 410, 411, 440, 181, 412, 182, 423, 413, 88,  89,  90,  91,  109, 110, 414, 415, 416,
    424, 425, 427, 290, 111, 112, 113, 114, 115, 116, 117, 118, 119, 120, 121, 92,  122, 93,  94,
    95,  123, 124, 426, 111, 428, 429, 430, 431, 432, 433, 436, 421, 441, 442, 92,  336, 93,  94,
    95,  306, 179, 242, 156, 150, 0,   0,   0,   145, 145, 0,   111, 112, 113, 114, 115, 116, 117,
    118, 119, 120, 121, 92,  122, 93,  94,  95,  123, 124, 26,  0,   0,   27,  0,   0,   0,   0,
    0,   0,   28,  29,  30,  31,  32,  33,  34,  35,  36,  37,  38,  39,  40,  41,  42,  43,  44,
    45,  46,  47,  48,  49,  50,  51,  52,  53,  54,  55,  56,  57,  58,  59,  60,  61,  62,  63,
    64,  65,  66,  67,  68,  69,  70,  71,  72,  73,  74,  75,  76,  77,  78,  79,  80,  0,   0,
    0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,
    0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   419, 417, 418, 0,   160, 0,   420,
    161, 0,   0,   0,   0,   0,   0,   28,  29,  30,  31,  32,  33,  34,  35,  36,  37,  38,  39,
    40,  41,  42,  43,  44,  45,  46,  47,  48,  49,  50,  51,  52,  53,  54,  55,  56,  57,  58,
    59,  60,  61,  62,  63,  64,  65,  66,  67,  68,  69,  70,  71,  72,  73,  74,  75,  76,  77,
    78,  79,  80,  170, 0,   0,   171, 0,   0,   0,   0,   0,   0,   28,  29,  30,  31,  32,  33,
    34,  35,  36,  37,  38,  39,  40,  41,  42,  43,  44,  45,  46,  47,  48,  49,  50,  51,  52,
    53,  54,  55,  56,  57,  58,  59,  60,  61,  62,  63,  64,  65,  66,  67,  68,  69,  70,  71,
    72,  73,  74,  75,  76,  77,  78,  79,  80,  331, 0,   0,   171, 0,   0,   0,   0,   0,   0,
    28,  29,  30,  31,  32,  33,  34,  35,  36,  0,   0,   0,   0,   0,   0,   0,   0,   0,   0,
    0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,
    0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   76,  77,  78,  79,  80,  246, 247, 248, 249,
    250, 251, 252, 253, 254, 255, 256, 257, 258, 259, 260, 261, 262, 263, 264, 265, 266, 267, 268,
    269, 270, 271, 272, 273, 274, 275, 276, 277, 278, 279, 280, 281, 282, 283, 284};

const short PipelineParserGen::yycheck_[] = {
    13,  13,  13,  16,  16,  16,  13,  27,  154, 16,  108, 16,  152, 10,  287, 161, 162, 157, 5,
    152, 152, 85,  86,  152, 157, 157, 161, 162, 157, 253, 254, 255, 256, 257, 258, 259, 261, 262,
    263, 264, 265, 266, 267, 268, 269, 270, 271, 3,   273, 274, 275, 0,   277, 3,   6,   3,   3,
    282, 3,   22,  158, 4,   82,  4,   3,   20,  291, 292, 67,  294, 6,   4,   297, 298, 5,   21,
    5,   5,   5,   177, 5,   5,   5,   5,   81,  5,   62,  5,   108, 8,   9,   10,  11,  318, 5,
    5,   5,   322, 5,   324, 325, 326, 327, 5,   329, 330, 6,   332, 4,   382, 4,   384, 4,   4,
    387, 4,   341, 342, 4,   344, 393, 4,   347, 4,   4,   4,   4,   3,   63,  5,   4,   356, 8,
    9,   10,  11,  12,  13,  158, 4,   153, 153, 153, 368, 4,   285, 153, 372, 153, 374, 375, 376,
    377, 285, 379, 380, 285, 177, 4,   78,  4,   80,  81,  82,  4,   4,   4,   4,   3,   64,  5,
    4,   4,   8,   9,   10,  11,  12,  13,  4,   14,  15,  16,  17,  18,  19,  4,   4,   65,  4,
    4,   67,  8,   9,   10,  11,  6,   295, 296, 6,   6,   6,   78,  4,   80,  81,  82,  6,   6,
    434, 6,   6,   6,   438, 3,   6,   5,   4,   6,   8,   9,   10,  11,  12,  13,  6,   6,   6,
    4,   4,   4,   244, 67,  68,  69,  70,  71,  72,  73,  74,  75,  76,  77,  78,  79,  80,  81,
    82,  83,  84,  6,   67,  4,   4,   4,   4,   4,   4,   4,   405, 4,   4,   78,  285, 80,  81,
    82,  260, 158, 162, 106, 82,  -1,  -1,  -1,  295, 296, -1,  67,  68,  69,  70,  71,  72,  73,
    74,  75,  76,  77,  78,  79,  80,  81,  82,  83,  84,  4,   -1,  -1,  7,   -1,  -1,  -1,  -1,
    -1,  -1,  14,  15,  16,  17,  18,  19,  20,  21,  22,  23,  24,  25,  26,  27,  28,  29,  30,
    31,  32,  33,  34,  35,  36,  37,  38,  39,  40,  41,  42,  43,  44,  45,  46,  47,  48,  49,
    50,  51,  52,  53,  54,  55,  56,  57,  58,  59,  60,  61,  62,  63,  64,  65,  66,  -1,  -1,
    -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,
    -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  405, 405, 405, -1,  4,   -1,  405,
    7,   -1,  -1,  -1,  -1,  -1,  -1,  14,  15,  16,  17,  18,  19,  20,  21,  22,  23,  24,  25,
    26,  27,  28,  29,  30,  31,  32,  33,  34,  35,  36,  37,  38,  39,  40,  41,  42,  43,  44,
    45,  46,  47,  48,  49,  50,  51,  52,  53,  54,  55,  56,  57,  58,  59,  60,  61,  62,  63,
    64,  65,  66,  4,   -1,  -1,  7,   -1,  -1,  -1,  -1,  -1,  -1,  14,  15,  16,  17,  18,  19,
    20,  21,  22,  23,  24,  25,  26,  27,  28,  29,  30,  31,  32,  33,  34,  35,  36,  37,  38,
    39,  40,  41,  42,  43,  44,  45,  46,  47,  48,  49,  50,  51,  52,  53,  54,  55,  56,  57,
    58,  59,  60,  61,  62,  63,  64,  65,  66,  4,   -1,  -1,  7,   -1,  -1,  -1,  -1,  -1,  -1,
    14,  15,  16,  17,  18,  19,  20,  21,  22,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,
    -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,
    -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  62,  63,  64,  65,  66,  23,  24,  25,  26,
    27,  28,  29,  30,  31,  32,  33,  34,  35,  36,  37,  38,  39,  40,  41,  42,  43,  44,  45,
    46,  47,  48,  49,  50,  51,  52,  53,  54,  55,  56,  57,  58,  59,  60,  61};

const unsigned char PipelineParserGen::yystos_[] = {
    0,   85,  86,  195, 5,   196, 3,   192, 0,   3,   126, 193, 14,  15,  16,  17,  18,  19,  127,
    128, 129, 130, 131, 132, 133, 6,   4,   7,   14,  15,  16,  17,  18,  19,  20,  21,  22,  23,
    24,  25,  26,  27,  28,  29,  30,  31,  32,  33,  34,  35,  36,  37,  38,  39,  40,  41,  42,
    43,  44,  45,  46,  47,  48,  49,  50,  51,  52,  53,  54,  55,  56,  57,  58,  59,  60,  61,
    62,  63,  64,  65,  66,  90,  91,  92,  93,  94,  100, 3,   8,   9,   10,  11,  78,  80,  81,
    82,  105, 107, 108, 109, 136, 3,   3,   136, 197, 198, 4,   3,   5,   12,  13,  67,  68,  69,
    70,  71,  72,  73,  74,  75,  76,  77,  79,  83,  84,  101, 102, 103, 104, 105, 106, 107, 108,
    109, 110, 111, 112, 113, 114, 115, 116, 117, 118, 119, 120, 121, 122, 123, 124, 194, 194, 4,
    134, 22,  20,  3,   126, 125, 112, 191, 4,   7,   88,  90,  92,  93,  94,  97,  136, 113, 4,
    7,   90,  92,  93,  94,  95,  96,  99,  191, 6,   3,   5,   8,   9,   10,  11,  12,  13,  78,
    80,  81,  82,  113, 135, 138, 140, 141, 143, 144, 145, 146, 147, 148, 149, 150, 151, 152, 153,
    154, 155, 156, 157, 158, 159, 160, 161, 163, 164, 165, 166, 167, 168, 169, 170, 171, 172, 173,
    174, 175, 176, 177, 178, 179, 180, 181, 182, 183, 184, 185, 186, 187, 135, 4,   21,  112, 23,
    24,  25,  26,  27,  28,  29,  30,  31,  32,  33,  34,  35,  36,  37,  38,  39,  40,  41,  42,
    43,  44,  45,  46,  47,  48,  49,  50,  51,  52,  53,  54,  55,  56,  57,  58,  59,  60,  61,
    142, 121, 137, 138, 190, 108, 5,   5,   139, 5,   5,   5,   5,   5,   139, 139, 139, 139, 139,
    139, 139, 197, 137, 137, 137, 137, 137, 137, 137, 137, 137, 137, 137, 5,   137, 137, 137, 5,
    137, 5,   5,   5,   5,   137, 5,   5,   4,   89,  90,  92,  94,  95,  98,  190, 6,   4,   137,
    137, 4,   137, 112, 112, 137, 137, 4,   4,   4,   4,   4,   4,   4,   62,  4,   4,   4,   4,
    4,   4,   4,   4,   4,   4,   4,   137, 4,   4,   4,   137, 4,   137, 137, 137, 137, 4,   137,
    137, 137, 137, 137, 137, 6,   6,   137, 6,   137, 137, 137, 137, 137, 137, 137, 137, 137, 190,
    6,   190, 4,   4,   190, 4,   63,  6,   6,   6,   190, 6,   6,   6,   6,   6,   6,   6,   105,
    107, 108, 109, 113, 162, 4,   4,   4,   6,   4,   4,   4,   4,   4,   4,   4,   64,  188, 4,
    137, 65,  189, 137, 4,   4};

const unsigned char PipelineParserGen::yyr1_[] = {
    0,   87,  195, 195, 196, 126, 126, 198, 197, 127, 127, 127, 127, 127, 127, 133, 128, 129, 136,
    136, 136, 136, 130, 131, 132, 134, 134, 97,  97,  135, 135, 135, 135, 135, 135, 135, 135, 135,
    135, 135, 135, 88,  88,  88,  88,  192, 193, 193, 100, 100, 194, 91,  91,  91,  91,  94,  90,
    90,  90,  90,  90,  90,  92,  92,  92,  92,  92,  92,  92,  93,  93,  93,  93,  93,  93,  93,
    93,  93,  93,  93,  93,  93,  93,  93,  93,  93,  93,  93,  93,  93,  93,  93,  93,  93,  93,
    93,  93,  93,  93,  93,  93,  93,  93,  93,  93,  93,  93,  93,  113, 114, 115, 116, 118, 119,
    120, 101, 102, 103, 104, 106, 110, 111, 105, 105, 107, 107, 108, 108, 109, 109, 117, 117, 121,
    121, 121, 121, 121, 121, 121, 121, 121, 121, 121, 121, 121, 121, 121, 121, 121, 121, 121, 190,
    190, 137, 137, 139, 138, 138, 138, 138, 138, 138, 138, 140, 141, 142, 142, 98,  89,  89,  89,
    89,  95,  143, 143, 143, 143, 143, 143, 143, 143, 143, 143, 143, 143, 143, 143, 143, 143, 143,
    144, 145, 173, 174, 175, 176, 177, 178, 179, 180, 181, 182, 183, 184, 185, 186, 187, 146, 146,
    146, 147, 148, 149, 150, 150, 151, 152, 112, 112, 122, 122, 123, 191, 191, 124, 125, 125, 99,
    96,  96,  96,  96,  96,  153, 153, 153, 153, 153, 153, 153, 154, 155, 156, 157, 158, 159, 160,
    161, 161, 161, 161, 161, 161, 161, 161, 161, 161, 162, 162, 162, 162, 162, 188, 188, 189, 189,
    163, 164, 165, 166, 167, 168, 169, 170, 171, 172};

const signed char PipelineParserGen::yyr2_[] = {
    0, 2, 2, 2, 3, 0, 4, 0, 2, 1, 1, 1, 1, 1, 1, 5, 3, 7, 1,  1, 1, 1, 2, 2, 4, 0, 2, 2, 2, 1, 1,
    1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 3, 0, 2, 2, 2,  1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,
    1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,  1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,
    1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,  1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,
    1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,  1, 1, 1, 1, 1, 1, 1, 1, 0, 2, 1, 1,
    4, 1, 1, 1, 1, 1, 1, 1, 3, 3, 0, 2, 2, 1, 1, 1, 1, 1, 1,  1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,
    1, 1, 1, 1, 8, 4, 4, 4, 7, 4, 4, 4, 7, 4, 7, 8, 7, 7, 4,  7, 7, 1, 1, 1, 8, 8, 6, 1, 1, 6, 6,
    1, 1, 1, 1, 3, 0, 2, 3, 0, 2, 2, 1, 1, 1, 1, 1, 1, 1, 1,  1, 1, 1, 1, 4, 4, 4, 4, 4, 4, 4, 1,
    1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 0, 2, 0, 2, 11, 4, 4, 4, 4, 4, 4, 4, 4, 4};


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
                                                   "START_PIPELINE",
                                                   "START_MATCH",
                                                   "$accept",
                                                   "projectionFieldname",
                                                   "expressionFieldname",
                                                   "stageAsUserFieldname",
                                                   "filterFieldname",
                                                   "argAsUserFieldname",
                                                   "aggExprAsUserFieldname",
                                                   "invariableUserFieldname",
                                                   "idAsUserFieldname",
                                                   "valueFieldname",
                                                   "projectField",
                                                   "expressionField",
                                                   "valueField",
                                                   "filterField",
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
                                                   "matchExpression",
                                                   "filterFields",
                                                   "filterVal",
                                                   "start",
                                                   "pipeline",
                                                   "START_ORDERED_OBJECT",
                                                   "$@1",
                                                   YY_NULLPTR};
#endif


#if YYDEBUG
const short PipelineParserGen::yyrline_[] = {
    0,    248,  248,  249,  256,  262,  263,  271,  271,  274,  274,  274,  274,  274,  274,  277,
    287,  293,  303,  303,  303,  303,  307,  312,  317,  323,  326,  333,  336,  342,  343,  346,
    349,  352,  355,  358,  361,  364,  367,  370,  373,  377,  377,  377,  377,  381,  387,  390,
    397,  400,  406,  410,  410,  410,  410,  414,  422,  425,  428,  431,  434,  437,  446,  449,
    452,  455,  458,  461,  464,  472,  475,  478,  481,  484,  487,  490,  493,  496,  499,  502,
    505,  508,  511,  514,  517,  520,  523,  526,  529,  532,  535,  538,  541,  544,  547,  550,
    553,  556,  559,  562,  565,  568,  571,  574,  577,  580,  583,  586,  593,  599,  605,  611,
    617,  623,  629,  635,  641,  647,  653,  659,  665,  671,  677,  680,  686,  689,  695,  698,
    704,  707,  713,  716,  722,  723,  724,  725,  726,  727,  728,  729,  730,  731,  732,  733,
    734,  735,  736,  737,  738,  739,  740,  747,  748,  755,  755,  759,  764,  764,  764,  764,
    764,  764,  765,  771,  779,  785,  788,  795,  802,  802,  802,  802,  806,  812,  812,  812,
    812,  812,  812,  812,  812,  812,  812,  812,  812,  812,  813,  813,  813,  813,  817,  827,
    833,  838,  843,  849,  854,  859,  864,  870,  875,  881,  890,  896,  902,  907,  913,  919,
    919,  919,  923,  933,  943,  950,  950,  954,  961,  968,  968,  972,  972,  976,  982,  983,
    990,  996,  999,  1006, 1013, 1014, 1015, 1016, 1017, 1020, 1020, 1020, 1020, 1020, 1020, 1020,
    1022, 1027, 1032, 1037, 1042, 1047, 1052, 1058, 1059, 1060, 1061, 1062, 1063, 1064, 1065, 1066,
    1067, 1073, 1073, 1073, 1073, 1073, 1077, 1080, 1087, 1090, 1096, 1105, 1110, 1115, 1120, 1125,
    1130, 1135, 1140, 1145};

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


#line 58 "src/mongo/db/cst/pipeline_grammar.yy"
}  // namespace mongo
#line 4205 "src/mongo/db/cst/pipeline_parser_gen.cpp"

#line 1149 "src/mongo/db/cst/pipeline_grammar.yy"
