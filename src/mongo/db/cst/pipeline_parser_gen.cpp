// A Bison parser, made by GNU Bison 3.5.4.

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

// Undocumented macros, especially those whose name start with YY_,
// are private implementation details.  Do not rely on them.


#include "pipeline_parser_gen.hpp"


// Unqualified %code blocks.
#line 83 "src/mongo/db/cst/pipeline_grammar.yy"

#include "mongo/db/cst/bson_lexer.h"
#include "mongo/db/cst/c_node_validation.h"
#include "mongo/db/cst/key_fieldname.h"
#include "mongo/platform/decimal128.h"
#include "mongo/stdx/variant.h"

namespace mongo {
// Mandatory error function.
void PipelineParserGen::error(const PipelineParserGen::location_type& loc, const std::string& msg) {
    uasserted(ErrorCodes::FailedToParse, str::stream() << msg << " at element " << loc);
}
}  // namespace mongo

// Default location for actions, called each time a rule is matched but before the action is
// run. Also called when bison encounters a syntax ambiguity, which should not be relevant for
// mongo.
#define YYLLOC_DEFAULT(newPos, rhsPositions, nRhs)

#line 66 "src/mongo/db/cst/pipeline_parser_gen.cpp"


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

#define YY_STACK_PRINT()      \
    do {                      \
        if (yydebug_)         \
            yystack_print_(); \
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
#line 158 "src/mongo/db/cst/pipeline_parser_gen.cpp"


/* Return YYSTR after stripping away unnecessary quotes and
   backslashes, so that it's suitable for yyerror.  The heuristic is
   that double-quoting is unnecessary unless the string contains an
   apostrophe, a comma, or backslash (other than backslash-backslash).
   YYSTR is taken from yytname.  */
std::string PipelineParserGen::yytnamerr_(const char* yystr) {
    if (*yystr == '"') {
        std::string yyr;
        char const* yyp = yystr;

        for (;;)
            switch (*++yyp) {
                case '\'':
                case ',':
                    goto do_not_strip_quotes;

                case '\\':
                    if (*++yyp != '\\')
                        goto do_not_strip_quotes;
                    else
                        goto append;

                append:
                default:
                    yyr += *yyp;
                    break;

                case '"':
                    return yyr;
            }
    do_not_strip_quotes:;
    }

    return yystr;
}


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
| Symbol types.  |
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

PipelineParserGen::symbol_number_type PipelineParserGen::by_state::type_get() const YY_NOEXCEPT {
    if (state == empty_state)
        return empty_symbol;
    else
        return yystos_[+state];
}

PipelineParserGen::stack_symbol_type::stack_symbol_type() {}

PipelineParserGen::stack_symbol_type::stack_symbol_type(YY_RVREF(stack_symbol_type) that)
    : super_type(YY_MOVE(that.state), YY_MOVE(that.location)) {
    switch (that.type_get()) {
        case 99:  // "BinData"
            value.YY_MOVE_OR_COPY<BSONBinData>(YY_MOVE(that.value));
            break;

        case 106:  // "Code"
            value.YY_MOVE_OR_COPY<BSONCode>(YY_MOVE(that.value));
            break;

        case 108:  // "CodeWScope"
            value.YY_MOVE_OR_COPY<BSONCodeWScope>(YY_MOVE(that.value));
            break;

        case 105:  // "dbPointer"
            value.YY_MOVE_OR_COPY<BSONDBRef>(YY_MOVE(that.value));
            break;

        case 104:  // "regex"
            value.YY_MOVE_OR_COPY<BSONRegEx>(YY_MOVE(that.value));
            break;

        case 107:  // "Symbol"
            value.YY_MOVE_OR_COPY<BSONSymbol>(YY_MOVE(that.value));
            break;

        case 132:  // dbPointer
        case 133:  // javascript
        case 134:  // symbol
        case 135:  // javascriptWScope
        case 136:  // int
        case 137:  // timestamp
        case 138:  // long
        case 139:  // double
        case 140:  // decimal
        case 141:  // minKey
        case 142:  // maxKey
        case 143:  // value
        case 144:  // string
        case 145:  // binary
        case 146:  // undefined
        case 147:  // objectId
        case 148:  // bool
        case 149:  // date
        case 150:  // null
        case 151:  // regex
        case 152:  // simpleValue
        case 153:  // compoundValue
        case 154:  // valueArray
        case 155:  // valueObject
        case 156:  // valueFields
        case 157:  // stageList
        case 158:  // stage
        case 159:  // inhibitOptimization
        case 160:  // unionWith
        case 161:  // skip
        case 162:  // limit
        case 163:  // project
        case 164:  // sample
        case 165:  // projectFields
        case 166:  // projection
        case 167:  // num
        case 168:  // expression
        case 169:  // compoundExpression
        case 170:  // exprFixedTwoArg
        case 171:  // expressionArray
        case 172:  // expressionObject
        case 173:  // expressionFields
        case 174:  // maths
        case 175:  // add
        case 176:  // atan2
        case 177:  // boolExps
        case 178:  // and
        case 179:  // or
        case 180:  // not
        case 181:  // literalEscapes
        case 182:  // const
        case 183:  // literal
        case 184:  // stringExps
        case 185:  // concat
        case 186:  // dateFromString
        case 187:  // dateToString
        case 188:  // indexOfBytes
        case 189:  // indexOfCP
        case 190:  // ltrim
        case 191:  // regexFind
        case 192:  // regexFindAll
        case 193:  // regexMatch
        case 194:  // regexArgs
        case 195:  // replaceOne
        case 196:  // replaceAll
        case 197:  // rtrim
        case 198:  // split
        case 199:  // strLenBytes
        case 200:  // strLenCP
        case 201:  // strcasecmp
        case 202:  // substr
        case 203:  // substrBytes
        case 204:  // substrCP
        case 205:  // toLower
        case 206:  // toUpper
        case 207:  // trim
        case 208:  // compExprs
        case 209:  // cmp
        case 210:  // eq
        case 211:  // gt
        case 212:  // gte
        case 213:  // lt
        case 214:  // lte
        case 215:  // ne
        case 216:  // typeExpression
        case 217:  // convert
        case 218:  // toBool
        case 219:  // toDate
        case 220:  // toDecimal
        case 221:  // toDouble
        case 222:  // toInt
        case 223:  // toLong
        case 224:  // toObjectId
        case 225:  // toString
        case 226:  // type
        case 227:  // abs
        case 228:  // ceil
        case 229:  // divide
        case 230:  // exponent
        case 231:  // floor
        case 232:  // ln
        case 233:  // log
        case 234:  // logten
        case 235:  // mod
        case 236:  // multiply
        case 237:  // pow
        case 238:  // round
        case 239:  // sqrt
        case 240:  // subtract
        case 241:  // trunc
        case 251:  // matchExpression
        case 252:  // filterFields
        case 253:  // filterVal
            value.YY_MOVE_OR_COPY<CNode>(YY_MOVE(that.value));
            break;

        case 119:  // projectionFieldname
        case 120:  // expressionFieldname
        case 121:  // stageAsUserFieldname
        case 122:  // filterFieldname
        case 123:  // argAsUserFieldname
        case 124:  // aggExprAsUserFieldname
        case 125:  // invariableUserFieldname
        case 126:  // idAsUserFieldname
        case 127:  // valueFieldname
            value.YY_MOVE_OR_COPY<CNode::Fieldname>(YY_MOVE(that.value));
            break;

        case 102:  // "Date"
            value.YY_MOVE_OR_COPY<Date_t>(YY_MOVE(that.value));
            break;

        case 112:  // "non-zero decimal"
            value.YY_MOVE_OR_COPY<Decimal128>(YY_MOVE(that.value));
            break;

        case 101:  // "ObjectID"
            value.YY_MOVE_OR_COPY<OID>(YY_MOVE(that.value));
            break;

        case 113:  // "Timestamp"
            value.YY_MOVE_OR_COPY<Timestamp>(YY_MOVE(that.value));
            break;

        case 115:  // "maxKey"
            value.YY_MOVE_OR_COPY<UserMaxKey>(YY_MOVE(that.value));
            break;

        case 114:  // "minKey"
            value.YY_MOVE_OR_COPY<UserMinKey>(YY_MOVE(that.value));
            break;

        case 103:  // "null"
            value.YY_MOVE_OR_COPY<UserNull>(YY_MOVE(that.value));
            break;

        case 100:  // "undefined"
            value.YY_MOVE_OR_COPY<UserUndefined>(YY_MOVE(that.value));
            break;

        case 111:  // "non-zero double"
            value.YY_MOVE_OR_COPY<double>(YY_MOVE(that.value));
            break;

        case 109:  // "non-zero integer"
            value.YY_MOVE_OR_COPY<int>(YY_MOVE(that.value));
            break;

        case 110:  // "non-zero long"
            value.YY_MOVE_OR_COPY<long long>(YY_MOVE(that.value));
            break;

        case 128:  // projectField
        case 129:  // expressionField
        case 130:  // valueField
        case 131:  // filterField
        case 242:  // onErrorArg
        case 243:  // onNullArg
        case 244:  // formatArg
        case 245:  // timezoneArg
        case 246:  // charsArg
        case 247:  // optionsArg
            value.YY_MOVE_OR_COPY<std::pair<CNode::Fieldname, CNode>>(YY_MOVE(that.value));
            break;

        case 97:  // "fieldname"
        case 98:  // "string"
            value.YY_MOVE_OR_COPY<std::string>(YY_MOVE(that.value));
            break;

        case 248:  // expressions
        case 249:  // values
        case 250:  // exprZeroToTwo
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
    switch (that.type_get()) {
        case 99:  // "BinData"
            value.move<BSONBinData>(YY_MOVE(that.value));
            break;

        case 106:  // "Code"
            value.move<BSONCode>(YY_MOVE(that.value));
            break;

        case 108:  // "CodeWScope"
            value.move<BSONCodeWScope>(YY_MOVE(that.value));
            break;

        case 105:  // "dbPointer"
            value.move<BSONDBRef>(YY_MOVE(that.value));
            break;

        case 104:  // "regex"
            value.move<BSONRegEx>(YY_MOVE(that.value));
            break;

        case 107:  // "Symbol"
            value.move<BSONSymbol>(YY_MOVE(that.value));
            break;

        case 132:  // dbPointer
        case 133:  // javascript
        case 134:  // symbol
        case 135:  // javascriptWScope
        case 136:  // int
        case 137:  // timestamp
        case 138:  // long
        case 139:  // double
        case 140:  // decimal
        case 141:  // minKey
        case 142:  // maxKey
        case 143:  // value
        case 144:  // string
        case 145:  // binary
        case 146:  // undefined
        case 147:  // objectId
        case 148:  // bool
        case 149:  // date
        case 150:  // null
        case 151:  // regex
        case 152:  // simpleValue
        case 153:  // compoundValue
        case 154:  // valueArray
        case 155:  // valueObject
        case 156:  // valueFields
        case 157:  // stageList
        case 158:  // stage
        case 159:  // inhibitOptimization
        case 160:  // unionWith
        case 161:  // skip
        case 162:  // limit
        case 163:  // project
        case 164:  // sample
        case 165:  // projectFields
        case 166:  // projection
        case 167:  // num
        case 168:  // expression
        case 169:  // compoundExpression
        case 170:  // exprFixedTwoArg
        case 171:  // expressionArray
        case 172:  // expressionObject
        case 173:  // expressionFields
        case 174:  // maths
        case 175:  // add
        case 176:  // atan2
        case 177:  // boolExps
        case 178:  // and
        case 179:  // or
        case 180:  // not
        case 181:  // literalEscapes
        case 182:  // const
        case 183:  // literal
        case 184:  // stringExps
        case 185:  // concat
        case 186:  // dateFromString
        case 187:  // dateToString
        case 188:  // indexOfBytes
        case 189:  // indexOfCP
        case 190:  // ltrim
        case 191:  // regexFind
        case 192:  // regexFindAll
        case 193:  // regexMatch
        case 194:  // regexArgs
        case 195:  // replaceOne
        case 196:  // replaceAll
        case 197:  // rtrim
        case 198:  // split
        case 199:  // strLenBytes
        case 200:  // strLenCP
        case 201:  // strcasecmp
        case 202:  // substr
        case 203:  // substrBytes
        case 204:  // substrCP
        case 205:  // toLower
        case 206:  // toUpper
        case 207:  // trim
        case 208:  // compExprs
        case 209:  // cmp
        case 210:  // eq
        case 211:  // gt
        case 212:  // gte
        case 213:  // lt
        case 214:  // lte
        case 215:  // ne
        case 216:  // typeExpression
        case 217:  // convert
        case 218:  // toBool
        case 219:  // toDate
        case 220:  // toDecimal
        case 221:  // toDouble
        case 222:  // toInt
        case 223:  // toLong
        case 224:  // toObjectId
        case 225:  // toString
        case 226:  // type
        case 227:  // abs
        case 228:  // ceil
        case 229:  // divide
        case 230:  // exponent
        case 231:  // floor
        case 232:  // ln
        case 233:  // log
        case 234:  // logten
        case 235:  // mod
        case 236:  // multiply
        case 237:  // pow
        case 238:  // round
        case 239:  // sqrt
        case 240:  // subtract
        case 241:  // trunc
        case 251:  // matchExpression
        case 252:  // filterFields
        case 253:  // filterVal
            value.move<CNode>(YY_MOVE(that.value));
            break;

        case 119:  // projectionFieldname
        case 120:  // expressionFieldname
        case 121:  // stageAsUserFieldname
        case 122:  // filterFieldname
        case 123:  // argAsUserFieldname
        case 124:  // aggExprAsUserFieldname
        case 125:  // invariableUserFieldname
        case 126:  // idAsUserFieldname
        case 127:  // valueFieldname
            value.move<CNode::Fieldname>(YY_MOVE(that.value));
            break;

        case 102:  // "Date"
            value.move<Date_t>(YY_MOVE(that.value));
            break;

        case 112:  // "non-zero decimal"
            value.move<Decimal128>(YY_MOVE(that.value));
            break;

        case 101:  // "ObjectID"
            value.move<OID>(YY_MOVE(that.value));
            break;

        case 113:  // "Timestamp"
            value.move<Timestamp>(YY_MOVE(that.value));
            break;

        case 115:  // "maxKey"
            value.move<UserMaxKey>(YY_MOVE(that.value));
            break;

        case 114:  // "minKey"
            value.move<UserMinKey>(YY_MOVE(that.value));
            break;

        case 103:  // "null"
            value.move<UserNull>(YY_MOVE(that.value));
            break;

        case 100:  // "undefined"
            value.move<UserUndefined>(YY_MOVE(that.value));
            break;

        case 111:  // "non-zero double"
            value.move<double>(YY_MOVE(that.value));
            break;

        case 109:  // "non-zero integer"
            value.move<int>(YY_MOVE(that.value));
            break;

        case 110:  // "non-zero long"
            value.move<long long>(YY_MOVE(that.value));
            break;

        case 128:  // projectField
        case 129:  // expressionField
        case 130:  // valueField
        case 131:  // filterField
        case 242:  // onErrorArg
        case 243:  // onNullArg
        case 244:  // formatArg
        case 245:  // timezoneArg
        case 246:  // charsArg
        case 247:  // optionsArg
            value.move<std::pair<CNode::Fieldname, CNode>>(YY_MOVE(that.value));
            break;

        case 97:  // "fieldname"
        case 98:  // "string"
            value.move<std::string>(YY_MOVE(that.value));
            break;

        case 248:  // expressions
        case 249:  // values
        case 250:  // exprZeroToTwo
            value.move<std::vector<CNode>>(YY_MOVE(that.value));
            break;

        default:
            break;
    }

    // that is emptied.
    that.type = empty_symbol;
}

#if YY_CPLUSPLUS < 201103L
PipelineParserGen::stack_symbol_type& PipelineParserGen::stack_symbol_type::operator=(
    const stack_symbol_type& that) {
    state = that.state;
    switch (that.type_get()) {
        case 99:  // "BinData"
            value.copy<BSONBinData>(that.value);
            break;

        case 106:  // "Code"
            value.copy<BSONCode>(that.value);
            break;

        case 108:  // "CodeWScope"
            value.copy<BSONCodeWScope>(that.value);
            break;

        case 105:  // "dbPointer"
            value.copy<BSONDBRef>(that.value);
            break;

        case 104:  // "regex"
            value.copy<BSONRegEx>(that.value);
            break;

        case 107:  // "Symbol"
            value.copy<BSONSymbol>(that.value);
            break;

        case 132:  // dbPointer
        case 133:  // javascript
        case 134:  // symbol
        case 135:  // javascriptWScope
        case 136:  // int
        case 137:  // timestamp
        case 138:  // long
        case 139:  // double
        case 140:  // decimal
        case 141:  // minKey
        case 142:  // maxKey
        case 143:  // value
        case 144:  // string
        case 145:  // binary
        case 146:  // undefined
        case 147:  // objectId
        case 148:  // bool
        case 149:  // date
        case 150:  // null
        case 151:  // regex
        case 152:  // simpleValue
        case 153:  // compoundValue
        case 154:  // valueArray
        case 155:  // valueObject
        case 156:  // valueFields
        case 157:  // stageList
        case 158:  // stage
        case 159:  // inhibitOptimization
        case 160:  // unionWith
        case 161:  // skip
        case 162:  // limit
        case 163:  // project
        case 164:  // sample
        case 165:  // projectFields
        case 166:  // projection
        case 167:  // num
        case 168:  // expression
        case 169:  // compoundExpression
        case 170:  // exprFixedTwoArg
        case 171:  // expressionArray
        case 172:  // expressionObject
        case 173:  // expressionFields
        case 174:  // maths
        case 175:  // add
        case 176:  // atan2
        case 177:  // boolExps
        case 178:  // and
        case 179:  // or
        case 180:  // not
        case 181:  // literalEscapes
        case 182:  // const
        case 183:  // literal
        case 184:  // stringExps
        case 185:  // concat
        case 186:  // dateFromString
        case 187:  // dateToString
        case 188:  // indexOfBytes
        case 189:  // indexOfCP
        case 190:  // ltrim
        case 191:  // regexFind
        case 192:  // regexFindAll
        case 193:  // regexMatch
        case 194:  // regexArgs
        case 195:  // replaceOne
        case 196:  // replaceAll
        case 197:  // rtrim
        case 198:  // split
        case 199:  // strLenBytes
        case 200:  // strLenCP
        case 201:  // strcasecmp
        case 202:  // substr
        case 203:  // substrBytes
        case 204:  // substrCP
        case 205:  // toLower
        case 206:  // toUpper
        case 207:  // trim
        case 208:  // compExprs
        case 209:  // cmp
        case 210:  // eq
        case 211:  // gt
        case 212:  // gte
        case 213:  // lt
        case 214:  // lte
        case 215:  // ne
        case 216:  // typeExpression
        case 217:  // convert
        case 218:  // toBool
        case 219:  // toDate
        case 220:  // toDecimal
        case 221:  // toDouble
        case 222:  // toInt
        case 223:  // toLong
        case 224:  // toObjectId
        case 225:  // toString
        case 226:  // type
        case 227:  // abs
        case 228:  // ceil
        case 229:  // divide
        case 230:  // exponent
        case 231:  // floor
        case 232:  // ln
        case 233:  // log
        case 234:  // logten
        case 235:  // mod
        case 236:  // multiply
        case 237:  // pow
        case 238:  // round
        case 239:  // sqrt
        case 240:  // subtract
        case 241:  // trunc
        case 251:  // matchExpression
        case 252:  // filterFields
        case 253:  // filterVal
            value.copy<CNode>(that.value);
            break;

        case 119:  // projectionFieldname
        case 120:  // expressionFieldname
        case 121:  // stageAsUserFieldname
        case 122:  // filterFieldname
        case 123:  // argAsUserFieldname
        case 124:  // aggExprAsUserFieldname
        case 125:  // invariableUserFieldname
        case 126:  // idAsUserFieldname
        case 127:  // valueFieldname
            value.copy<CNode::Fieldname>(that.value);
            break;

        case 102:  // "Date"
            value.copy<Date_t>(that.value);
            break;

        case 112:  // "non-zero decimal"
            value.copy<Decimal128>(that.value);
            break;

        case 101:  // "ObjectID"
            value.copy<OID>(that.value);
            break;

        case 113:  // "Timestamp"
            value.copy<Timestamp>(that.value);
            break;

        case 115:  // "maxKey"
            value.copy<UserMaxKey>(that.value);
            break;

        case 114:  // "minKey"
            value.copy<UserMinKey>(that.value);
            break;

        case 103:  // "null"
            value.copy<UserNull>(that.value);
            break;

        case 100:  // "undefined"
            value.copy<UserUndefined>(that.value);
            break;

        case 111:  // "non-zero double"
            value.copy<double>(that.value);
            break;

        case 109:  // "non-zero integer"
            value.copy<int>(that.value);
            break;

        case 110:  // "non-zero long"
            value.copy<long long>(that.value);
            break;

        case 128:  // projectField
        case 129:  // expressionField
        case 130:  // valueField
        case 131:  // filterField
        case 242:  // onErrorArg
        case 243:  // onNullArg
        case 244:  // formatArg
        case 245:  // timezoneArg
        case 246:  // charsArg
        case 247:  // optionsArg
            value.copy<std::pair<CNode::Fieldname, CNode>>(that.value);
            break;

        case 97:  // "fieldname"
        case 98:  // "string"
            value.copy<std::string>(that.value);
            break;

        case 248:  // expressions
        case 249:  // values
        case 250:  // exprZeroToTwo
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
    switch (that.type_get()) {
        case 99:  // "BinData"
            value.move<BSONBinData>(that.value);
            break;

        case 106:  // "Code"
            value.move<BSONCode>(that.value);
            break;

        case 108:  // "CodeWScope"
            value.move<BSONCodeWScope>(that.value);
            break;

        case 105:  // "dbPointer"
            value.move<BSONDBRef>(that.value);
            break;

        case 104:  // "regex"
            value.move<BSONRegEx>(that.value);
            break;

        case 107:  // "Symbol"
            value.move<BSONSymbol>(that.value);
            break;

        case 132:  // dbPointer
        case 133:  // javascript
        case 134:  // symbol
        case 135:  // javascriptWScope
        case 136:  // int
        case 137:  // timestamp
        case 138:  // long
        case 139:  // double
        case 140:  // decimal
        case 141:  // minKey
        case 142:  // maxKey
        case 143:  // value
        case 144:  // string
        case 145:  // binary
        case 146:  // undefined
        case 147:  // objectId
        case 148:  // bool
        case 149:  // date
        case 150:  // null
        case 151:  // regex
        case 152:  // simpleValue
        case 153:  // compoundValue
        case 154:  // valueArray
        case 155:  // valueObject
        case 156:  // valueFields
        case 157:  // stageList
        case 158:  // stage
        case 159:  // inhibitOptimization
        case 160:  // unionWith
        case 161:  // skip
        case 162:  // limit
        case 163:  // project
        case 164:  // sample
        case 165:  // projectFields
        case 166:  // projection
        case 167:  // num
        case 168:  // expression
        case 169:  // compoundExpression
        case 170:  // exprFixedTwoArg
        case 171:  // expressionArray
        case 172:  // expressionObject
        case 173:  // expressionFields
        case 174:  // maths
        case 175:  // add
        case 176:  // atan2
        case 177:  // boolExps
        case 178:  // and
        case 179:  // or
        case 180:  // not
        case 181:  // literalEscapes
        case 182:  // const
        case 183:  // literal
        case 184:  // stringExps
        case 185:  // concat
        case 186:  // dateFromString
        case 187:  // dateToString
        case 188:  // indexOfBytes
        case 189:  // indexOfCP
        case 190:  // ltrim
        case 191:  // regexFind
        case 192:  // regexFindAll
        case 193:  // regexMatch
        case 194:  // regexArgs
        case 195:  // replaceOne
        case 196:  // replaceAll
        case 197:  // rtrim
        case 198:  // split
        case 199:  // strLenBytes
        case 200:  // strLenCP
        case 201:  // strcasecmp
        case 202:  // substr
        case 203:  // substrBytes
        case 204:  // substrCP
        case 205:  // toLower
        case 206:  // toUpper
        case 207:  // trim
        case 208:  // compExprs
        case 209:  // cmp
        case 210:  // eq
        case 211:  // gt
        case 212:  // gte
        case 213:  // lt
        case 214:  // lte
        case 215:  // ne
        case 216:  // typeExpression
        case 217:  // convert
        case 218:  // toBool
        case 219:  // toDate
        case 220:  // toDecimal
        case 221:  // toDouble
        case 222:  // toInt
        case 223:  // toLong
        case 224:  // toObjectId
        case 225:  // toString
        case 226:  // type
        case 227:  // abs
        case 228:  // ceil
        case 229:  // divide
        case 230:  // exponent
        case 231:  // floor
        case 232:  // ln
        case 233:  // log
        case 234:  // logten
        case 235:  // mod
        case 236:  // multiply
        case 237:  // pow
        case 238:  // round
        case 239:  // sqrt
        case 240:  // subtract
        case 241:  // trunc
        case 251:  // matchExpression
        case 252:  // filterFields
        case 253:  // filterVal
            value.move<CNode>(that.value);
            break;

        case 119:  // projectionFieldname
        case 120:  // expressionFieldname
        case 121:  // stageAsUserFieldname
        case 122:  // filterFieldname
        case 123:  // argAsUserFieldname
        case 124:  // aggExprAsUserFieldname
        case 125:  // invariableUserFieldname
        case 126:  // idAsUserFieldname
        case 127:  // valueFieldname
            value.move<CNode::Fieldname>(that.value);
            break;

        case 102:  // "Date"
            value.move<Date_t>(that.value);
            break;

        case 112:  // "non-zero decimal"
            value.move<Decimal128>(that.value);
            break;

        case 101:  // "ObjectID"
            value.move<OID>(that.value);
            break;

        case 113:  // "Timestamp"
            value.move<Timestamp>(that.value);
            break;

        case 115:  // "maxKey"
            value.move<UserMaxKey>(that.value);
            break;

        case 114:  // "minKey"
            value.move<UserMinKey>(that.value);
            break;

        case 103:  // "null"
            value.move<UserNull>(that.value);
            break;

        case 100:  // "undefined"
            value.move<UserUndefined>(that.value);
            break;

        case 111:  // "non-zero double"
            value.move<double>(that.value);
            break;

        case 109:  // "non-zero integer"
            value.move<int>(that.value);
            break;

        case 110:  // "non-zero long"
            value.move<long long>(that.value);
            break;

        case 128:  // projectField
        case 129:  // expressionField
        case 130:  // valueField
        case 131:  // filterField
        case 242:  // onErrorArg
        case 243:  // onNullArg
        case 244:  // formatArg
        case 245:  // timezoneArg
        case 246:  // charsArg
        case 247:  // optionsArg
            value.move<std::pair<CNode::Fieldname, CNode>>(that.value);
            break;

        case 97:  // "fieldname"
        case 98:  // "string"
            value.move<std::string>(that.value);
            break;

        case 248:  // expressions
        case 249:  // values
        case 250:  // exprZeroToTwo
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
    symbol_number_type yytype = yysym.type_get();
#if defined __GNUC__ && !defined __clang__ && !defined __ICC && \
    __GNUC__ * 100 + __GNUC_MINOR__ <= 408
    // Avoid a (spurious) G++ 4.8 warning about "array subscript is
    // below array bounds".
    if (yysym.empty())
        std::abort();
#endif
    yyo << (yytype < yyntokens_ ? "token" : "nterm") << ' ' << yytname_[yytype] << " ("
        << yysym.location << ": ";
    YYUSE(yytype);
    yyo << ')';
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
    int yyr = yypgoto_[yysym - yyntokens_] + yystate;
    if (0 <= yyr && yyr <= yylast_ && yycheck_[yyr] == yystate)
        return yytable_[yyr];
    else
        return yydefgoto_[yysym - yyntokens_];
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
            YYCDEBUG << "Reading a token: ";
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

        /* If the proper action on seeing token YYLA.TYPE is to reduce or
           to detect an error, take that action.  */
        yyn += yyla.type_get();
        if (yyn < 0 || yylast_ < yyn || yycheck_[yyn] != yyla.type_get()) {
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
                case 99:  // "BinData"
                    yylhs.value.emplace<BSONBinData>();
                    break;

                case 106:  // "Code"
                    yylhs.value.emplace<BSONCode>();
                    break;

                case 108:  // "CodeWScope"
                    yylhs.value.emplace<BSONCodeWScope>();
                    break;

                case 105:  // "dbPointer"
                    yylhs.value.emplace<BSONDBRef>();
                    break;

                case 104:  // "regex"
                    yylhs.value.emplace<BSONRegEx>();
                    break;

                case 107:  // "Symbol"
                    yylhs.value.emplace<BSONSymbol>();
                    break;

                case 132:  // dbPointer
                case 133:  // javascript
                case 134:  // symbol
                case 135:  // javascriptWScope
                case 136:  // int
                case 137:  // timestamp
                case 138:  // long
                case 139:  // double
                case 140:  // decimal
                case 141:  // minKey
                case 142:  // maxKey
                case 143:  // value
                case 144:  // string
                case 145:  // binary
                case 146:  // undefined
                case 147:  // objectId
                case 148:  // bool
                case 149:  // date
                case 150:  // null
                case 151:  // regex
                case 152:  // simpleValue
                case 153:  // compoundValue
                case 154:  // valueArray
                case 155:  // valueObject
                case 156:  // valueFields
                case 157:  // stageList
                case 158:  // stage
                case 159:  // inhibitOptimization
                case 160:  // unionWith
                case 161:  // skip
                case 162:  // limit
                case 163:  // project
                case 164:  // sample
                case 165:  // projectFields
                case 166:  // projection
                case 167:  // num
                case 168:  // expression
                case 169:  // compoundExpression
                case 170:  // exprFixedTwoArg
                case 171:  // expressionArray
                case 172:  // expressionObject
                case 173:  // expressionFields
                case 174:  // maths
                case 175:  // add
                case 176:  // atan2
                case 177:  // boolExps
                case 178:  // and
                case 179:  // or
                case 180:  // not
                case 181:  // literalEscapes
                case 182:  // const
                case 183:  // literal
                case 184:  // stringExps
                case 185:  // concat
                case 186:  // dateFromString
                case 187:  // dateToString
                case 188:  // indexOfBytes
                case 189:  // indexOfCP
                case 190:  // ltrim
                case 191:  // regexFind
                case 192:  // regexFindAll
                case 193:  // regexMatch
                case 194:  // regexArgs
                case 195:  // replaceOne
                case 196:  // replaceAll
                case 197:  // rtrim
                case 198:  // split
                case 199:  // strLenBytes
                case 200:  // strLenCP
                case 201:  // strcasecmp
                case 202:  // substr
                case 203:  // substrBytes
                case 204:  // substrCP
                case 205:  // toLower
                case 206:  // toUpper
                case 207:  // trim
                case 208:  // compExprs
                case 209:  // cmp
                case 210:  // eq
                case 211:  // gt
                case 212:  // gte
                case 213:  // lt
                case 214:  // lte
                case 215:  // ne
                case 216:  // typeExpression
                case 217:  // convert
                case 218:  // toBool
                case 219:  // toDate
                case 220:  // toDecimal
                case 221:  // toDouble
                case 222:  // toInt
                case 223:  // toLong
                case 224:  // toObjectId
                case 225:  // toString
                case 226:  // type
                case 227:  // abs
                case 228:  // ceil
                case 229:  // divide
                case 230:  // exponent
                case 231:  // floor
                case 232:  // ln
                case 233:  // log
                case 234:  // logten
                case 235:  // mod
                case 236:  // multiply
                case 237:  // pow
                case 238:  // round
                case 239:  // sqrt
                case 240:  // subtract
                case 241:  // trunc
                case 251:  // matchExpression
                case 252:  // filterFields
                case 253:  // filterVal
                    yylhs.value.emplace<CNode>();
                    break;

                case 119:  // projectionFieldname
                case 120:  // expressionFieldname
                case 121:  // stageAsUserFieldname
                case 122:  // filterFieldname
                case 123:  // argAsUserFieldname
                case 124:  // aggExprAsUserFieldname
                case 125:  // invariableUserFieldname
                case 126:  // idAsUserFieldname
                case 127:  // valueFieldname
                    yylhs.value.emplace<CNode::Fieldname>();
                    break;

                case 102:  // "Date"
                    yylhs.value.emplace<Date_t>();
                    break;

                case 112:  // "non-zero decimal"
                    yylhs.value.emplace<Decimal128>();
                    break;

                case 101:  // "ObjectID"
                    yylhs.value.emplace<OID>();
                    break;

                case 113:  // "Timestamp"
                    yylhs.value.emplace<Timestamp>();
                    break;

                case 115:  // "maxKey"
                    yylhs.value.emplace<UserMaxKey>();
                    break;

                case 114:  // "minKey"
                    yylhs.value.emplace<UserMinKey>();
                    break;

                case 103:  // "null"
                    yylhs.value.emplace<UserNull>();
                    break;

                case 100:  // "undefined"
                    yylhs.value.emplace<UserUndefined>();
                    break;

                case 111:  // "non-zero double"
                    yylhs.value.emplace<double>();
                    break;

                case 109:  // "non-zero integer"
                    yylhs.value.emplace<int>();
                    break;

                case 110:  // "non-zero long"
                    yylhs.value.emplace<long long>();
                    break;

                case 128:  // projectField
                case 129:  // expressionField
                case 130:  // valueField
                case 131:  // filterField
                case 242:  // onErrorArg
                case 243:  // onNullArg
                case 244:  // formatArg
                case 245:  // timezoneArg
                case 246:  // charsArg
                case 247:  // optionsArg
                    yylhs.value.emplace<std::pair<CNode::Fieldname, CNode>>();
                    break;

                case 97:  // "fieldname"
                case 98:  // "string"
                    yylhs.value.emplace<std::string>();
                    break;

                case 248:  // expressions
                case 249:  // values
                case 250:  // exprZeroToTwo
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
#line 279 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        *cst = CNode{YY_MOVE(yystack_[0].value.as<CNode>())};
                    }
#line 1688 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 4:
#line 286 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        *cst = YY_MOVE(yystack_[1].value.as<CNode>());
                    }
#line 1696 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 5:
#line 292 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                    }
#line 1702 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 6:
#line 293 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() =
                            CNode{CNode::ArrayChildren{YY_MOVE(yystack_[2].value.as<CNode>())}};
                    }
#line 1710 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 7:
#line 301 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        lexer.sortObjTokens();
                    }
#line 1716 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 9:
#line 304 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 1722 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 10:
#line 304 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 1728 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 11:
#line 304 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 1734 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 12:
#line 304 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 1740 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 13:
#line 304 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 1746 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 14:
#line 304 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 1752 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 15:
#line 307 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{std::pair{
                            KeyFieldname::sample,
                            CNode{CNode::ObjectChildren{
                                {KeyFieldname::sizeArg, YY_MOVE(yystack_[1].value.as<CNode>())},
                            }}}}};
                    }
#line 1764 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 16:
#line 317 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            std::pair{KeyFieldname::inhibitOptimization, CNode::noopLeaf()}}};
                    }
#line 1772 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 17:
#line 323 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        auto pipeline = YY_MOVE(yystack_[1].value.as<CNode>());
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{std::pair{
                            KeyFieldname::unionWith,
                            CNode{CNode::ObjectChildren{
                                {KeyFieldname::collArg, YY_MOVE(yystack_[3].value.as<CNode>())},
                                {KeyFieldname::pipelineArg, std::move(pipeline)}}}}}};
                    }
#line 1785 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 18:
#line 333 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 1791 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 19:
#line 333 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 1797 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 20:
#line 333 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 1803 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 21:
#line 333 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 1809 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 22:
#line 337 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            std::pair{KeyFieldname::skip, YY_MOVE(yystack_[0].value.as<CNode>())}}};
                    }
#line 1817 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 23:
#line 342 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{std::pair{
                            KeyFieldname::limit, YY_MOVE(yystack_[0].value.as<CNode>())}}};
                    }
#line 1825 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 24:
#line 347 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        auto&& fields = YY_MOVE(yystack_[1].value.as<CNode>());
                        if (auto inclusion =
                                c_node_validation::validateProjectionAsInclusionOrExclusion(fields);
                            inclusion.isOK())
                            yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{std::pair{
                                inclusion.getValue() == c_node_validation::IsInclusion::yes
                                    ? KeyFieldname::projectInclusion
                                    : KeyFieldname::projectExclusion,
                                std::move(fields)}}};
                        else
                            error(yystack_[3].location, inclusion.getStatus().reason());
                    }
#line 1842 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 25:
#line 362 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode::noopLeaf();
                    }
#line 1850 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 26:
#line 365 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[1].value.as<CNode>());
                        yylhs.value.as<CNode>().objectChildren().emplace_back(
                            YY_MOVE(yystack_[0].value.as<std::pair<CNode::Fieldname, CNode>>()));
                    }
#line 1859 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 27:
#line 372 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<std::pair<CNode::Fieldname, CNode>>() = {
                            KeyFieldname::id, YY_MOVE(yystack_[0].value.as<CNode>())};
                    }
#line 1867 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 28:
#line 375 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<std::pair<CNode::Fieldname, CNode>>() = {
                            YY_MOVE(yystack_[1].value.as<CNode::Fieldname>()),
                            YY_MOVE(yystack_[0].value.as<CNode>())};
                    }
#line 1875 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 29:
#line 381 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 1881 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 30:
#line 382 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 1887 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 31:
#line 383 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 1893 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 32:
#line 384 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 1899 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 33:
#line 385 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 1905 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 34:
#line 386 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 1911 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 35:
#line 387 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 1917 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 36:
#line 388 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 1923 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 37:
#line 389 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 1929 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 38:
#line 390 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 1935 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 39:
#line 391 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 1941 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 40:
#line 392 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() =
                            CNode{NonZeroKey{YY_MOVE(yystack_[0].value.as<int>())}};
                    }
#line 1949 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 41:
#line 395 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{KeyValue::intZeroKey};
                    }
#line 1957 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 42:
#line 398 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() =
                            CNode{NonZeroKey{YY_MOVE(yystack_[0].value.as<long long>())}};
                    }
#line 1965 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 43:
#line 401 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{KeyValue::longZeroKey};
                    }
#line 1973 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 44:
#line 404 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() =
                            CNode{NonZeroKey{YY_MOVE(yystack_[0].value.as<double>())}};
                    }
#line 1981 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 45:
#line 407 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{KeyValue::doubleZeroKey};
                    }
#line 1989 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 46:
#line 410 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() =
                            CNode{NonZeroKey{YY_MOVE(yystack_[0].value.as<Decimal128>())}};
                    }
#line 1997 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 47:
#line 413 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{KeyValue::decimalZeroKey};
                    }
#line 2005 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 48:
#line 416 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{KeyValue::trueKey};
                    }
#line 2013 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 49:
#line 419 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{KeyValue::falseKey};
                    }
#line 2021 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 50:
#line 422 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 2027 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 51:
#line 423 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 2033 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 52:
#line 424 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 2039 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 53:
#line 425 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 2045 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 54:
#line 429 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() =
                            YY_MOVE(yystack_[0].value.as<CNode::Fieldname>());
                    }
#line 2051 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 55:
#line 429 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() =
                            YY_MOVE(yystack_[0].value.as<CNode::Fieldname>());
                    }
#line 2057 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 56:
#line 429 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() =
                            YY_MOVE(yystack_[0].value.as<CNode::Fieldname>());
                    }
#line 2063 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 57:
#line 429 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() =
                            YY_MOVE(yystack_[0].value.as<CNode::Fieldname>());
                    }
#line 2069 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 58:
#line 433 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{std::pair{
                            KeyFieldname::match, YY_MOVE(yystack_[1].value.as<CNode>())}}};
                    }
#line 2077 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 59:
#line 439 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode::noopLeaf();
                    }
#line 2085 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 60:
#line 442 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[1].value.as<CNode>());
                        yylhs.value.as<CNode>().objectChildren().emplace_back(
                            YY_MOVE(yystack_[0].value.as<std::pair<CNode::Fieldname, CNode>>()));
                    }
#line 2094 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 61:
#line 449 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<std::pair<CNode::Fieldname, CNode>>() = {
                            KeyFieldname::id, YY_MOVE(yystack_[0].value.as<CNode>())};
                    }
#line 2102 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 62:
#line 452 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<std::pair<CNode::Fieldname, CNode>>() = {
                            YY_MOVE(yystack_[1].value.as<CNode::Fieldname>()),
                            YY_MOVE(yystack_[0].value.as<CNode>())};
                    }
#line 2110 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 63:
#line 458 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 2116 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 64:
#line 462 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() =
                            YY_MOVE(yystack_[0].value.as<CNode::Fieldname>());
                    }
#line 2122 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 65:
#line 462 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() =
                            YY_MOVE(yystack_[0].value.as<CNode::Fieldname>());
                    }
#line 2128 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 66:
#line 462 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() =
                            YY_MOVE(yystack_[0].value.as<CNode::Fieldname>());
                    }
#line 2134 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 67:
#line 462 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() =
                            YY_MOVE(yystack_[0].value.as<CNode::Fieldname>());
                    }
#line 2140 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 68:
#line 466 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() =
                            UserFieldname{YY_MOVE(yystack_[0].value.as<std::string>())};
                    }
#line 2148 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 69:
#line 474 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() =
                            UserFieldname{"$_internalInhibitOptimization"};
                    }
#line 2156 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 70:
#line 477 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$unionWith"};
                    }
#line 2164 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 71:
#line 480 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$skip"};
                    }
#line 2172 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 72:
#line 483 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$limit"};
                    }
#line 2180 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 73:
#line 486 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$project"};
                    }
#line 2188 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 74:
#line 489 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$sample"};
                    }
#line 2196 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 75:
#line 498 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"coll"};
                    }
#line 2204 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 76:
#line 501 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"pipeline"};
                    }
#line 2212 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 77:
#line 504 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"size"};
                    }
#line 2220 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 78:
#line 507 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"input"};
                    }
#line 2228 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 79:
#line 510 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"to"};
                    }
#line 2236 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 80:
#line 513 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"onError"};
                    }
#line 2244 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 81:
#line 516 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"onNull"};
                    }
#line 2252 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 82:
#line 519 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"dateString"};
                    }
#line 2260 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 83:
#line 522 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"format"};
                    }
#line 2268 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 84:
#line 525 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"timezone"};
                    }
#line 2276 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 85:
#line 528 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"date"};
                    }
#line 2284 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 86:
#line 531 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"chars"};
                    }
#line 2292 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 87:
#line 534 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"regex"};
                    }
#line 2300 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 88:
#line 537 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"options"};
                    }
#line 2308 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 89:
#line 540 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"find"};
                    }
#line 2316 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 90:
#line 543 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"replacement"};
                    }
#line 2324 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 91:
#line 551 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$add"};
                    }
#line 2332 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 92:
#line 554 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$atan2"};
                    }
#line 2340 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 93:
#line 557 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$and"};
                    }
#line 2348 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 94:
#line 560 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$const"};
                    }
#line 2356 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 95:
#line 563 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$literal"};
                    }
#line 2364 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 96:
#line 566 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$or"};
                    }
#line 2372 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 97:
#line 569 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$not"};
                    }
#line 2380 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 98:
#line 572 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$cmp"};
                    }
#line 2388 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 99:
#line 575 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$eq"};
                    }
#line 2396 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 100:
#line 578 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$gt"};
                    }
#line 2404 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 101:
#line 581 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$gte"};
                    }
#line 2412 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 102:
#line 584 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$lt"};
                    }
#line 2420 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 103:
#line 587 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$lte"};
                    }
#line 2428 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 104:
#line 590 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$ne"};
                    }
#line 2436 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 105:
#line 593 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$convert"};
                    }
#line 2444 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 106:
#line 596 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$toBool"};
                    }
#line 2452 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 107:
#line 599 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$toDate"};
                    }
#line 2460 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 108:
#line 602 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$toDecimal"};
                    }
#line 2468 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 109:
#line 605 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$toDouble"};
                    }
#line 2476 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 110:
#line 608 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$toInt"};
                    }
#line 2484 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 111:
#line 611 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$toLong"};
                    }
#line 2492 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 112:
#line 614 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$toObjectId"};
                    }
#line 2500 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 113:
#line 617 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$toString"};
                    }
#line 2508 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 114:
#line 620 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$type"};
                    }
#line 2516 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 115:
#line 623 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$abs"};
                    }
#line 2524 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 116:
#line 626 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$ceil"};
                    }
#line 2532 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 117:
#line 629 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$divide"};
                    }
#line 2540 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 118:
#line 632 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$exp"};
                    }
#line 2548 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 119:
#line 635 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$floor"};
                    }
#line 2556 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 120:
#line 638 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$ln"};
                    }
#line 2564 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 121:
#line 641 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$log"};
                    }
#line 2572 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 122:
#line 644 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$log10"};
                    }
#line 2580 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 123:
#line 647 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$mod"};
                    }
#line 2588 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 124:
#line 650 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$multiply"};
                    }
#line 2596 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 125:
#line 653 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$pow"};
                    }
#line 2604 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 126:
#line 656 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$round"};
                    }
#line 2612 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 127:
#line 659 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$sqrt"};
                    }
#line 2620 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 128:
#line 662 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$subtract"};
                    }
#line 2628 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 129:
#line 665 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$trunc"};
                    }
#line 2636 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 130:
#line 668 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$concat"};
                    }
#line 2644 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 131:
#line 671 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$dateFromString"};
                    }
#line 2652 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 132:
#line 674 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$dateToString"};
                    }
#line 2660 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 133:
#line 677 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$indexOfBytes"};
                    }
#line 2668 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 134:
#line 680 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$indexOfCP"};
                    }
#line 2676 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 135:
#line 683 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$ltrim"};
                    }
#line 2684 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 136:
#line 686 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$regexFind"};
                    }
#line 2692 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 137:
#line 689 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$regexFindAll"};
                    }
#line 2700 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 138:
#line 692 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$regexMatch"};
                    }
#line 2708 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 139:
#line 695 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$replaceOne"};
                    }
#line 2716 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 140:
#line 698 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$replaceAll"};
                    }
#line 2724 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 141:
#line 701 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$rtrim"};
                    }
#line 2732 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 142:
#line 704 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$split"};
                    }
#line 2740 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 143:
#line 707 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$strLenBytes"};
                    }
#line 2748 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 144:
#line 710 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$strLenCP"};
                    }
#line 2756 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 145:
#line 713 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$strcasecmp"};
                    }
#line 2764 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 146:
#line 716 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$substr"};
                    }
#line 2772 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 147:
#line 719 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$substrBytes"};
                    }
#line 2780 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 148:
#line 722 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$substrCP"};
                    }
#line 2788 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 149:
#line 725 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$toLower"};
                    }
#line 2796 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 150:
#line 728 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$trim"};
                    }
#line 2804 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 151:
#line 731 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$toUpper"};
                    }
#line 2812 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 152:
#line 738 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() =
                            CNode{UserString{YY_MOVE(yystack_[0].value.as<std::string>())}};
                    }
#line 2820 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 153:
#line 744 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() =
                            CNode{UserBinary{YY_MOVE(yystack_[0].value.as<BSONBinData>())}};
                    }
#line 2828 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 154:
#line 750 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{UserUndefined{}};
                    }
#line 2836 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 155:
#line 756 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{UserObjectId{}};
                    }
#line 2844 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 156:
#line 762 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() =
                            CNode{UserDate{YY_MOVE(yystack_[0].value.as<Date_t>())}};
                    }
#line 2852 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 157:
#line 768 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{UserNull{}};
                    }
#line 2860 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 158:
#line 774 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() =
                            CNode{UserRegex{YY_MOVE(yystack_[0].value.as<BSONRegEx>())}};
                    }
#line 2868 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 159:
#line 780 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() =
                            CNode{UserDBPointer{YY_MOVE(yystack_[0].value.as<BSONDBRef>())}};
                    }
#line 2876 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 160:
#line 786 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() =
                            CNode{UserJavascript{YY_MOVE(yystack_[0].value.as<BSONCode>())}};
                    }
#line 2884 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 161:
#line 792 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() =
                            CNode{UserSymbol{YY_MOVE(yystack_[0].value.as<BSONSymbol>())}};
                    }
#line 2892 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 162:
#line 798 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{UserJavascriptWithScope{
                            YY_MOVE(yystack_[0].value.as<BSONCodeWScope>())}};
                    }
#line 2900 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 163:
#line 804 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() =
                            CNode{UserTimestamp{YY_MOVE(yystack_[0].value.as<Timestamp>())}};
                    }
#line 2908 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 164:
#line 810 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() =
                            CNode{UserMinKey{YY_MOVE(yystack_[0].value.as<UserMinKey>())}};
                    }
#line 2916 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 165:
#line 816 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() =
                            CNode{UserMaxKey{YY_MOVE(yystack_[0].value.as<UserMaxKey>())}};
                    }
#line 2924 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 166:
#line 822 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() =
                            CNode{UserInt{YY_MOVE(yystack_[0].value.as<int>())}};
                    }
#line 2932 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 167:
#line 825 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{UserInt{0}};
                    }
#line 2940 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 168:
#line 831 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() =
                            CNode{UserLong{YY_MOVE(yystack_[0].value.as<long long>())}};
                    }
#line 2948 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 169:
#line 834 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{UserLong{0ll}};
                    }
#line 2956 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 170:
#line 840 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() =
                            CNode{UserDouble{YY_MOVE(yystack_[0].value.as<double>())}};
                    }
#line 2964 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 171:
#line 843 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{UserDouble{0.0}};
                    }
#line 2972 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 172:
#line 849 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() =
                            CNode{UserDecimal{YY_MOVE(yystack_[0].value.as<Decimal128>())}};
                    }
#line 2980 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 173:
#line 852 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{UserDecimal{0.0}};
                    }
#line 2988 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 174:
#line 858 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{UserBoolean{true}};
                    }
#line 2996 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 175:
#line 861 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{UserBoolean{false}};
                    }
#line 3004 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 176:
#line 867 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 3010 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 177:
#line 868 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 3016 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 178:
#line 869 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 3022 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 179:
#line 870 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 3028 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 180:
#line 871 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 3034 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 181:
#line 872 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 3040 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 182:
#line 873 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 3046 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 183:
#line 874 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 3052 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 184:
#line 875 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 3058 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 185:
#line 876 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 3064 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 186:
#line 877 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 3070 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 187:
#line 878 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 3076 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 188:
#line 879 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 3082 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 189:
#line 880 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 3088 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 190:
#line 881 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 3094 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 191:
#line 882 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 3100 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 192:
#line 883 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 3106 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 193:
#line 884 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 3112 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 194:
#line 885 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 3118 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 195:
#line 892 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                    }
#line 3124 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 196:
#line 893 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<std::vector<CNode>>() =
                            YY_MOVE(yystack_[0].value.as<std::vector<CNode>>());
                        yylhs.value.as<std::vector<CNode>>().emplace_back(
                            YY_MOVE(yystack_[1].value.as<CNode>()));
                    }
#line 3133 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 197:
#line 900 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 3139 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 198:
#line 900 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 3145 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 199:
#line 904 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() =
                            CNode{CNode::ArrayChildren{YY_MOVE(yystack_[2].value.as<CNode>()),
                                                       YY_MOVE(yystack_[1].value.as<CNode>())}};
                    }
#line 3153 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 200:
#line 909 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 3159 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 201:
#line 909 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 3165 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 202:
#line 909 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 3171 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 203:
#line 909 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 3177 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 204:
#line 909 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 3183 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 205:
#line 909 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 3189 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 206:
#line 910 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 3195 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 207:
#line 910 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 3201 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 208:
#line 916 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() =
                            CNode{YY_MOVE(yystack_[1].value.as<std::vector<CNode>>())};
                    }
#line 3209 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 209:
#line 924 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[1].value.as<CNode>());
                    }
#line 3217 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 210:
#line 930 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode::noopLeaf();
                    }
#line 3225 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 211:
#line 933 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[1].value.as<CNode>());
                        yylhs.value.as<CNode>().objectChildren().emplace_back(
                            YY_MOVE(yystack_[0].value.as<std::pair<CNode::Fieldname, CNode>>()));
                    }
#line 3234 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 212:
#line 940 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<std::pair<CNode::Fieldname, CNode>>() = {
                            YY_MOVE(yystack_[1].value.as<CNode::Fieldname>()),
                            YY_MOVE(yystack_[0].value.as<CNode>())};
                    }
#line 3242 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 213:
#line 947 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() =
                            YY_MOVE(yystack_[0].value.as<CNode::Fieldname>());
                    }
#line 3248 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 214:
#line 947 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() =
                            YY_MOVE(yystack_[0].value.as<CNode::Fieldname>());
                    }
#line 3254 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 215:
#line 947 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() =
                            YY_MOVE(yystack_[0].value.as<CNode::Fieldname>());
                    }
#line 3260 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 216:
#line 947 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() =
                            YY_MOVE(yystack_[0].value.as<CNode::Fieldname>());
                    }
#line 3266 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 217:
#line 951 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"_id"};
                    }
#line 3274 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 218:
#line 957 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 3280 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 219:
#line 957 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 3286 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 220:
#line 957 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 3292 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 221:
#line 957 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 3298 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 222:
#line 957 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 3304 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 223:
#line 957 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 3310 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 224:
#line 957 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 3316 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 225:
#line 957 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 3322 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 226:
#line 957 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 3328 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 227:
#line 957 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 3334 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 228:
#line 957 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 3340 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 229:
#line 957 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 3346 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 230:
#line 957 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 3352 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 231:
#line 958 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 3358 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 232:
#line 958 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 3364 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 233:
#line 958 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 3370 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 234:
#line 958 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 3376 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 235:
#line 962 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::add, YY_MOVE(yystack_[1].value.as<CNode>())}}};
                    }
#line 3385 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 236:
#line 969 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::atan2, YY_MOVE(yystack_[1].value.as<CNode>())}}};
                    }
#line 3394 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 237:
#line 975 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::abs, YY_MOVE(yystack_[1].value.as<CNode>())}}};
                    }
#line 3402 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 238:
#line 980 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::ceil, YY_MOVE(yystack_[1].value.as<CNode>())}}};
                    }
#line 3410 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 239:
#line 985 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::divide,
                             CNode{CNode::ArrayChildren{YY_MOVE(yystack_[3].value.as<CNode>()),
                                                        YY_MOVE(yystack_[2].value.as<CNode>())}}}}};
                    }
#line 3419 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 240:
#line 991 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::exponent, YY_MOVE(yystack_[1].value.as<CNode>())}}};
                    }
#line 3427 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 241:
#line 996 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::floor, YY_MOVE(yystack_[1].value.as<CNode>())}}};
                    }
#line 3435 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 242:
#line 1001 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::ln, YY_MOVE(yystack_[1].value.as<CNode>())}}};
                    }
#line 3443 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 243:
#line 1006 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::log,
                             CNode{CNode::ArrayChildren{YY_MOVE(yystack_[3].value.as<CNode>()),
                                                        YY_MOVE(yystack_[2].value.as<CNode>())}}}}};
                    }
#line 3452 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 244:
#line 1012 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::logten, YY_MOVE(yystack_[1].value.as<CNode>())}}};
                    }
#line 3460 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 245:
#line 1017 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::mod,
                             CNode{CNode::ArrayChildren{YY_MOVE(yystack_[3].value.as<CNode>()),
                                                        YY_MOVE(yystack_[2].value.as<CNode>())}}}}};
                    }
#line 3469 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 246:
#line 1023 "src/mongo/db/cst/pipeline_grammar.yy"
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
#line 3481 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 247:
#line 1032 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::pow,
                             CNode{CNode::ArrayChildren{YY_MOVE(yystack_[3].value.as<CNode>()),
                                                        YY_MOVE(yystack_[2].value.as<CNode>())}}}}};
                    }
#line 3490 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 248:
#line 1038 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::round,
                             CNode{CNode::ArrayChildren{YY_MOVE(yystack_[3].value.as<CNode>()),
                                                        YY_MOVE(yystack_[2].value.as<CNode>())}}}}};
                    }
#line 3499 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 249:
#line 1044 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::sqrt, YY_MOVE(yystack_[1].value.as<CNode>())}}};
                    }
#line 3507 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 250:
#line 1049 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::subtract,
                             CNode{CNode::ArrayChildren{YY_MOVE(yystack_[3].value.as<CNode>()),
                                                        YY_MOVE(yystack_[2].value.as<CNode>())}}}}};
                    }
#line 3516 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 251:
#line 1055 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::trunc,
                             CNode{CNode::ArrayChildren{YY_MOVE(yystack_[3].value.as<CNode>()),
                                                        YY_MOVE(yystack_[2].value.as<CNode>())}}}}};
                    }
#line 3525 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 252:
#line 1061 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 3531 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 253:
#line 1061 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 3537 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 254:
#line 1061 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 3543 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 255:
#line 1065 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::andExpr, YY_MOVE(yystack_[1].value.as<CNode>())}}};
                    }
#line 3552 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 256:
#line 1072 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::orExpr, YY_MOVE(yystack_[1].value.as<CNode>())}}};
                    }
#line 3561 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 257:
#line 1079 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::notExpr,
                             CNode{CNode::ArrayChildren{YY_MOVE(yystack_[2].value.as<CNode>())}}}}};
                    }
#line 3570 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 258:
#line 1086 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 3576 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 259:
#line 1086 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 3582 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 260:
#line 1086 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 3588 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 261:
#line 1086 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 3594 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 262:
#line 1086 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 3600 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 263:
#line 1086 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 3606 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 264:
#line 1086 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 3612 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 265:
#line 1087 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 3618 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 266:
#line 1087 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 3624 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 267:
#line 1087 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 3630 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 268:
#line 1087 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 3636 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 269:
#line 1087 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 3642 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 270:
#line 1087 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 3648 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 271:
#line 1087 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 3654 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 272:
#line 1087 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 3660 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 273:
#line 1088 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 3666 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 274:
#line 1088 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 3672 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 275:
#line 1088 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 3678 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 276:
#line 1088 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 3684 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 277:
#line 1088 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 3690 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 278:
#line 1088 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 3696 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 279:
#line 1088 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 3702 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 280:
#line 1092 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::concat, CNode{CNode::ArrayChildren{}}}}};
                        auto&& others = YY_MOVE(yystack_[2].value.as<std::vector<CNode>>());
                        auto&& array =
                            yylhs.value.as<CNode>().objectChildren()[0].second.arrayChildren();
                        array.insert(array.end(), others.begin(), others.end());
                    }
#line 3714 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 281:
#line 1102 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<std::pair<CNode::Fieldname, CNode>>() =
                            std::pair{KeyFieldname::formatArg, CNode{KeyValue::absentKey}};
                    }
#line 3722 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 282:
#line 1105 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<std::pair<CNode::Fieldname, CNode>>() = std::pair{
                            KeyFieldname::formatArg, YY_MOVE(yystack_[0].value.as<CNode>())};
                    }
#line 3730 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 283:
#line 1111 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<std::pair<CNode::Fieldname, CNode>>() =
                            std::pair{KeyFieldname::timezoneArg, CNode{KeyValue::absentKey}};
                    }
#line 3738 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 284:
#line 1114 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<std::pair<CNode::Fieldname, CNode>>() = std::pair{
                            KeyFieldname::timezoneArg, YY_MOVE(yystack_[0].value.as<CNode>())};
                    }
#line 3746 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 285:
#line 1121 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::dateFromString,
                             CNode{CNode::ObjectChildren{
                                 {KeyFieldname::dateStringArg,
                                  YY_MOVE(yystack_[6].value.as<CNode>())},
                                 YY_MOVE(
                                     yystack_[5].value.as<std::pair<CNode::Fieldname, CNode>>()),
                                 YY_MOVE(
                                     yystack_[4].value.as<std::pair<CNode::Fieldname, CNode>>()),
                                 YY_MOVE(
                                     yystack_[3].value.as<std::pair<CNode::Fieldname, CNode>>()),
                                 YY_MOVE(yystack_[2]
                                             .value.as<std::pair<CNode::Fieldname, CNode>>())}}}}};
                    }
#line 3756 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 286:
#line 1130 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::dateToString,
                             CNode{CNode::ObjectChildren{
                                 {KeyFieldname::dateArg, YY_MOVE(yystack_[5].value.as<CNode>())},
                                 YY_MOVE(
                                     yystack_[4].value.as<std::pair<CNode::Fieldname, CNode>>()),
                                 YY_MOVE(
                                     yystack_[3].value.as<std::pair<CNode::Fieldname, CNode>>()),
                                 YY_MOVE(yystack_[2]
                                             .value.as<std::pair<CNode::Fieldname, CNode>>())}}}}};
                    }
#line 3766 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 287:
#line 1138 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<std::vector<CNode>>() = CNode::ArrayChildren{};
                    }
#line 3774 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 288:
#line 1141 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<std::vector<CNode>>() =
                            CNode::ArrayChildren{YY_MOVE(yystack_[0].value.as<CNode>())};
                    }
#line 3782 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 289:
#line 1144 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<std::vector<CNode>>() =
                            CNode::ArrayChildren{YY_MOVE(yystack_[1].value.as<CNode>()),
                                                 YY_MOVE(yystack_[0].value.as<CNode>())};
                    }
#line 3790 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 290:
#line 1151 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::indexOfBytes,
                             CNode{CNode::ArrayChildren{YY_MOVE(yystack_[4].value.as<CNode>()),
                                                        YY_MOVE(yystack_[3].value.as<CNode>())}}}}};
                        auto&& others = YY_MOVE(yystack_[2].value.as<std::vector<CNode>>());
                        auto&& array =
                            yylhs.value.as<CNode>().objectChildren()[0].second.arrayChildren();
                        array.insert(array.end(), others.begin(), others.end());
                    }
#line 3802 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 291:
#line 1162 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::indexOfCP,
                             CNode{CNode::ArrayChildren{YY_MOVE(yystack_[4].value.as<CNode>()),
                                                        YY_MOVE(yystack_[3].value.as<CNode>())}}}}};
                        auto&& others = YY_MOVE(yystack_[2].value.as<std::vector<CNode>>());
                        auto&& array =
                            yylhs.value.as<CNode>().objectChildren()[0].second.arrayChildren();
                        array.insert(array.end(), others.begin(), others.end());
                    }
#line 3814 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 292:
#line 1172 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<std::pair<CNode::Fieldname, CNode>>() =
                            std::pair{KeyFieldname::charsArg, CNode{KeyValue::absentKey}};
                    }
#line 3822 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 293:
#line 1175 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<std::pair<CNode::Fieldname, CNode>>() = std::pair{
                            KeyFieldname::charsArg, YY_MOVE(yystack_[0].value.as<CNode>())};
                    }
#line 3830 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 294:
#line 1181 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::ltrim,
                             CNode{CNode::ObjectChildren{
                                 {KeyFieldname::inputArg, YY_MOVE(yystack_[2].value.as<CNode>())},
                                 YY_MOVE(yystack_[4]
                                             .value.as<std::pair<CNode::Fieldname, CNode>>())}}}}};
                    }
#line 3840 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 295:
#line 1189 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::rtrim,
                             CNode{CNode::ObjectChildren{
                                 {KeyFieldname::inputArg, YY_MOVE(yystack_[2].value.as<CNode>())},
                                 YY_MOVE(yystack_[4]
                                             .value.as<std::pair<CNode::Fieldname, CNode>>())}}}}};
                    }
#line 3850 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 296:
#line 1197 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::trim,
                             CNode{CNode::ObjectChildren{
                                 {KeyFieldname::inputArg, YY_MOVE(yystack_[2].value.as<CNode>())},
                                 YY_MOVE(yystack_[4]
                                             .value.as<std::pair<CNode::Fieldname, CNode>>())}}}}};
                    }
#line 3860 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 297:
#line 1205 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<std::pair<CNode::Fieldname, CNode>>() =
                            std::pair{KeyFieldname::optionsArg, CNode{KeyValue::absentKey}};
                    }
#line 3868 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 298:
#line 1208 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<std::pair<CNode::Fieldname, CNode>>() = std::pair{
                            KeyFieldname::optionsArg, YY_MOVE(yystack_[0].value.as<CNode>())};
                    }
#line 3876 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 299:
#line 1213 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        // Note that the order of these arguments must match the constructor for the
                        // regex expression.
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::inputArg, YY_MOVE(yystack_[4].value.as<CNode>())},
                            {KeyFieldname::regexArg, YY_MOVE(yystack_[1].value.as<CNode>())},
                            YY_MOVE(yystack_[3].value.as<std::pair<CNode::Fieldname, CNode>>())}};
                    }
#line 3888 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 300:
#line 1222 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::regexFind, YY_MOVE(yystack_[1].value.as<CNode>())}}};
                    }
#line 3896 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 301:
#line 1228 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::regexFindAll, YY_MOVE(yystack_[1].value.as<CNode>())}}};
                    }
#line 3904 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 302:
#line 1234 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::regexMatch, YY_MOVE(yystack_[1].value.as<CNode>())}}};
                    }
#line 3912 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 303:
#line 1241 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::replaceOne,
                             CNode{CNode::ObjectChildren{
                                 {KeyFieldname::inputArg, YY_MOVE(yystack_[4].value.as<CNode>())},
                                 {KeyFieldname::findArg, YY_MOVE(yystack_[6].value.as<CNode>())},
                                 {KeyFieldname::replacementArg,
                                  YY_MOVE(yystack_[2].value.as<CNode>())}}}}}};
                    }
#line 3923 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 304:
#line 1251 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::replaceAll,
                             CNode{CNode::ObjectChildren{
                                 {KeyFieldname::inputArg, YY_MOVE(yystack_[4].value.as<CNode>())},
                                 {KeyFieldname::findArg, YY_MOVE(yystack_[6].value.as<CNode>())},
                                 {KeyFieldname::replacementArg,
                                  YY_MOVE(yystack_[2].value.as<CNode>())}}}}}};
                    }
#line 3934 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 305:
#line 1260 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::split,
                             CNode{CNode::ArrayChildren{YY_MOVE(yystack_[3].value.as<CNode>()),
                                                        YY_MOVE(yystack_[2].value.as<CNode>())}}}}};
                    }
#line 3943 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 306:
#line 1267 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::strLenBytes, YY_MOVE(yystack_[1].value.as<CNode>())}}};
                    }
#line 3952 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 307:
#line 1274 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::strLenCP, YY_MOVE(yystack_[1].value.as<CNode>())}}};
                    }
#line 3961 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 308:
#line 1282 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::strcasecmp,
                             CNode{CNode::ArrayChildren{YY_MOVE(yystack_[3].value.as<CNode>()),
                                                        YY_MOVE(yystack_[2].value.as<CNode>())}}}}};
                    }
#line 3970 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 309:
#line 1290 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::substr,
                             CNode{CNode::ArrayChildren{YY_MOVE(yystack_[4].value.as<CNode>()),
                                                        YY_MOVE(yystack_[3].value.as<CNode>()),
                                                        YY_MOVE(yystack_[2].value.as<CNode>())}}}}};
                    }
#line 3979 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 310:
#line 1298 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::substrBytes,
                             CNode{CNode::ArrayChildren{YY_MOVE(yystack_[4].value.as<CNode>()),
                                                        YY_MOVE(yystack_[3].value.as<CNode>()),
                                                        YY_MOVE(yystack_[2].value.as<CNode>())}}}}};
                    }
#line 3988 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 311:
#line 1306 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::substrCP,
                             CNode{CNode::ArrayChildren{YY_MOVE(yystack_[4].value.as<CNode>()),
                                                        YY_MOVE(yystack_[3].value.as<CNode>()),
                                                        YY_MOVE(yystack_[2].value.as<CNode>())}}}}};
                    }
#line 3997 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 312:
#line 1313 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::toLower, YY_MOVE(yystack_[1].value.as<CNode>())}}};
                    }
#line 4005 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 313:
#line 1319 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::toUpper, YY_MOVE(yystack_[1].value.as<CNode>())}}};
                    }
#line 4013 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 314:
#line 1325 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 4019 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 315:
#line 1325 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 4025 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 316:
#line 1329 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::constExpr,
                             CNode{CNode::ArrayChildren{YY_MOVE(yystack_[2].value.as<CNode>())}}}}};
                    }
#line 4034 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 317:
#line 1336 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::literal,
                             CNode{CNode::ArrayChildren{YY_MOVE(yystack_[2].value.as<CNode>())}}}}};
                    }
#line 4043 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 318:
#line 1343 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 4049 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 319:
#line 1343 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 4055 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 320:
#line 1347 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 4061 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 321:
#line 1347 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 4067 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 322:
#line 1351 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() =
                            CNode{YY_MOVE(yystack_[1].value.as<std::vector<CNode>>())};
                    }
#line 4075 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 323:
#line 1357 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                    }
#line 4081 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 324:
#line 1358 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<std::vector<CNode>>() =
                            YY_MOVE(yystack_[0].value.as<std::vector<CNode>>());
                        yylhs.value.as<std::vector<CNode>>().emplace_back(
                            YY_MOVE(yystack_[1].value.as<CNode>()));
                    }
#line 4090 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 325:
#line 1365 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[1].value.as<CNode>());
                    }
#line 4098 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 326:
#line 1371 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode::noopLeaf();
                    }
#line 4106 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 327:
#line 1374 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[1].value.as<CNode>());
                        yylhs.value.as<CNode>().objectChildren().emplace_back(
                            YY_MOVE(yystack_[0].value.as<std::pair<CNode::Fieldname, CNode>>()));
                    }
#line 4115 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 328:
#line 1381 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<std::pair<CNode::Fieldname, CNode>>() = {
                            YY_MOVE(yystack_[1].value.as<CNode::Fieldname>()),
                            YY_MOVE(yystack_[0].value.as<CNode>())};
                    }
#line 4123 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 329:
#line 1388 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() =
                            YY_MOVE(yystack_[0].value.as<CNode::Fieldname>());
                    }
#line 4129 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 330:
#line 1389 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() =
                            YY_MOVE(yystack_[0].value.as<CNode::Fieldname>());
                    }
#line 4135 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 331:
#line 1390 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() =
                            YY_MOVE(yystack_[0].value.as<CNode::Fieldname>());
                    }
#line 4141 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 332:
#line 1391 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() =
                            YY_MOVE(yystack_[0].value.as<CNode::Fieldname>());
                    }
#line 4147 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 333:
#line 1392 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() =
                            YY_MOVE(yystack_[0].value.as<CNode::Fieldname>());
                    }
#line 4153 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 334:
#line 1395 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 4159 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 335:
#line 1395 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 4165 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 336:
#line 1395 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 4171 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 337:
#line 1395 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 4177 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 338:
#line 1395 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 4183 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 339:
#line 1395 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 4189 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 340:
#line 1395 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 4195 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 341:
#line 1397 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::cmp, YY_MOVE(yystack_[1].value.as<CNode>())}}};
                    }
#line 4204 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 342:
#line 1402 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::eq, YY_MOVE(yystack_[1].value.as<CNode>())}}};
                    }
#line 4213 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 343:
#line 1407 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::gt, YY_MOVE(yystack_[1].value.as<CNode>())}}};
                    }
#line 4222 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 344:
#line 1412 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::gte, YY_MOVE(yystack_[1].value.as<CNode>())}}};
                    }
#line 4231 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 345:
#line 1417 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::lt, YY_MOVE(yystack_[1].value.as<CNode>())}}};
                    }
#line 4240 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 346:
#line 1422 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::lte, YY_MOVE(yystack_[1].value.as<CNode>())}}};
                    }
#line 4249 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 347:
#line 1427 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::ne, YY_MOVE(yystack_[1].value.as<CNode>())}}};
                    }
#line 4258 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 348:
#line 1433 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 4264 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 349:
#line 1434 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 4270 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 350:
#line 1435 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 4276 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 351:
#line 1436 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 4282 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 352:
#line 1437 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 4288 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 353:
#line 1438 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 4294 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 354:
#line 1439 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 4300 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 355:
#line 1440 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 4306 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 356:
#line 1441 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 4312 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 357:
#line 1442 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 4318 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 358:
#line 1447 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<std::pair<CNode::Fieldname, CNode>>() =
                            std::pair{KeyFieldname::onErrorArg, CNode{KeyValue::absentKey}};
                    }
#line 4326 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 359:
#line 1450 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<std::pair<CNode::Fieldname, CNode>>() = std::pair{
                            KeyFieldname::onErrorArg, YY_MOVE(yystack_[0].value.as<CNode>())};
                    }
#line 4334 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 360:
#line 1457 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<std::pair<CNode::Fieldname, CNode>>() =
                            std::pair{KeyFieldname::onNullArg, CNode{KeyValue::absentKey}};
                    }
#line 4342 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 361:
#line 1460 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<std::pair<CNode::Fieldname, CNode>>() = std::pair{
                            KeyFieldname::onNullArg, YY_MOVE(yystack_[0].value.as<CNode>())};
                    }
#line 4350 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 362:
#line 1467 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::convert,
                             CNode{CNode::ObjectChildren{
                                 {KeyFieldname::inputArg, YY_MOVE(yystack_[6].value.as<CNode>())},
                                 {KeyFieldname::toArg, YY_MOVE(yystack_[2].value.as<CNode>())},
                                 YY_MOVE(
                                     yystack_[5].value.as<std::pair<CNode::Fieldname, CNode>>()),
                                 YY_MOVE(yystack_[4]
                                             .value.as<std::pair<CNode::Fieldname, CNode>>())}}}}};
                    }
#line 4361 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 363:
#line 1476 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::toBool, YY_MOVE(yystack_[1].value.as<CNode>())}}};
                    }
#line 4369 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 364:
#line 1481 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::toDate, YY_MOVE(yystack_[1].value.as<CNode>())}}};
                    }
#line 4377 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 365:
#line 1486 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::toDecimal, YY_MOVE(yystack_[1].value.as<CNode>())}}};
                    }
#line 4385 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 366:
#line 1491 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::toDouble, YY_MOVE(yystack_[1].value.as<CNode>())}}};
                    }
#line 4393 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 367:
#line 1496 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::toInt, YY_MOVE(yystack_[1].value.as<CNode>())}}};
                    }
#line 4401 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 368:
#line 1501 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::toLong, YY_MOVE(yystack_[1].value.as<CNode>())}}};
                    }
#line 4409 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 369:
#line 1506 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::toObjectId, YY_MOVE(yystack_[1].value.as<CNode>())}}};
                    }
#line 4417 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 370:
#line 1511 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::toString, YY_MOVE(yystack_[1].value.as<CNode>())}}};
                    }
#line 4425 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 371:
#line 1516 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::type, YY_MOVE(yystack_[1].value.as<CNode>())}}};
                    }
#line 4433 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;


#line 4437 "src/mongo/db/cst/pipeline_parser_gen.cpp"

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
            YY_STACK_PRINT();

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
            error(yyla.location, yysyntax_error_(yystack_[0].state, yyla));
        }


        yyerror_range[1].location = yyla.location;
        if (yyerrstatus_ == 3) {
            /* If just tried and failed to reuse lookahead token after an
               error, discard it.  */

            // Return failure if at end of input.
            if (yyla.type_get() == yyeof_)
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
        goto yyerrlab1;


    /*-------------------------------------------------------------.
    | yyerrlab1 -- common code for both syntax error and YYERROR.  |
    `-------------------------------------------------------------*/
    yyerrlab1:
        yyerrstatus_ = 3;  // Each real token shifted decrements this.
        {
            stack_symbol_type error_token;
            for (;;) {
                yyn = yypact_[+yystack_[0].state];
                if (!yy_pact_value_is_default_(yyn)) {
                    yyn += yy_error_token_;
                    if (0 <= yyn && yyn <= yylast_ && yycheck_[yyn] == yy_error_token_) {
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
        if (!yyla.empty())
            yy_destroy_("Cleanup: discarding lookahead", yyla);

        /* Do not reclaim the symbols of the rule whose action triggered
           this YYABORT or YYACCEPT.  */
        yypop_(yylen);
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

// Generate an error message.
std::string PipelineParserGen::yysyntax_error_(state_type yystate, const symbol_type& yyla) const {
    // Number of reported tokens (one for the "unexpected", one per
    // "expected").
    std::ptrdiff_t yycount = 0;
    // Its maximum.
    enum { YYERROR_VERBOSE_ARGS_MAXIMUM = 5 };
    // Arguments of yyformat.
    char const* yyarg[YYERROR_VERBOSE_ARGS_MAXIMUM];

    /* There are many possibilities here to consider:
       - If this state is a consistent state with a default action, then
         the only way this function was invoked is if the default action
         is an error action.  In that case, don't check for expected
         tokens because there are none.
       - The only way there can be no lookahead present (in yyla) is
         if this state is a consistent state with a default action.
         Thus, detecting the absence of a lookahead is sufficient to
         determine that there is no unexpected or expected token to
         report.  In that case, just report a simple "syntax error".
       - Don't assume there isn't a lookahead just because this state is
         a consistent state with a default action.  There might have
         been a previous inconsistent state, consistent state with a
         non-default action, or user semantic action that manipulated
         yyla.  (However, yyla is currently not documented for users.)
       - Of course, the expected token list depends on states to have
         correct lookahead information, and it depends on the parser not
         to perform extra reductions after fetching a lookahead from the
         scanner and before detecting a syntax error.  Thus, state merging
         (from LALR or IELR) and default reductions corrupt the expected
         token list.  However, the list is correct for canonical LR with
         one exception: it will still contain any token that will not be
         accepted due to an error action in a later state.
    */
    if (!yyla.empty()) {
        symbol_number_type yytoken = yyla.type_get();
        yyarg[yycount++] = yytname_[yytoken];

        int yyn = yypact_[+yystate];
        if (!yy_pact_value_is_default_(yyn)) {
            /* Start YYX at -YYN if negative to avoid negative indexes in
               YYCHECK.  In other words, skip the first -YYN actions for
               this state because they are default actions.  */
            int yyxbegin = yyn < 0 ? -yyn : 0;
            // Stay within bounds of both yycheck and yytname.
            int yychecklim = yylast_ - yyn + 1;
            int yyxend = yychecklim < yyntokens_ ? yychecklim : yyntokens_;
            for (int yyx = yyxbegin; yyx < yyxend; ++yyx)
                if (yycheck_[yyx + yyn] == yyx && yyx != yy_error_token_ &&
                    !yy_table_value_is_error_(yytable_[yyx + yyn])) {
                    if (yycount == YYERROR_VERBOSE_ARGS_MAXIMUM) {
                        yycount = 1;
                        break;
                    } else
                        yyarg[yycount++] = yytname_[yyx];
                }
        }
    }

    char const* yyformat = YY_NULLPTR;
    switch (yycount) {
#define YYCASE_(N, S) \
    case N:           \
        yyformat = S; \
        break
        default:  // Avoid compiler warnings.
            YYCASE_(0, YY_("syntax error"));
            YYCASE_(1, YY_("syntax error, unexpected %s"));
            YYCASE_(2, YY_("syntax error, unexpected %s, expecting %s"));
            YYCASE_(3, YY_("syntax error, unexpected %s, expecting %s or %s"));
            YYCASE_(4, YY_("syntax error, unexpected %s, expecting %s or %s or %s"));
            YYCASE_(5, YY_("syntax error, unexpected %s, expecting %s or %s or %s or %s"));
#undef YYCASE_
    }

    std::string yyres;
    // Argument number.
    std::ptrdiff_t yyi = 0;
    for (char const* yyp = yyformat; *yyp; ++yyp)
        if (yyp[0] == '%' && yyp[1] == 's' && yyi < yycount) {
            yyres += yytnamerr_(yyarg[yyi++]);
            ++yyp;
        } else
            yyres += *yyp;
    return yyres;
}


const short PipelineParserGen::yypact_ninf_ = -584;

const signed char PipelineParserGen::yytable_ninf_ = -1;

const short PipelineParserGen::yypact_[] = {
    -64,  -42,  -54,  35,   -38,  -584, -584, -584, -584, -28,  13,   356,  -10,  48,   -4,   -2,
    48,   -584, 43,   -584, -584, -584, -584, -584, -584, -584, -584, -584, -584, -584, -584, -584,
    -584, -584, -584, -584, -584, -584, -584, -584, -584, -584, -584, -584, -584, -584, -584, -584,
    -584, -584, -584, -584, -584, -584, -584, -584, -584, -584, -584, -584, 715,  -584, -584, -584,
    -584, -584, -584, -584, -584, -584, -584, -584, -584, -584, -584, -584, -584, -584, -584, -584,
    -584, -584, -584, -584, -584, -584, -584, -584, -584, -584, -584, -584, -584, -584, -584, -584,
    -584, -584, -584, -584, -584, -584, -584, -584, -584, -584, -584, -584, -584, -584, -584, -584,
    -584, 715,  -584, -584, -584, -584, 50,   -584, -584, -584, -584, -584, -584, -584, -584, -584,
    -584, -584, -584, -584, -584, 73,   -584, 88,   27,   -38,  -584, -584, 715,  -584, -584, -584,
    -584, -584, -584, -584, -584, -584, -584, -584, -584, -584, -584, -584, -584, -584, -584, -584,
    -584, -584, -584, -584, -584, -584, -584, -584, -584, -584, -584, -584, -584, -584, -584, -584,
    -584, -584, -584, -584, -584, -584, -584, 451,  48,   16,   -584, -584, 715,  77,   546,  -584,
    734,  734,  -584, -584, -584, -584, -584, 92,   116,  -584, -584, -584, -584, -584, -584, -584,
    -584, -584, 715,  -584, -584, -584, -584, -584, -584, -584, 827,  641,  -584, -584, -584, -584,
    -584, -584, -584, -584, -584, -584, -584, -584, -584, -584, -584, -584, -584, -584, -584, -584,
    -584, -584, -584, -584, -584, -584, -584, -584, -584, -584, -584, -584, -584, -584, -584, -584,
    -584, -584, -584, -584, -584, -584, -584, -584, -584, -584, -584, -584, -584, -584, -584, -584,
    -584, -584, -584, -584, -584, -584, -584, -584, -584, -584, -584, -584, -584, -584, -584, -584,
    -584, -584, -584, -584, -584, -584, -584, -584, -584, -584, -584, -584, -584, -584, -584, -584,
    -584, -584, -584, -584, -584, -584, -584, -24,  -584, -584, 827,  -584, 108,  827,  69,   69,
    70,   827,  70,   72,   74,   -584, -584, -584, 75,   70,   827,  827,  70,   70,   81,   89,
    91,   827,  93,   827,  70,   70,   -584, 95,   99,   70,   100,  69,   102,  -584, -584, -584,
    -584, -584, 104,  -584, 105,  827,  109,  827,  827,  113,  114,  120,  122,  827,  827,  827,
    827,  827,  827,  827,  827,  827,  827,  -584, 124,  827,  874,  112,  -584, -584, 127,  131,
    164,  827,  166,  167,  170,  827,  715,  196,  200,  202,  827,  175,  176,  177,  178,  181,
    827,  827,  715,  182,  827,  183,  185,  186,  217,  827,  827,  188,  827,  189,  827,  191,
    216,  194,  195,  225,  227,  827,  217,  827,  203,  827,  204,  206,  827,  827,  827,  827,
    208,  209,  210,  212,  213,  214,  215,  218,  219,  220,  217,  827,  222,  -584, 827,  -584,
    -584, -584, -584, -584, -584, -584, -584, -584, 827,  -584, -584, -584, 226,  228,  827,  827,
    827,  827,  -584, -584, -584, -584, -584, 827,  827,  229,  -584, 827,  -584, -584, -584, 827,
    240,  827,  827,  -584, 231,  -584, 827,  -584, 827,  -584, -584, 827,  827,  827,  250,  827,
    -584, 827,  -584, -584, 827,  827,  827,  827,  -584, -584, -584, -584, -584, -584, -584, -584,
    -584, -584, 255,  827,  -584, -584, 233,  234,  235,  256,  262,  262,  239,  827,  827,  241,
    243,  -584, 827,  244,  827,  245,  247,  223,  264,  268,  248,  827,  249,  251,  827,  827,
    827,  252,  827,  254,  -584, -584, -584, 827,  271,  827,  270,  270,  257,  827,  259,  260,
    -584, 261,  263,  265,  269,  -584, 267,  827,  274,  827,  827,  277,  278,  279,  280,  272,
    282,  283,  284,  285,  286,  -584, 827,  275,  -584, 827,  256,  271,  -584, -584, 287,  288,
    -584, 290,  -584, 291,  -584, -584, 827,  301,  310,  -584, 293,  -584, -584, 294,  296,  297,
    -584, 298,  -584, -584, 827,  -584, 271,  303,  -584, -584, -584, -584, 304,  827,  827,  -584,
    -584, -584, -584, -584, 305,  306,  308,  -584, 309,  311,  312,  313,  -584, 314,  315,  -584,
    -584, -584, -584};

const short PipelineParserGen::yydefact_[] = {
    0,   0,   0,   0,   5,   2,   59,  3,   1,   0,   0,   0,   0,   0,   0,   0,   0,   7,   0,
    9,   10,  11,  12,  13,  14,  4,   115, 91,  93,  86,  75,  85,  82,  89,  83,  78,  80,  81,
    88,  76,  87,  90,  77,  84,  79,  92,  116, 98,  130, 94,  105, 131, 132, 117, 58,  99,  118,
    119, 100, 101, 0,   133, 134, 95,  120, 121, 122, 102, 103, 135, 123, 124, 104, 97,  96,  125,
    136, 137, 138, 140, 139, 126, 141, 142, 127, 69,  72,  73,  74,  71,  70,  145, 143, 144, 146,
    147, 148, 128, 106, 107, 108, 109, 110, 111, 149, 112, 113, 151, 150, 129, 114, 68,  65,  0,
    66,  67,  64,  60,  0,   173, 171, 167, 169, 166, 168, 170, 172, 18,  19,  20,  21,  23,  25,
    0,   22,  0,   0,   5,   175, 174, 323, 326, 152, 153, 154, 155, 156, 157, 158, 159, 160, 161,
    162, 163, 164, 165, 183, 184, 185, 186, 187, 192, 188, 189, 190, 193, 194, 63,  176, 177, 178,
    179, 191, 180, 181, 182, 318, 319, 320, 321, 61,  62,  16,  0,   0,   0,   8,   6,   323, 0,
    0,   24,  0,   0,   55,  56,  57,  54,  26,  0,   0,   324, 322, 325, 217, 330, 331, 332, 329,
    333, 0,   327, 49,  48,  47,  45,  41,  43,  195, 210, 40,  42,  44,  46,  36,  37,  38,  39,
    50,  51,  52,  29,  30,  31,  32,  33,  34,  35,  27,  53,  200, 201, 202, 218, 219, 203, 252,
    253, 254, 204, 314, 315, 207, 258, 259, 260, 261, 262, 263, 264, 265, 266, 267, 268, 269, 270,
    271, 272, 273, 274, 275, 276, 277, 279, 278, 205, 334, 335, 336, 337, 338, 339, 340, 206, 348,
    349, 350, 351, 352, 353, 354, 355, 356, 357, 220, 221, 222, 223, 224, 225, 226, 227, 228, 229,
    230, 231, 232, 233, 234, 28,  15,  0,   328, 197, 195, 198, 0,   0,   0,   0,   0,   0,   0,
    0,   0,   7,   7,   7,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,
    7,   0,   0,   0,   0,   0,   0,   7,   7,   7,   7,   7,   0,   7,   0,   0,   0,   0,   0,
    0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   7,   0,   0,   0,   0,
    196, 208, 0,   0,   0,   0,   0,   0,   0,   195, 0,   0,   0,   0,   0,   0,   0,   0,   0,
    0,   0,   0,   0,   0,   0,   0,   0,   0,   292, 0,   0,   0,   0,   0,   0,   0,   0,   0,
    0,   0,   0,   0,   292, 0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,
    0,   0,   0,   0,   0,   292, 0,   0,   209, 0,   214, 215, 213, 216, 211, 17,  237, 235, 255,
    0,   236, 238, 341, 0,   0,   0,   0,   0,   0,   342, 240, 241, 343, 344, 0,   0,   0,   242,
    0,   244, 345, 346, 0,   0,   0,   0,   347, 0,   256, 0,   300, 0,   301, 302, 0,   0,   0,
    0,   0,   249, 0,   306, 307, 0,   0,   0,   0,   363, 364, 365, 366, 367, 368, 312, 369, 370,
    313, 0,   0,   371, 212, 0,   0,   0,   358, 281, 281, 0,   287, 287, 0,   0,   293, 0,   0,
    195, 0,   0,   297, 0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   199, 280, 316,
    0,   360, 0,   283, 283, 0,   288, 0,   0,   317, 0,   0,   0,   0,   257, 0,   0,   0,   0,
    0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   359, 0,   0,   282, 0,   358, 360, 239,
    289, 0,   0,   243, 0,   245, 0,   247, 298, 0,   0,   0,   248, 0,   305, 308, 0,   0,   0,
    250, 0,   251, 361, 0,   284, 360, 0,   290, 291, 294, 246, 0,   0,   0,   295, 309, 310, 311,
    296, 0,   0,   0,   299, 0,   0,   0,   0,   286, 0,   0,   362, 285, 304, 303};

const short PipelineParserGen::yypgoto_[] = {
    -584, -584, -584, -177, -584, -174, -166, -171, -86,  -584, -584, -584, -584, -584, -124, -122,
    -116, -87,  2,    -85,  10,   -11,  12,   -83,  -76,  -137, -153, -70,  -68,  -66,  -584, -62,
    -58,  -55,  -59,  -584, -584, -584, -584, 161,  -584, -584, -584, -584, -584, -584, -584, -584,
    144,  14,   -310, -53,  -244, -282, -584, -584, -584, -584, -584, -584, -584, -584, -584, -584,
    -584, -584, -584, -584, -584, -584, -584, -584, -584, -584, -584, -584, -209, -584, -584, -584,
    -584, -584, -584, -584, -584, -584, -584, -584, -584, -584, -584, -584, -584, -584, -584, -584,
    -584, -584, -584, -584, -584, -584, -584, -584, -584, -584, -584, -584, -584, -584, -584, -584,
    -584, -584, -584, -584, -584, -584, -584, -584, -584, -584, -584, -584, -281, -583, -217, -203,
    -408, -584, -306, 158,  -173, -584, -584, 242,  -584, -584, -17,  -584};

const short PipelineParserGen::yydefgoto_[] = {
    -1,  193, 446, 112, 113, 114, 115, 116, 209, 210, 198, 451, 211, 117, 156, 157, 158, 159,
    160, 161, 162, 163, 164, 165, 166, 167, 168, 169, 170, 171, 172, 173, 174, 175, 313, 177,
    178, 179, 190, 10,  18,  19,  20,  21,  22,  23,  24,  183, 238, 131, 314, 315, 386, 240,
    241, 378, 242, 243, 244, 245, 246, 247, 248, 249, 250, 251, 252, 253, 254, 255, 256, 257,
    258, 259, 260, 261, 415, 262, 263, 264, 265, 266, 267, 268, 269, 270, 271, 272, 273, 274,
    275, 276, 277, 278, 279, 280, 281, 282, 283, 284, 285, 286, 287, 288, 289, 290, 291, 292,
    293, 294, 295, 296, 297, 298, 299, 300, 301, 302, 303, 304, 305, 306, 307, 308, 552, 583,
    554, 586, 480, 568, 316, 189, 558, 7,   11,  180, 3,   5,   416, 136};

const short PipelineParserGen::yytable_[] = {
    135, 176, 129, 188, 615, 129, 194, 382, 380, 195, 120, 387, 197, 205, 494, 127, 206, 196, 127,
    208, 396, 397, 6,   128, 207, 130, 128, 403, 130, 405, 134, 629, 200, 4,   514, 8,   383, 384,
    9,   231, 231, 12,  13,  14,  15,  16,  17,  424, 25,  426, 427, 188, 1,   2,   176, 432, 433,
    434, 435, 436, 437, 438, 439, 440, 441, 413, 118, 444, 224, 224, 225, 225, 132, 312, 133, 456,
    226, 226, 388, 137, 119, 176, 120, 460, 465, 395, 182, 125, 398, 399, 471, 472, 184, 121, 475,
    185, 406, 407, 122, 481, 482, 411, 484, 186, 486, 227, 227, 228, 228, 229, 229, 493, 202, 495,
    142, 497, 230, 230, 500, 501, 502, 503, 232, 232, 233, 233, 234, 234, 310, 176, 235, 235, 311,
    515, 236, 236, 517, 237, 237, 239, 239, 417, 418, 381, 218, 385, 518, 389, 452, 390, 394, 176,
    521, 522, 523, 524, 400, 123, 124, 125, 126, 525, 526, 453, 401, 528, 402, 454, 404, 529, 409,
    531, 532, 129, 410, 412, 534, 414, 535, 421, 423, 536, 537, 538, 425, 540, 127, 541, 428, 429,
    542, 543, 544, 545, 128, 430, 130, 431, 199, 443, 455, 447, 457, 458, 448, 547, 459, 449, 462,
    463, 464, 466, 467, 468, 469, 557, 557, 470, 474, 476, 562, 477, 478, 479, 483, 485, 564, 487,
    488, 572, 489, 490, 575, 576, 577, 491, 579, 492, 567, 496, 498, 581, 499, 584, 504, 505, 506,
    589, 507, 508, 509, 510, 530, 461, 511, 512, 513, 597, 516, 599, 600, 519, 539, 520, 527, 473,
    533, 546, 548, 551, 549, 550, 611, 553, 556, 613, 569, 560, 561, 563, 570, 565, 566, 571, 573,
    582, 574, 578, 620, 580, 585, 598, 450, 588, 590, 591, 612, 592, 187, 593, 379, 594, 628, 596,
    595, 614, 555, 605, 391, 392, 393, 632, 633, 601, 602, 603, 604, 606, 607, 621, 608, 609, 610,
    616, 617, 408, 618, 619, 622, 623, 624, 176, 625, 626, 627, 419, 420, 309, 422, 630, 631, 634,
    635, 176, 636, 637, 201, 638, 639, 640, 641, 642, 587, 559, 0,   181, 0,   0,   442, 26,  27,
    28,  29,  30,  31,  32,  33,  34,  35,  36,  37,  38,  39,  40,  41,  42,  43,  44,  45,  0,
    0,   46,  47,  48,  49,  50,  51,  52,  0,   53,  0,   0,   54,  55,  56,  57,  58,  59,  60,
    61,  62,  0,   63,  64,  65,  66,  0,   67,  68,  69,  70,  71,  72,  73,  74,  75,  76,  77,
    78,  79,  80,  81,  82,  83,  84,  85,  86,  87,  88,  89,  90,  0,   0,   91,  92,  93,  94,
    95,  96,  97,  98,  99,  100, 101, 102, 103, 104, 105, 106, 107, 108, 109, 110, 111, 26,  27,
    28,  29,  30,  31,  32,  33,  34,  35,  36,  37,  38,  39,  40,  41,  42,  43,  44,  45,  0,
    0,   46,  47,  48,  49,  50,  51,  52,  0,   53,  0,   0,   191, 55,  56,  57,  58,  59,  192,
    61,  62,  0,   63,  64,  65,  66,  0,   67,  68,  69,  70,  71,  72,  73,  74,  75,  76,  77,
    78,  79,  80,  81,  82,  83,  84,  85,  86,  87,  88,  89,  90,  0,   0,   91,  92,  93,  94,
    95,  96,  97,  98,  99,  100, 101, 102, 103, 104, 105, 106, 107, 108, 109, 110, 111, 26,  27,
    28,  29,  30,  31,  32,  33,  34,  35,  36,  37,  38,  39,  40,  41,  42,  43,  44,  45,  0,
    0,   46,  47,  48,  49,  50,  51,  52,  0,   53,  0,   0,   203, 55,  56,  57,  58,  59,  204,
    61,  62,  0,   63,  64,  65,  66,  0,   67,  68,  69,  70,  71,  72,  73,  74,  75,  76,  77,
    78,  79,  80,  81,  82,  83,  84,  85,  86,  87,  88,  89,  90,  0,   0,   91,  92,  93,  94,
    95,  96,  97,  98,  99,  100, 101, 102, 103, 104, 105, 106, 107, 108, 109, 110, 111, 317, 318,
    319, 0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   320, 0,
    0,   321, 322, 323, 324, 325, 326, 327, 0,   328, 0,   0,   0,   329, 330, 331, 332, 333, 0,
    334, 335, 0,   336, 337, 338, 339, 0,   340, 341, 342, 343, 344, 345, 346, 347, 348, 349, 350,
    351, 352, 353, 354, 355, 356, 357, 0,   0,   0,   0,   0,   0,   0,   0,   358, 359, 360, 361,
    362, 363, 364, 365, 366, 367, 368, 369, 370, 371, 372, 373, 374, 375, 376, 377, 138, 139, 0,
    0,   0,   0,   0,   0,   0,   119, 0,   120, 0,   0,   0,   0,   0,   0,   0,   212, 213, 0,
    121, 0,   0,   0,   0,   122, 214, 0,   215, 0,   0,   0,   0,   0,   0,   0,   0,   0,   0,
    216, 0,   0,   0,   0,   217, 0,   0,   0,   0,   0,   140, 141, 0,   0,   0,   0,   0,   0,
    0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   218, 219, 0,   0,   142, 143, 144, 145,
    146, 147, 148, 149, 150, 151, 152, 123, 124, 125, 126, 153, 154, 155, 0,   142, 143, 144, 145,
    146, 147, 148, 149, 150, 151, 152, 220, 221, 222, 223, 153, 154, 155, 138, 139, 0,   0,   0,
    0,   0,   0,   0,   119, 0,   120, 0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   121, 0,
    0,   0,   0,   122, 0,   0,   29,  30,  31,  32,  33,  34,  35,  36,  37,  38,  39,  40,  41,
    42,  43,  44,  0,   0,   0,   0,   0,   0,   218, 219, 0,   0,   0,   0,   0,   0,   445, 0,
    0,   0,   0,   0,   204, 0,   0,   0,   0,   0,   0,   0,   0,   142, 143, 144, 145, 146, 147,
    148, 149, 150, 151, 152, 123, 124, 125, 126, 153, 154, 155, 85,  86,  87,  88,  89,  90,  0,
    0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,
    0,   0,   111};

const short PipelineParserGen::yycheck_[] = {
    17,  60,  13,  140, 587, 16,  183, 317, 314, 183, 34,  321, 183, 190, 422, 13,  190, 183, 16,
    190, 330, 331, 76,  13,  190, 13,  16,  337, 16,  339, 16,  614, 185, 75,  442, 0,   318, 319,
    76,  192, 193, 69,  70,  71,  72,  73,  74,  357, 35,  359, 360, 188, 116, 117, 113, 365, 366,
    367, 368, 369, 370, 371, 372, 373, 374, 347, 76,  377, 192, 193, 192, 193, 76,  210, 76,  385,
    192, 193, 322, 36,  32,  140, 34,  389, 394, 329, 36,  111, 332, 333, 400, 401, 19,  45,  404,
    7,   340, 341, 50,  409, 410, 345, 412, 76,  414, 192, 193, 192, 193, 192, 193, 421, 35,  423,
    98,  425, 192, 193, 428, 429, 430, 431, 192, 193, 192, 193, 192, 193, 36,  188, 192, 193, 16,
    443, 192, 193, 446, 192, 193, 192, 193, 350, 351, 35,  75,  75,  456, 75,  36,  75,  75,  210,
    462, 463, 464, 465, 75,  109, 110, 111, 112, 471, 472, 36,  75,  475, 75,  36,  75,  479, 75,
    481, 482, 184, 75,  75,  486, 75,  488, 75,  75,  491, 492, 493, 75,  495, 184, 497, 75,  75,
    500, 501, 502, 503, 184, 75,  184, 75,  184, 75,  36,  378, 36,  36,  378, 515, 36,  378, 12,
    9,   8,   36,  36,  36,  36,  525, 526, 36,  36,  36,  530, 36,  36,  6,   36,  36,  532, 36,
    12,  539, 36,  36,  542, 543, 544, 10,  546, 10,  15,  36,  36,  551, 36,  553, 36,  36,  36,
    557, 36,  36,  36,  36,  12,  390, 36,  36,  36,  567, 36,  569, 570, 35,  12,  35,  35,  402,
    35,  12,  35,  13,  36,  36,  582, 11,  35,  585, 12,  36,  35,  35,  12,  36,  35,  35,  35,
    14,  35,  35,  598, 35,  20,  17,  378, 36,  35,  35,  21,  36,  137, 36,  311, 36,  612, 36,
    35,  586, 523, 35,  325, 326, 327, 621, 622, 36,  36,  36,  36,  35,  35,  18,  36,  36,  36,
    36,  36,  342, 36,  36,  18,  36,  36,  390, 36,  36,  36,  352, 353, 193, 355, 36,  36,  36,
    36,  402, 36,  36,  188, 36,  36,  36,  36,  36,  555, 526, -1,  113, -1,  -1,  375, 3,   4,
    5,   6,   7,   8,   9,   10,  11,  12,  13,  14,  15,  16,  17,  18,  19,  20,  21,  22,  -1,
    -1,  25,  26,  27,  28,  29,  30,  31,  -1,  33,  -1,  -1,  36,  37,  38,  39,  40,  41,  42,
    43,  44,  -1,  46,  47,  48,  49,  -1,  51,  52,  53,  54,  55,  56,  57,  58,  59,  60,  61,
    62,  63,  64,  65,  66,  67,  68,  69,  70,  71,  72,  73,  74,  -1,  -1,  77,  78,  79,  80,
    81,  82,  83,  84,  85,  86,  87,  88,  89,  90,  91,  92,  93,  94,  95,  96,  97,  3,   4,
    5,   6,   7,   8,   9,   10,  11,  12,  13,  14,  15,  16,  17,  18,  19,  20,  21,  22,  -1,
    -1,  25,  26,  27,  28,  29,  30,  31,  -1,  33,  -1,  -1,  36,  37,  38,  39,  40,  41,  42,
    43,  44,  -1,  46,  47,  48,  49,  -1,  51,  52,  53,  54,  55,  56,  57,  58,  59,  60,  61,
    62,  63,  64,  65,  66,  67,  68,  69,  70,  71,  72,  73,  74,  -1,  -1,  77,  78,  79,  80,
    81,  82,  83,  84,  85,  86,  87,  88,  89,  90,  91,  92,  93,  94,  95,  96,  97,  3,   4,
    5,   6,   7,   8,   9,   10,  11,  12,  13,  14,  15,  16,  17,  18,  19,  20,  21,  22,  -1,
    -1,  25,  26,  27,  28,  29,  30,  31,  -1,  33,  -1,  -1,  36,  37,  38,  39,  40,  41,  42,
    43,  44,  -1,  46,  47,  48,  49,  -1,  51,  52,  53,  54,  55,  56,  57,  58,  59,  60,  61,
    62,  63,  64,  65,  66,  67,  68,  69,  70,  71,  72,  73,  74,  -1,  -1,  77,  78,  79,  80,
    81,  82,  83,  84,  85,  86,  87,  88,  89,  90,  91,  92,  93,  94,  95,  96,  97,  3,   4,
    5,   -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  22,  -1,
    -1,  25,  26,  27,  28,  29,  30,  31,  -1,  33,  -1,  -1,  -1,  37,  38,  39,  40,  41,  -1,
    43,  44,  -1,  46,  47,  48,  49,  -1,  51,  52,  53,  54,  55,  56,  57,  58,  59,  60,  61,
    62,  63,  64,  65,  66,  67,  68,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  77,  78,  79,  80,
    81,  82,  83,  84,  85,  86,  87,  88,  89,  90,  91,  92,  93,  94,  95,  96,  23,  24,  -1,
    -1,  -1,  -1,  -1,  -1,  -1,  32,  -1,  34,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  23,  24,  -1,
    45,  -1,  -1,  -1,  -1,  50,  32,  -1,  34,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,
    45,  -1,  -1,  -1,  -1,  50,  -1,  -1,  -1,  -1,  -1,  75,  76,  -1,  -1,  -1,  -1,  -1,  -1,
    -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  75,  76,  -1,  -1,  98,  99,  100, 101,
    102, 103, 104, 105, 106, 107, 108, 109, 110, 111, 112, 113, 114, 115, -1,  98,  99,  100, 101,
    102, 103, 104, 105, 106, 107, 108, 109, 110, 111, 112, 113, 114, 115, 23,  24,  -1,  -1,  -1,
    -1,  -1,  -1,  -1,  32,  -1,  34,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  45,  -1,
    -1,  -1,  -1,  50,  -1,  -1,  6,   7,   8,   9,   10,  11,  12,  13,  14,  15,  16,  17,  18,
    19,  20,  21,  -1,  -1,  -1,  -1,  -1,  -1,  75,  76,  -1,  -1,  -1,  -1,  -1,  -1,  36,  -1,
    -1,  -1,  -1,  -1,  42,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  98,  99,  100, 101, 102, 103,
    104, 105, 106, 107, 108, 109, 110, 111, 112, 113, 114, 115, 69,  70,  71,  72,  73,  74,  -1,
    -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,
    -1,  -1,  97};

const short PipelineParserGen::yystos_[] = {
    0,   116, 117, 254, 75,  255, 76,  251, 0,   76,  157, 252, 69,  70,  71,  72,  73,  74,  158,
    159, 160, 161, 162, 163, 164, 35,  3,   4,   5,   6,   7,   8,   9,   10,  11,  12,  13,  14,
    15,  16,  17,  18,  19,  20,  21,  22,  25,  26,  27,  28,  29,  30,  31,  33,  36,  37,  38,
    39,  40,  41,  42,  43,  44,  46,  47,  48,  49,  51,  52,  53,  54,  55,  56,  57,  58,  59,
    60,  61,  62,  63,  64,  65,  66,  67,  68,  69,  70,  71,  72,  73,  74,  77,  78,  79,  80,
    81,  82,  83,  84,  85,  86,  87,  88,  89,  90,  91,  92,  93,  94,  95,  96,  97,  121, 122,
    123, 124, 125, 131, 76,  32,  34,  45,  50,  109, 110, 111, 112, 136, 138, 139, 140, 167, 76,
    76,  167, 256, 257, 36,  23,  24,  75,  76,  98,  99,  100, 101, 102, 103, 104, 105, 106, 107,
    108, 113, 114, 115, 132, 133, 134, 135, 136, 137, 138, 139, 140, 141, 142, 143, 144, 145, 146,
    147, 148, 149, 150, 151, 152, 153, 154, 155, 253, 253, 36,  165, 19,  7,   76,  157, 143, 249,
    156, 36,  42,  119, 121, 123, 124, 125, 128, 167, 144, 249, 35,  36,  42,  121, 123, 124, 125,
    126, 127, 130, 23,  24,  32,  34,  45,  50,  75,  76,  109, 110, 111, 112, 132, 133, 134, 135,
    137, 141, 142, 144, 145, 146, 147, 149, 150, 151, 166, 169, 171, 172, 174, 175, 176, 177, 178,
    179, 180, 181, 182, 183, 184, 185, 186, 187, 188, 189, 190, 191, 192, 193, 195, 196, 197, 198,
    199, 200, 201, 202, 203, 204, 205, 206, 207, 208, 209, 210, 211, 212, 213, 214, 215, 216, 217,
    218, 219, 220, 221, 222, 223, 224, 225, 226, 227, 228, 229, 230, 231, 232, 233, 234, 235, 236,
    237, 238, 239, 240, 241, 166, 36,  16,  143, 152, 168, 169, 248, 3,   4,   5,   22,  25,  26,
    27,  28,  29,  30,  31,  33,  37,  38,  39,  40,  41,  43,  44,  46,  47,  48,  49,  51,  52,
    53,  54,  55,  56,  57,  58,  59,  60,  61,  62,  63,  64,  65,  66,  67,  68,  77,  78,  79,
    80,  81,  82,  83,  84,  85,  86,  87,  88,  89,  90,  91,  92,  93,  94,  95,  96,  173, 139,
    248, 35,  168, 171, 171, 75,  170, 168, 170, 75,  75,  256, 256, 256, 75,  170, 168, 168, 170,
    170, 75,  75,  75,  168, 75,  168, 170, 170, 256, 75,  75,  170, 75,  171, 75,  194, 256, 194,
    194, 256, 256, 75,  256, 75,  168, 75,  168, 168, 75,  75,  75,  75,  168, 168, 168, 168, 168,
    168, 168, 168, 168, 168, 256, 75,  168, 36,  120, 121, 123, 125, 126, 129, 36,  36,  36,  36,
    168, 36,  36,  36,  248, 143, 12,  9,   8,   168, 36,  36,  36,  36,  36,  168, 168, 143, 36,
    168, 36,  36,  36,  6,   246, 168, 168, 36,  168, 36,  168, 36,  12,  36,  36,  10,  10,  168,
    246, 168, 36,  168, 36,  36,  168, 168, 168, 168, 36,  36,  36,  36,  36,  36,  36,  36,  36,
    36,  246, 168, 36,  168, 168, 35,  35,  168, 168, 168, 168, 168, 168, 35,  168, 168, 12,  168,
    168, 35,  168, 168, 168, 168, 168, 12,  168, 168, 168, 168, 168, 168, 12,  168, 35,  36,  36,
    13,  242, 11,  244, 244, 35,  168, 250, 250, 36,  35,  168, 35,  248, 36,  35,  15,  247, 12,
    12,  35,  168, 35,  35,  168, 168, 168, 35,  168, 35,  168, 14,  243, 168, 20,  245, 245, 36,
    168, 35,  35,  36,  36,  36,  35,  36,  168, 17,  168, 168, 36,  36,  36,  36,  35,  35,  35,
    36,  36,  36,  168, 21,  168, 242, 243, 36,  36,  36,  36,  168, 18,  18,  36,  36,  36,  36,
    36,  168, 243, 36,  36,  168, 168, 36,  36,  36,  36,  36,  36,  36,  36,  36};

const short PipelineParserGen::yyr1_[] = {
    0,   118, 254, 254, 255, 157, 157, 257, 256, 158, 158, 158, 158, 158, 158, 164, 159, 160, 167,
    167, 167, 167, 161, 162, 163, 165, 165, 128, 128, 166, 166, 166, 166, 166, 166, 166, 166, 166,
    166, 166, 166, 166, 166, 166, 166, 166, 166, 166, 166, 166, 166, 166, 166, 166, 119, 119, 119,
    119, 251, 252, 252, 131, 131, 253, 122, 122, 122, 122, 125, 121, 121, 121, 121, 121, 121, 123,
    123, 123, 123, 123, 123, 123, 123, 123, 123, 123, 123, 123, 123, 123, 123, 124, 124, 124, 124,
    124, 124, 124, 124, 124, 124, 124, 124, 124, 124, 124, 124, 124, 124, 124, 124, 124, 124, 124,
    124, 124, 124, 124, 124, 124, 124, 124, 124, 124, 124, 124, 124, 124, 124, 124, 124, 124, 124,
    124, 124, 124, 124, 124, 124, 124, 124, 124, 124, 124, 124, 124, 124, 124, 124, 124, 124, 124,
    144, 145, 146, 147, 149, 150, 151, 132, 133, 134, 135, 137, 141, 142, 136, 136, 138, 138, 139,
    139, 140, 140, 148, 148, 152, 152, 152, 152, 152, 152, 152, 152, 152, 152, 152, 152, 152, 152,
    152, 152, 152, 152, 152, 248, 248, 168, 168, 170, 169, 169, 169, 169, 169, 169, 169, 169, 171,
    172, 173, 173, 129, 120, 120, 120, 120, 126, 174, 174, 174, 174, 174, 174, 174, 174, 174, 174,
    174, 174, 174, 174, 174, 174, 174, 175, 176, 227, 228, 229, 230, 231, 232, 233, 234, 235, 236,
    237, 238, 239, 240, 241, 177, 177, 177, 178, 179, 180, 184, 184, 184, 184, 184, 184, 184, 184,
    184, 184, 184, 184, 184, 184, 184, 184, 184, 184, 184, 184, 184, 184, 185, 244, 244, 245, 245,
    186, 187, 250, 250, 250, 188, 189, 246, 246, 190, 197, 207, 247, 247, 194, 191, 192, 193, 195,
    196, 198, 199, 200, 201, 202, 203, 204, 205, 206, 181, 181, 182, 183, 143, 143, 153, 153, 154,
    249, 249, 155, 156, 156, 130, 127, 127, 127, 127, 127, 208, 208, 208, 208, 208, 208, 208, 209,
    210, 211, 212, 213, 214, 215, 216, 216, 216, 216, 216, 216, 216, 216, 216, 216, 242, 242, 243,
    243, 217, 218, 219, 220, 221, 222, 223, 224, 225, 226};

const signed char PipelineParserGen::yyr2_[] = {
    0, 2, 2, 2, 3, 0, 4, 0, 2, 1, 1, 1, 1, 1,  1,  5, 3, 7, 1, 1, 1, 1, 2, 2, 4,  0,  2, 2, 2,
    1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,  1,  1, 1, 1, 1, 1, 1, 1, 1, 1, 1,  1,  1, 1, 1,
    3, 0, 2, 2, 2, 1, 1, 1, 1, 1, 1, 1, 1, 1,  1,  1, 1, 1, 1, 1, 1, 1, 1, 1, 1,  1,  1, 1, 1,
    1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,  1,  1, 1, 1, 1, 1, 1, 1, 1, 1, 1,  1,  1, 1, 1,
    1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,  1,  1, 1, 1, 1, 1, 1, 1, 1, 1, 1,  1,  1, 1, 1,
    1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,  1,  1, 1, 1, 1, 1, 1, 1, 1, 1, 1,  1,  1, 1, 1,
    1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,  1,  1, 1, 1, 1, 1, 1, 0, 2, 1, 1,  4,  1, 1, 1,
    1, 1, 1, 1, 1, 3, 3, 0, 2, 2, 1, 1, 1, 1,  1,  1, 1, 1, 1, 1, 1, 1, 1, 1, 1,  1,  1, 1, 1,
    1, 1, 1, 4, 4, 4, 4, 7, 4, 4, 4, 7, 4, 7,  8,  7, 7, 4, 7, 7, 1, 1, 1, 4, 4,  6,  1, 1, 1,
    1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,  1,  1, 1, 1, 1, 6, 0, 2, 0, 2, 11, 10, 0, 1, 2,
    8, 8, 0, 2, 8, 8, 8, 0, 2, 7, 4, 4, 4, 11, 11, 7, 4, 4, 7, 8, 8, 8, 4, 4, 1,  1,  6, 6, 1,
    1, 1, 1, 3, 0, 2, 3, 0, 2, 2, 1, 1, 1, 1,  1,  1, 1, 1, 1, 1, 1, 1, 4, 4, 4,  4,  4, 4, 4,
    1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 0, 2, 0, 2,  11, 4, 4, 4, 4, 4, 4, 4, 4, 4};


// YYTNAME[SYMBOL-NUM] -- String name of the symbol SYMBOL-NUM.
// First, the terminals, then, starting at \a yyntokens_, nonterminals.
const char* const PipelineParserGen::yytname_[] = {"\"EOF\"",
                                                   "error",
                                                   "$undefined",
                                                   "ABS",
                                                   "ADD",
                                                   "AND",
                                                   "\"chars argument\"",
                                                   "\"coll argument\"",
                                                   "\"date argument\"",
                                                   "\"dateString argument\"",
                                                   "\"find argument\"",
                                                   "\"format argument\"",
                                                   "\"input argument\"",
                                                   "\"onError argument\"",
                                                   "\"onNull argument\"",
                                                   "\"options argument\"",
                                                   "\"pipeline argument\"",
                                                   "\"regex argument\"",
                                                   "\"replacement argument\"",
                                                   "\"size argument\"",
                                                   "\"timezone argument\"",
                                                   "\"to argument\"",
                                                   "ATAN2",
                                                   "\"false\"",
                                                   "\"true\"",
                                                   "CEIL",
                                                   "CMP",
                                                   "CONCAT",
                                                   "CONST_EXPR",
                                                   "CONVERT",
                                                   "DATE_FROM_STRING",
                                                   "DATE_TO_STRING",
                                                   "\"zero (decimal)\"",
                                                   "DIVIDE",
                                                   "\"zero (double)\"",
                                                   "\"end of array\"",
                                                   "\"end of object\"",
                                                   "EQ",
                                                   "EXPONENT",
                                                   "FLOOR",
                                                   "GT",
                                                   "GTE",
                                                   "ID",
                                                   "INDEX_OF_BYTES",
                                                   "INDEX_OF_CP",
                                                   "\"zero (int)\"",
                                                   "LITERAL",
                                                   "LN",
                                                   "LOG",
                                                   "LOGTEN",
                                                   "\"zero (long)\"",
                                                   "LT",
                                                   "LTE",
                                                   "LTRIM",
                                                   "MOD",
                                                   "MULTIPLY",
                                                   "NE",
                                                   "NOT",
                                                   "OR",
                                                   "POW",
                                                   "REGEX_FIND",
                                                   "REGEX_FIND_ALL",
                                                   "REGEX_MATCH",
                                                   "REPLACE_ALL",
                                                   "REPLACE_ONE",
                                                   "ROUND",
                                                   "RTRIM",
                                                   "SPLIT",
                                                   "SQRT",
                                                   "STAGE_INHIBIT_OPTIMIZATION",
                                                   "STAGE_LIMIT",
                                                   "STAGE_PROJECT",
                                                   "STAGE_SAMPLE",
                                                   "STAGE_SKIP",
                                                   "STAGE_UNION_WITH",
                                                   "\"array\"",
                                                   "\"object\"",
                                                   "STR_CASE_CMP",
                                                   "STR_LEN_BYTES",
                                                   "STR_LEN_CP",
                                                   "SUBSTR",
                                                   "SUBSTR_BYTES",
                                                   "SUBSTR_CP",
                                                   "SUBTRACT",
                                                   "TO_BOOL",
                                                   "TO_DATE",
                                                   "TO_DECIMAL",
                                                   "TO_DOUBLE",
                                                   "TO_INT",
                                                   "TO_LONG",
                                                   "TO_LOWER",
                                                   "TO_OBJECT_ID",
                                                   "TO_STRING",
                                                   "TO_UPPER",
                                                   "TRIM",
                                                   "TRUNC",
                                                   "TYPE",
                                                   "\"fieldname\"",
                                                   "\"string\"",
                                                   "\"BinData\"",
                                                   "\"undefined\"",
                                                   "\"ObjectID\"",
                                                   "\"Date\"",
                                                   "\"null\"",
                                                   "\"regex\"",
                                                   "\"dbPointer\"",
                                                   "\"Code\"",
                                                   "\"Symbol\"",
                                                   "\"CodeWScope\"",
                                                   "\"non-zero integer\"",
                                                   "\"non-zero long\"",
                                                   "\"non-zero double\"",
                                                   "\"non-zero decimal\"",
                                                   "\"Timestamp\"",
                                                   "\"minKey\"",
                                                   "\"maxKey\"",
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
                                                   "stringExps",
                                                   "concat",
                                                   "dateFromString",
                                                   "dateToString",
                                                   "indexOfBytes",
                                                   "indexOfCP",
                                                   "ltrim",
                                                   "regexFind",
                                                   "regexFindAll",
                                                   "regexMatch",
                                                   "regexArgs",
                                                   "replaceOne",
                                                   "replaceAll",
                                                   "rtrim",
                                                   "split",
                                                   "strLenBytes",
                                                   "strLenCP",
                                                   "strcasecmp",
                                                   "substr",
                                                   "substrBytes",
                                                   "substrCP",
                                                   "toLower",
                                                   "toUpper",
                                                   "trim",
                                                   "compExprs",
                                                   "cmp",
                                                   "eq",
                                                   "gt",
                                                   "gte",
                                                   "lt",
                                                   "lte",
                                                   "ne",
                                                   "typeExpression",
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
                                                   "formatArg",
                                                   "timezoneArg",
                                                   "charsArg",
                                                   "optionsArg",
                                                   "expressions",
                                                   "values",
                                                   "exprZeroToTwo",
                                                   "matchExpression",
                                                   "filterFields",
                                                   "filterVal",
                                                   "start",
                                                   "pipeline",
                                                   "START_ORDERED_OBJECT",
                                                   "$@1",
                                                   YY_NULLPTR};

#if YYDEBUG
const short PipelineParserGen::yyrline_[] = {
    0,    278,  278,  279,  286,  292,  293,  301,  301,  304,  304,  304,  304,  304,  304,  307,
    317,  323,  333,  333,  333,  333,  337,  342,  347,  362,  365,  372,  375,  381,  382,  383,
    384,  385,  386,  387,  388,  389,  390,  391,  392,  395,  398,  401,  404,  407,  410,  413,
    416,  419,  422,  423,  424,  425,  429,  429,  429,  429,  433,  439,  442,  449,  452,  458,
    462,  462,  462,  462,  466,  474,  477,  480,  483,  486,  489,  498,  501,  504,  507,  510,
    513,  516,  519,  522,  525,  528,  531,  534,  537,  540,  543,  551,  554,  557,  560,  563,
    566,  569,  572,  575,  578,  581,  584,  587,  590,  593,  596,  599,  602,  605,  608,  611,
    614,  617,  620,  623,  626,  629,  632,  635,  638,  641,  644,  647,  650,  653,  656,  659,
    662,  665,  668,  671,  674,  677,  680,  683,  686,  689,  692,  695,  698,  701,  704,  707,
    710,  713,  716,  719,  722,  725,  728,  731,  738,  744,  750,  756,  762,  768,  774,  780,
    786,  792,  798,  804,  810,  816,  822,  825,  831,  834,  840,  843,  849,  852,  858,  861,
    867,  868,  869,  870,  871,  872,  873,  874,  875,  876,  877,  878,  879,  880,  881,  882,
    883,  884,  885,  892,  893,  900,  900,  904,  909,  909,  909,  909,  909,  909,  910,  910,
    916,  924,  930,  933,  940,  947,  947,  947,  947,  951,  957,  957,  957,  957,  957,  957,
    957,  957,  957,  957,  957,  957,  957,  958,  958,  958,  958,  962,  969,  975,  980,  985,
    991,  996,  1001, 1006, 1012, 1017, 1023, 1032, 1038, 1044, 1049, 1055, 1061, 1061, 1061, 1065,
    1072, 1079, 1086, 1086, 1086, 1086, 1086, 1086, 1086, 1087, 1087, 1087, 1087, 1087, 1087, 1087,
    1087, 1088, 1088, 1088, 1088, 1088, 1088, 1088, 1092, 1102, 1105, 1111, 1114, 1120, 1129, 1138,
    1141, 1144, 1150, 1161, 1172, 1175, 1181, 1189, 1197, 1205, 1208, 1213, 1222, 1228, 1234, 1240,
    1250, 1260, 1267, 1274, 1281, 1289, 1297, 1305, 1313, 1319, 1325, 1325, 1329, 1336, 1343, 1343,
    1347, 1347, 1351, 1357, 1358, 1365, 1371, 1374, 1381, 1388, 1389, 1390, 1391, 1392, 1395, 1395,
    1395, 1395, 1395, 1395, 1395, 1397, 1402, 1407, 1412, 1417, 1422, 1427, 1433, 1434, 1435, 1436,
    1437, 1438, 1439, 1440, 1441, 1442, 1447, 1450, 1457, 1460, 1466, 1476, 1481, 1486, 1491, 1496,
    1501, 1506, 1511, 1516};

// Print the state stack on the debug stream.
void PipelineParserGen::yystack_print_() {
    *yycdebug_ << "Stack now";
    for (stack_type::const_iterator i = yystack_.begin(), i_end = yystack_.end(); i != i_end; ++i)
        *yycdebug_ << ' ' << int(i->state);
    *yycdebug_ << '\n';
}

// Report on the debug stream that the rule \a yyrule is going to be reduced.
void PipelineParserGen::yy_reduce_print_(int yyrule) {
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
#line 5388 "src/mongo/db/cst/pipeline_parser_gen.cpp"

#line 1520 "src/mongo/db/cst/pipeline_grammar.yy"
