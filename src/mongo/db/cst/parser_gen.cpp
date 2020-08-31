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


#include "parser_gen.hpp"


// Unqualified %code blocks.
#line 82 "src/mongo/db/cst/grammar.yy"

#include "mongo/db/cst/bson_lexer.h"
#include "mongo/db/cst/c_node_disambiguation.h"
#include "mongo/db/cst/c_node_validation.h"
#include "mongo/db/cst/key_fieldname.h"
#include "mongo/platform/decimal128.h"
#include "mongo/stdx/variant.h"

namespace mongo {
// Mandatory error function.
void ParserGen::error(const ParserGen::location_type& loc, const std::string& msg) {
    uasserted(ErrorCodes::FailedToParse, str::stream() << msg << " at element " << loc);
}
}  // namespace mongo

// Default location for actions, called each time a rule is matched but before the action is
// run. Also called when bison encounters a syntax ambiguity, which should not be relevant for
// mongo.
#define YYLLOC_DEFAULT(newPos, rhsPositions, nRhs)

#line 67 "src/mongo/db/cst/parser_gen.cpp"


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

#line 57 "src/mongo/db/cst/grammar.yy"
namespace mongo {
#line 159 "src/mongo/db/cst/parser_gen.cpp"


/* Return YYSTR after stripping away unnecessary quotes and
   backslashes, so that it's suitable for yyerror.  The heuristic is
   that double-quoting is unnecessary unless the string contains an
   apostrophe, a comma, or backslash (other than backslash-backslash).
   YYSTR is taken from yytname.  */
std::string ParserGen::yytnamerr_(const char* yystr) {
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
ParserGen::ParserGen(BSONLexer& lexer_yyarg, CNode* cst_yyarg)
#if YYDEBUG
    : yydebug_(false),
      yycdebug_(&std::cerr),
#else
    :
#endif
      lexer(lexer_yyarg),
      cst(cst_yyarg) {
}

ParserGen::~ParserGen() {}

ParserGen::syntax_error::~syntax_error() YY_NOEXCEPT YY_NOTHROW {}

/*---------------.
| Symbol types.  |
`---------------*/


// by_state.
ParserGen::by_state::by_state() YY_NOEXCEPT : state(empty_state) {}

ParserGen::by_state::by_state(const by_state& that) YY_NOEXCEPT : state(that.state) {}

void ParserGen::by_state::clear() YY_NOEXCEPT {
    state = empty_state;
}

void ParserGen::by_state::move(by_state& that) {
    state = that.state;
    that.clear();
}

ParserGen::by_state::by_state(state_type s) YY_NOEXCEPT : state(s) {}

ParserGen::symbol_number_type ParserGen::by_state::type_get() const YY_NOEXCEPT {
    if (state == empty_state)
        return empty_symbol;
    else
        return yystos_[+state];
}

ParserGen::stack_symbol_type::stack_symbol_type() {}

ParserGen::stack_symbol_type::stack_symbol_type(YY_RVREF(stack_symbol_type) that)
    : super_type(YY_MOVE(that.state), YY_MOVE(that.location)) {
    switch (that.type_get()) {
        case 114:  // "BinData"
            value.YY_MOVE_OR_COPY<BSONBinData>(YY_MOVE(that.value));
            break;

        case 121:  // "Code"
            value.YY_MOVE_OR_COPY<BSONCode>(YY_MOVE(that.value));
            break;

        case 123:  // "CodeWScope"
            value.YY_MOVE_OR_COPY<BSONCodeWScope>(YY_MOVE(that.value));
            break;

        case 120:  // "dbPointer"
            value.YY_MOVE_OR_COPY<BSONDBRef>(YY_MOVE(that.value));
            break;

        case 119:  // "regex"
            value.YY_MOVE_OR_COPY<BSONRegEx>(YY_MOVE(that.value));
            break;

        case 122:  // "Symbol"
            value.YY_MOVE_OR_COPY<BSONSymbol>(YY_MOVE(that.value));
            break;

        case 148:  // dbPointer
        case 149:  // javascript
        case 150:  // symbol
        case 151:  // javascriptWScope
        case 152:  // int
        case 153:  // timestamp
        case 154:  // long
        case 155:  // double
        case 156:  // decimal
        case 157:  // minKey
        case 158:  // maxKey
        case 159:  // value
        case 160:  // string
        case 161:  // fieldPath
        case 162:  // binary
        case 163:  // undefined
        case 164:  // objectId
        case 165:  // bool
        case 166:  // date
        case 167:  // null
        case 168:  // regex
        case 169:  // simpleValue
        case 170:  // compoundValue
        case 171:  // valueArray
        case 172:  // valueObject
        case 173:  // valueFields
        case 174:  // variable
        case 175:  // pipeline
        case 176:  // stageList
        case 177:  // stage
        case 178:  // inhibitOptimization
        case 179:  // unionWith
        case 180:  // skip
        case 181:  // limit
        case 182:  // project
        case 183:  // sample
        case 184:  // projectFields
        case 185:  // projection
        case 186:  // num
        case 187:  // expression
        case 188:  // compoundExpression
        case 189:  // exprFixedTwoArg
        case 190:  // expressionArray
        case 191:  // expressionObject
        case 192:  // expressionFields
        case 193:  // maths
        case 194:  // add
        case 195:  // atan2
        case 196:  // boolExps
        case 197:  // and
        case 198:  // or
        case 199:  // not
        case 200:  // literalEscapes
        case 201:  // const
        case 202:  // literal
        case 203:  // stringExps
        case 204:  // concat
        case 205:  // dateFromString
        case 206:  // dateToString
        case 207:  // indexOfBytes
        case 208:  // indexOfCP
        case 209:  // ltrim
        case 210:  // regexFind
        case 211:  // regexFindAll
        case 212:  // regexMatch
        case 213:  // regexArgs
        case 214:  // replaceOne
        case 215:  // replaceAll
        case 216:  // rtrim
        case 217:  // split
        case 218:  // strLenBytes
        case 219:  // strLenCP
        case 220:  // strcasecmp
        case 221:  // substr
        case 222:  // substrBytes
        case 223:  // substrCP
        case 224:  // toLower
        case 225:  // toUpper
        case 226:  // trim
        case 227:  // compExprs
        case 228:  // cmp
        case 229:  // eq
        case 230:  // gt
        case 231:  // gte
        case 232:  // lt
        case 233:  // lte
        case 234:  // ne
        case 235:  // typeExpression
        case 236:  // convert
        case 237:  // toBool
        case 238:  // toDate
        case 239:  // toDecimal
        case 240:  // toDouble
        case 241:  // toInt
        case 242:  // toLong
        case 243:  // toObjectId
        case 244:  // toString
        case 245:  // type
        case 246:  // abs
        case 247:  // ceil
        case 248:  // divide
        case 249:  // exponent
        case 250:  // floor
        case 251:  // ln
        case 252:  // log
        case 253:  // logten
        case 254:  // mod
        case 255:  // multiply
        case 256:  // pow
        case 257:  // round
        case 258:  // sqrt
        case 259:  // subtract
        case 260:  // trunc
        case 270:  // matchExpression
        case 271:  // filterFields
        case 272:  // filterVal
        case 273:  // sortSpecs
        case 274:  // specList
        case 275:  // metaSort
        case 276:  // oneOrNegOne
        case 277:  // metaSortKeyword
            value.YY_MOVE_OR_COPY<CNode>(YY_MOVE(that.value));
            break;

        case 135:  // projectionFieldname
        case 136:  // expressionFieldname
        case 137:  // stageAsUserFieldname
        case 138:  // filterFieldname
        case 139:  // argAsUserFieldname
        case 140:  // aggExprAsUserFieldname
        case 141:  // invariableUserFieldname
        case 142:  // idAsUserFieldname
        case 143:  // valueFieldname
            value.YY_MOVE_OR_COPY<CNode::Fieldname>(YY_MOVE(that.value));
            break;

        case 117:  // "Date"
            value.YY_MOVE_OR_COPY<Date_t>(YY_MOVE(that.value));
            break;

        case 127:  // "arbitrary decimal"
            value.YY_MOVE_OR_COPY<Decimal128>(YY_MOVE(that.value));
            break;

        case 116:  // "ObjectID"
            value.YY_MOVE_OR_COPY<OID>(YY_MOVE(that.value));
            break;

        case 128:  // "Timestamp"
            value.YY_MOVE_OR_COPY<Timestamp>(YY_MOVE(that.value));
            break;

        case 130:  // "maxKey"
            value.YY_MOVE_OR_COPY<UserMaxKey>(YY_MOVE(that.value));
            break;

        case 129:  // "minKey"
            value.YY_MOVE_OR_COPY<UserMinKey>(YY_MOVE(that.value));
            break;

        case 118:  // "null"
            value.YY_MOVE_OR_COPY<UserNull>(YY_MOVE(that.value));
            break;

        case 115:  // "undefined"
            value.YY_MOVE_OR_COPY<UserUndefined>(YY_MOVE(that.value));
            break;

        case 126:  // "arbitrary double"
            value.YY_MOVE_OR_COPY<double>(YY_MOVE(that.value));
            break;

        case 124:  // "arbitrary integer"
            value.YY_MOVE_OR_COPY<int>(YY_MOVE(that.value));
            break;

        case 125:  // "arbitrary long"
            value.YY_MOVE_OR_COPY<long long>(YY_MOVE(that.value));
            break;

        case 144:  // projectField
        case 145:  // expressionField
        case 146:  // valueField
        case 147:  // filterField
        case 261:  // onErrorArg
        case 262:  // onNullArg
        case 263:  // formatArg
        case 264:  // timezoneArg
        case 265:  // charsArg
        case 266:  // optionsArg
        case 278:  // sortSpec
            value.YY_MOVE_OR_COPY<std::pair<CNode::Fieldname, CNode>>(YY_MOVE(that.value));
            break;

        case 112:  // "fieldname"
        case 113:  // "string"
        case 131:  // "$-prefixed string"
        case 132:  // "$$-prefixed string"
        case 133:  // "$-prefixed fieldname"
            value.YY_MOVE_OR_COPY<std::string>(YY_MOVE(that.value));
            break;

        case 267:  // expressions
        case 268:  // values
        case 269:  // exprZeroToTwo
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

ParserGen::stack_symbol_type::stack_symbol_type(state_type s, YY_MOVE_REF(symbol_type) that)
    : super_type(s, YY_MOVE(that.location)) {
    switch (that.type_get()) {
        case 114:  // "BinData"
            value.move<BSONBinData>(YY_MOVE(that.value));
            break;

        case 121:  // "Code"
            value.move<BSONCode>(YY_MOVE(that.value));
            break;

        case 123:  // "CodeWScope"
            value.move<BSONCodeWScope>(YY_MOVE(that.value));
            break;

        case 120:  // "dbPointer"
            value.move<BSONDBRef>(YY_MOVE(that.value));
            break;

        case 119:  // "regex"
            value.move<BSONRegEx>(YY_MOVE(that.value));
            break;

        case 122:  // "Symbol"
            value.move<BSONSymbol>(YY_MOVE(that.value));
            break;

        case 148:  // dbPointer
        case 149:  // javascript
        case 150:  // symbol
        case 151:  // javascriptWScope
        case 152:  // int
        case 153:  // timestamp
        case 154:  // long
        case 155:  // double
        case 156:  // decimal
        case 157:  // minKey
        case 158:  // maxKey
        case 159:  // value
        case 160:  // string
        case 161:  // fieldPath
        case 162:  // binary
        case 163:  // undefined
        case 164:  // objectId
        case 165:  // bool
        case 166:  // date
        case 167:  // null
        case 168:  // regex
        case 169:  // simpleValue
        case 170:  // compoundValue
        case 171:  // valueArray
        case 172:  // valueObject
        case 173:  // valueFields
        case 174:  // variable
        case 175:  // pipeline
        case 176:  // stageList
        case 177:  // stage
        case 178:  // inhibitOptimization
        case 179:  // unionWith
        case 180:  // skip
        case 181:  // limit
        case 182:  // project
        case 183:  // sample
        case 184:  // projectFields
        case 185:  // projection
        case 186:  // num
        case 187:  // expression
        case 188:  // compoundExpression
        case 189:  // exprFixedTwoArg
        case 190:  // expressionArray
        case 191:  // expressionObject
        case 192:  // expressionFields
        case 193:  // maths
        case 194:  // add
        case 195:  // atan2
        case 196:  // boolExps
        case 197:  // and
        case 198:  // or
        case 199:  // not
        case 200:  // literalEscapes
        case 201:  // const
        case 202:  // literal
        case 203:  // stringExps
        case 204:  // concat
        case 205:  // dateFromString
        case 206:  // dateToString
        case 207:  // indexOfBytes
        case 208:  // indexOfCP
        case 209:  // ltrim
        case 210:  // regexFind
        case 211:  // regexFindAll
        case 212:  // regexMatch
        case 213:  // regexArgs
        case 214:  // replaceOne
        case 215:  // replaceAll
        case 216:  // rtrim
        case 217:  // split
        case 218:  // strLenBytes
        case 219:  // strLenCP
        case 220:  // strcasecmp
        case 221:  // substr
        case 222:  // substrBytes
        case 223:  // substrCP
        case 224:  // toLower
        case 225:  // toUpper
        case 226:  // trim
        case 227:  // compExprs
        case 228:  // cmp
        case 229:  // eq
        case 230:  // gt
        case 231:  // gte
        case 232:  // lt
        case 233:  // lte
        case 234:  // ne
        case 235:  // typeExpression
        case 236:  // convert
        case 237:  // toBool
        case 238:  // toDate
        case 239:  // toDecimal
        case 240:  // toDouble
        case 241:  // toInt
        case 242:  // toLong
        case 243:  // toObjectId
        case 244:  // toString
        case 245:  // type
        case 246:  // abs
        case 247:  // ceil
        case 248:  // divide
        case 249:  // exponent
        case 250:  // floor
        case 251:  // ln
        case 252:  // log
        case 253:  // logten
        case 254:  // mod
        case 255:  // multiply
        case 256:  // pow
        case 257:  // round
        case 258:  // sqrt
        case 259:  // subtract
        case 260:  // trunc
        case 270:  // matchExpression
        case 271:  // filterFields
        case 272:  // filterVal
        case 273:  // sortSpecs
        case 274:  // specList
        case 275:  // metaSort
        case 276:  // oneOrNegOne
        case 277:  // metaSortKeyword
            value.move<CNode>(YY_MOVE(that.value));
            break;

        case 135:  // projectionFieldname
        case 136:  // expressionFieldname
        case 137:  // stageAsUserFieldname
        case 138:  // filterFieldname
        case 139:  // argAsUserFieldname
        case 140:  // aggExprAsUserFieldname
        case 141:  // invariableUserFieldname
        case 142:  // idAsUserFieldname
        case 143:  // valueFieldname
            value.move<CNode::Fieldname>(YY_MOVE(that.value));
            break;

        case 117:  // "Date"
            value.move<Date_t>(YY_MOVE(that.value));
            break;

        case 127:  // "arbitrary decimal"
            value.move<Decimal128>(YY_MOVE(that.value));
            break;

        case 116:  // "ObjectID"
            value.move<OID>(YY_MOVE(that.value));
            break;

        case 128:  // "Timestamp"
            value.move<Timestamp>(YY_MOVE(that.value));
            break;

        case 130:  // "maxKey"
            value.move<UserMaxKey>(YY_MOVE(that.value));
            break;

        case 129:  // "minKey"
            value.move<UserMinKey>(YY_MOVE(that.value));
            break;

        case 118:  // "null"
            value.move<UserNull>(YY_MOVE(that.value));
            break;

        case 115:  // "undefined"
            value.move<UserUndefined>(YY_MOVE(that.value));
            break;

        case 126:  // "arbitrary double"
            value.move<double>(YY_MOVE(that.value));
            break;

        case 124:  // "arbitrary integer"
            value.move<int>(YY_MOVE(that.value));
            break;

        case 125:  // "arbitrary long"
            value.move<long long>(YY_MOVE(that.value));
            break;

        case 144:  // projectField
        case 145:  // expressionField
        case 146:  // valueField
        case 147:  // filterField
        case 261:  // onErrorArg
        case 262:  // onNullArg
        case 263:  // formatArg
        case 264:  // timezoneArg
        case 265:  // charsArg
        case 266:  // optionsArg
        case 278:  // sortSpec
            value.move<std::pair<CNode::Fieldname, CNode>>(YY_MOVE(that.value));
            break;

        case 112:  // "fieldname"
        case 113:  // "string"
        case 131:  // "$-prefixed string"
        case 132:  // "$$-prefixed string"
        case 133:  // "$-prefixed fieldname"
            value.move<std::string>(YY_MOVE(that.value));
            break;

        case 267:  // expressions
        case 268:  // values
        case 269:  // exprZeroToTwo
            value.move<std::vector<CNode>>(YY_MOVE(that.value));
            break;

        default:
            break;
    }

    // that is emptied.
    that.type = empty_symbol;
}

#if YY_CPLUSPLUS < 201103L
ParserGen::stack_symbol_type& ParserGen::stack_symbol_type::operator=(
    const stack_symbol_type& that) {
    state = that.state;
    switch (that.type_get()) {
        case 114:  // "BinData"
            value.copy<BSONBinData>(that.value);
            break;

        case 121:  // "Code"
            value.copy<BSONCode>(that.value);
            break;

        case 123:  // "CodeWScope"
            value.copy<BSONCodeWScope>(that.value);
            break;

        case 120:  // "dbPointer"
            value.copy<BSONDBRef>(that.value);
            break;

        case 119:  // "regex"
            value.copy<BSONRegEx>(that.value);
            break;

        case 122:  // "Symbol"
            value.copy<BSONSymbol>(that.value);
            break;

        case 148:  // dbPointer
        case 149:  // javascript
        case 150:  // symbol
        case 151:  // javascriptWScope
        case 152:  // int
        case 153:  // timestamp
        case 154:  // long
        case 155:  // double
        case 156:  // decimal
        case 157:  // minKey
        case 158:  // maxKey
        case 159:  // value
        case 160:  // string
        case 161:  // fieldPath
        case 162:  // binary
        case 163:  // undefined
        case 164:  // objectId
        case 165:  // bool
        case 166:  // date
        case 167:  // null
        case 168:  // regex
        case 169:  // simpleValue
        case 170:  // compoundValue
        case 171:  // valueArray
        case 172:  // valueObject
        case 173:  // valueFields
        case 174:  // variable
        case 175:  // pipeline
        case 176:  // stageList
        case 177:  // stage
        case 178:  // inhibitOptimization
        case 179:  // unionWith
        case 180:  // skip
        case 181:  // limit
        case 182:  // project
        case 183:  // sample
        case 184:  // projectFields
        case 185:  // projection
        case 186:  // num
        case 187:  // expression
        case 188:  // compoundExpression
        case 189:  // exprFixedTwoArg
        case 190:  // expressionArray
        case 191:  // expressionObject
        case 192:  // expressionFields
        case 193:  // maths
        case 194:  // add
        case 195:  // atan2
        case 196:  // boolExps
        case 197:  // and
        case 198:  // or
        case 199:  // not
        case 200:  // literalEscapes
        case 201:  // const
        case 202:  // literal
        case 203:  // stringExps
        case 204:  // concat
        case 205:  // dateFromString
        case 206:  // dateToString
        case 207:  // indexOfBytes
        case 208:  // indexOfCP
        case 209:  // ltrim
        case 210:  // regexFind
        case 211:  // regexFindAll
        case 212:  // regexMatch
        case 213:  // regexArgs
        case 214:  // replaceOne
        case 215:  // replaceAll
        case 216:  // rtrim
        case 217:  // split
        case 218:  // strLenBytes
        case 219:  // strLenCP
        case 220:  // strcasecmp
        case 221:  // substr
        case 222:  // substrBytes
        case 223:  // substrCP
        case 224:  // toLower
        case 225:  // toUpper
        case 226:  // trim
        case 227:  // compExprs
        case 228:  // cmp
        case 229:  // eq
        case 230:  // gt
        case 231:  // gte
        case 232:  // lt
        case 233:  // lte
        case 234:  // ne
        case 235:  // typeExpression
        case 236:  // convert
        case 237:  // toBool
        case 238:  // toDate
        case 239:  // toDecimal
        case 240:  // toDouble
        case 241:  // toInt
        case 242:  // toLong
        case 243:  // toObjectId
        case 244:  // toString
        case 245:  // type
        case 246:  // abs
        case 247:  // ceil
        case 248:  // divide
        case 249:  // exponent
        case 250:  // floor
        case 251:  // ln
        case 252:  // log
        case 253:  // logten
        case 254:  // mod
        case 255:  // multiply
        case 256:  // pow
        case 257:  // round
        case 258:  // sqrt
        case 259:  // subtract
        case 260:  // trunc
        case 270:  // matchExpression
        case 271:  // filterFields
        case 272:  // filterVal
        case 273:  // sortSpecs
        case 274:  // specList
        case 275:  // metaSort
        case 276:  // oneOrNegOne
        case 277:  // metaSortKeyword
            value.copy<CNode>(that.value);
            break;

        case 135:  // projectionFieldname
        case 136:  // expressionFieldname
        case 137:  // stageAsUserFieldname
        case 138:  // filterFieldname
        case 139:  // argAsUserFieldname
        case 140:  // aggExprAsUserFieldname
        case 141:  // invariableUserFieldname
        case 142:  // idAsUserFieldname
        case 143:  // valueFieldname
            value.copy<CNode::Fieldname>(that.value);
            break;

        case 117:  // "Date"
            value.copy<Date_t>(that.value);
            break;

        case 127:  // "arbitrary decimal"
            value.copy<Decimal128>(that.value);
            break;

        case 116:  // "ObjectID"
            value.copy<OID>(that.value);
            break;

        case 128:  // "Timestamp"
            value.copy<Timestamp>(that.value);
            break;

        case 130:  // "maxKey"
            value.copy<UserMaxKey>(that.value);
            break;

        case 129:  // "minKey"
            value.copy<UserMinKey>(that.value);
            break;

        case 118:  // "null"
            value.copy<UserNull>(that.value);
            break;

        case 115:  // "undefined"
            value.copy<UserUndefined>(that.value);
            break;

        case 126:  // "arbitrary double"
            value.copy<double>(that.value);
            break;

        case 124:  // "arbitrary integer"
            value.copy<int>(that.value);
            break;

        case 125:  // "arbitrary long"
            value.copy<long long>(that.value);
            break;

        case 144:  // projectField
        case 145:  // expressionField
        case 146:  // valueField
        case 147:  // filterField
        case 261:  // onErrorArg
        case 262:  // onNullArg
        case 263:  // formatArg
        case 264:  // timezoneArg
        case 265:  // charsArg
        case 266:  // optionsArg
        case 278:  // sortSpec
            value.copy<std::pair<CNode::Fieldname, CNode>>(that.value);
            break;

        case 112:  // "fieldname"
        case 113:  // "string"
        case 131:  // "$-prefixed string"
        case 132:  // "$$-prefixed string"
        case 133:  // "$-prefixed fieldname"
            value.copy<std::string>(that.value);
            break;

        case 267:  // expressions
        case 268:  // values
        case 269:  // exprZeroToTwo
            value.copy<std::vector<CNode>>(that.value);
            break;

        default:
            break;
    }

    location = that.location;
    return *this;
}

ParserGen::stack_symbol_type& ParserGen::stack_symbol_type::operator=(stack_symbol_type& that) {
    state = that.state;
    switch (that.type_get()) {
        case 114:  // "BinData"
            value.move<BSONBinData>(that.value);
            break;

        case 121:  // "Code"
            value.move<BSONCode>(that.value);
            break;

        case 123:  // "CodeWScope"
            value.move<BSONCodeWScope>(that.value);
            break;

        case 120:  // "dbPointer"
            value.move<BSONDBRef>(that.value);
            break;

        case 119:  // "regex"
            value.move<BSONRegEx>(that.value);
            break;

        case 122:  // "Symbol"
            value.move<BSONSymbol>(that.value);
            break;

        case 148:  // dbPointer
        case 149:  // javascript
        case 150:  // symbol
        case 151:  // javascriptWScope
        case 152:  // int
        case 153:  // timestamp
        case 154:  // long
        case 155:  // double
        case 156:  // decimal
        case 157:  // minKey
        case 158:  // maxKey
        case 159:  // value
        case 160:  // string
        case 161:  // fieldPath
        case 162:  // binary
        case 163:  // undefined
        case 164:  // objectId
        case 165:  // bool
        case 166:  // date
        case 167:  // null
        case 168:  // regex
        case 169:  // simpleValue
        case 170:  // compoundValue
        case 171:  // valueArray
        case 172:  // valueObject
        case 173:  // valueFields
        case 174:  // variable
        case 175:  // pipeline
        case 176:  // stageList
        case 177:  // stage
        case 178:  // inhibitOptimization
        case 179:  // unionWith
        case 180:  // skip
        case 181:  // limit
        case 182:  // project
        case 183:  // sample
        case 184:  // projectFields
        case 185:  // projection
        case 186:  // num
        case 187:  // expression
        case 188:  // compoundExpression
        case 189:  // exprFixedTwoArg
        case 190:  // expressionArray
        case 191:  // expressionObject
        case 192:  // expressionFields
        case 193:  // maths
        case 194:  // add
        case 195:  // atan2
        case 196:  // boolExps
        case 197:  // and
        case 198:  // or
        case 199:  // not
        case 200:  // literalEscapes
        case 201:  // const
        case 202:  // literal
        case 203:  // stringExps
        case 204:  // concat
        case 205:  // dateFromString
        case 206:  // dateToString
        case 207:  // indexOfBytes
        case 208:  // indexOfCP
        case 209:  // ltrim
        case 210:  // regexFind
        case 211:  // regexFindAll
        case 212:  // regexMatch
        case 213:  // regexArgs
        case 214:  // replaceOne
        case 215:  // replaceAll
        case 216:  // rtrim
        case 217:  // split
        case 218:  // strLenBytes
        case 219:  // strLenCP
        case 220:  // strcasecmp
        case 221:  // substr
        case 222:  // substrBytes
        case 223:  // substrCP
        case 224:  // toLower
        case 225:  // toUpper
        case 226:  // trim
        case 227:  // compExprs
        case 228:  // cmp
        case 229:  // eq
        case 230:  // gt
        case 231:  // gte
        case 232:  // lt
        case 233:  // lte
        case 234:  // ne
        case 235:  // typeExpression
        case 236:  // convert
        case 237:  // toBool
        case 238:  // toDate
        case 239:  // toDecimal
        case 240:  // toDouble
        case 241:  // toInt
        case 242:  // toLong
        case 243:  // toObjectId
        case 244:  // toString
        case 245:  // type
        case 246:  // abs
        case 247:  // ceil
        case 248:  // divide
        case 249:  // exponent
        case 250:  // floor
        case 251:  // ln
        case 252:  // log
        case 253:  // logten
        case 254:  // mod
        case 255:  // multiply
        case 256:  // pow
        case 257:  // round
        case 258:  // sqrt
        case 259:  // subtract
        case 260:  // trunc
        case 270:  // matchExpression
        case 271:  // filterFields
        case 272:  // filterVal
        case 273:  // sortSpecs
        case 274:  // specList
        case 275:  // metaSort
        case 276:  // oneOrNegOne
        case 277:  // metaSortKeyword
            value.move<CNode>(that.value);
            break;

        case 135:  // projectionFieldname
        case 136:  // expressionFieldname
        case 137:  // stageAsUserFieldname
        case 138:  // filterFieldname
        case 139:  // argAsUserFieldname
        case 140:  // aggExprAsUserFieldname
        case 141:  // invariableUserFieldname
        case 142:  // idAsUserFieldname
        case 143:  // valueFieldname
            value.move<CNode::Fieldname>(that.value);
            break;

        case 117:  // "Date"
            value.move<Date_t>(that.value);
            break;

        case 127:  // "arbitrary decimal"
            value.move<Decimal128>(that.value);
            break;

        case 116:  // "ObjectID"
            value.move<OID>(that.value);
            break;

        case 128:  // "Timestamp"
            value.move<Timestamp>(that.value);
            break;

        case 130:  // "maxKey"
            value.move<UserMaxKey>(that.value);
            break;

        case 129:  // "minKey"
            value.move<UserMinKey>(that.value);
            break;

        case 118:  // "null"
            value.move<UserNull>(that.value);
            break;

        case 115:  // "undefined"
            value.move<UserUndefined>(that.value);
            break;

        case 126:  // "arbitrary double"
            value.move<double>(that.value);
            break;

        case 124:  // "arbitrary integer"
            value.move<int>(that.value);
            break;

        case 125:  // "arbitrary long"
            value.move<long long>(that.value);
            break;

        case 144:  // projectField
        case 145:  // expressionField
        case 146:  // valueField
        case 147:  // filterField
        case 261:  // onErrorArg
        case 262:  // onNullArg
        case 263:  // formatArg
        case 264:  // timezoneArg
        case 265:  // charsArg
        case 266:  // optionsArg
        case 278:  // sortSpec
            value.move<std::pair<CNode::Fieldname, CNode>>(that.value);
            break;

        case 112:  // "fieldname"
        case 113:  // "string"
        case 131:  // "$-prefixed string"
        case 132:  // "$$-prefixed string"
        case 133:  // "$-prefixed fieldname"
            value.move<std::string>(that.value);
            break;

        case 267:  // expressions
        case 268:  // values
        case 269:  // exprZeroToTwo
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
void ParserGen::yy_destroy_(const char* yymsg, basic_symbol<Base>& yysym) const {
    if (yymsg)
        YY_SYMBOL_PRINT(yymsg, yysym);
}

#if YYDEBUG
template <typename Base>
void ParserGen::yy_print_(std::ostream& yyo, const basic_symbol<Base>& yysym) const {
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

void ParserGen::yypush_(const char* m, YY_MOVE_REF(stack_symbol_type) sym) {
    if (m)
        YY_SYMBOL_PRINT(m, sym);
    yystack_.push(YY_MOVE(sym));
}

void ParserGen::yypush_(const char* m, state_type s, YY_MOVE_REF(symbol_type) sym) {
#if 201103L <= YY_CPLUSPLUS
    yypush_(m, stack_symbol_type(s, std::move(sym)));
#else
    stack_symbol_type ss(s, sym);
    yypush_(m, ss);
#endif
}

void ParserGen::yypop_(int n) {
    yystack_.pop(n);
}

#if YYDEBUG
std::ostream& ParserGen::debug_stream() const {
    return *yycdebug_;
}

void ParserGen::set_debug_stream(std::ostream& o) {
    yycdebug_ = &o;
}


ParserGen::debug_level_type ParserGen::debug_level() const {
    return yydebug_;
}

void ParserGen::set_debug_level(debug_level_type l) {
    yydebug_ = l;
}
#endif  // YYDEBUG

ParserGen::state_type ParserGen::yy_lr_goto_state_(state_type yystate, int yysym) {
    int yyr = yypgoto_[yysym - yyntokens_] + yystate;
    if (0 <= yyr && yyr <= yylast_ && yycheck_[yyr] == yystate)
        return yytable_[yyr];
    else
        return yydefgoto_[yysym - yyntokens_];
}

bool ParserGen::yy_pact_value_is_default_(int yyvalue) {
    return yyvalue == yypact_ninf_;
}

bool ParserGen::yy_table_value_is_error_(int yyvalue) {
    return yyvalue == yytable_ninf_;
}

int ParserGen::operator()() {
    return parse();
}

int ParserGen::parse() {
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
                case 114:  // "BinData"
                    yylhs.value.emplace<BSONBinData>();
                    break;

                case 121:  // "Code"
                    yylhs.value.emplace<BSONCode>();
                    break;

                case 123:  // "CodeWScope"
                    yylhs.value.emplace<BSONCodeWScope>();
                    break;

                case 120:  // "dbPointer"
                    yylhs.value.emplace<BSONDBRef>();
                    break;

                case 119:  // "regex"
                    yylhs.value.emplace<BSONRegEx>();
                    break;

                case 122:  // "Symbol"
                    yylhs.value.emplace<BSONSymbol>();
                    break;

                case 148:  // dbPointer
                case 149:  // javascript
                case 150:  // symbol
                case 151:  // javascriptWScope
                case 152:  // int
                case 153:  // timestamp
                case 154:  // long
                case 155:  // double
                case 156:  // decimal
                case 157:  // minKey
                case 158:  // maxKey
                case 159:  // value
                case 160:  // string
                case 161:  // fieldPath
                case 162:  // binary
                case 163:  // undefined
                case 164:  // objectId
                case 165:  // bool
                case 166:  // date
                case 167:  // null
                case 168:  // regex
                case 169:  // simpleValue
                case 170:  // compoundValue
                case 171:  // valueArray
                case 172:  // valueObject
                case 173:  // valueFields
                case 174:  // variable
                case 175:  // pipeline
                case 176:  // stageList
                case 177:  // stage
                case 178:  // inhibitOptimization
                case 179:  // unionWith
                case 180:  // skip
                case 181:  // limit
                case 182:  // project
                case 183:  // sample
                case 184:  // projectFields
                case 185:  // projection
                case 186:  // num
                case 187:  // expression
                case 188:  // compoundExpression
                case 189:  // exprFixedTwoArg
                case 190:  // expressionArray
                case 191:  // expressionObject
                case 192:  // expressionFields
                case 193:  // maths
                case 194:  // add
                case 195:  // atan2
                case 196:  // boolExps
                case 197:  // and
                case 198:  // or
                case 199:  // not
                case 200:  // literalEscapes
                case 201:  // const
                case 202:  // literal
                case 203:  // stringExps
                case 204:  // concat
                case 205:  // dateFromString
                case 206:  // dateToString
                case 207:  // indexOfBytes
                case 208:  // indexOfCP
                case 209:  // ltrim
                case 210:  // regexFind
                case 211:  // regexFindAll
                case 212:  // regexMatch
                case 213:  // regexArgs
                case 214:  // replaceOne
                case 215:  // replaceAll
                case 216:  // rtrim
                case 217:  // split
                case 218:  // strLenBytes
                case 219:  // strLenCP
                case 220:  // strcasecmp
                case 221:  // substr
                case 222:  // substrBytes
                case 223:  // substrCP
                case 224:  // toLower
                case 225:  // toUpper
                case 226:  // trim
                case 227:  // compExprs
                case 228:  // cmp
                case 229:  // eq
                case 230:  // gt
                case 231:  // gte
                case 232:  // lt
                case 233:  // lte
                case 234:  // ne
                case 235:  // typeExpression
                case 236:  // convert
                case 237:  // toBool
                case 238:  // toDate
                case 239:  // toDecimal
                case 240:  // toDouble
                case 241:  // toInt
                case 242:  // toLong
                case 243:  // toObjectId
                case 244:  // toString
                case 245:  // type
                case 246:  // abs
                case 247:  // ceil
                case 248:  // divide
                case 249:  // exponent
                case 250:  // floor
                case 251:  // ln
                case 252:  // log
                case 253:  // logten
                case 254:  // mod
                case 255:  // multiply
                case 256:  // pow
                case 257:  // round
                case 258:  // sqrt
                case 259:  // subtract
                case 260:  // trunc
                case 270:  // matchExpression
                case 271:  // filterFields
                case 272:  // filterVal
                case 273:  // sortSpecs
                case 274:  // specList
                case 275:  // metaSort
                case 276:  // oneOrNegOne
                case 277:  // metaSortKeyword
                    yylhs.value.emplace<CNode>();
                    break;

                case 135:  // projectionFieldname
                case 136:  // expressionFieldname
                case 137:  // stageAsUserFieldname
                case 138:  // filterFieldname
                case 139:  // argAsUserFieldname
                case 140:  // aggExprAsUserFieldname
                case 141:  // invariableUserFieldname
                case 142:  // idAsUserFieldname
                case 143:  // valueFieldname
                    yylhs.value.emplace<CNode::Fieldname>();
                    break;

                case 117:  // "Date"
                    yylhs.value.emplace<Date_t>();
                    break;

                case 127:  // "arbitrary decimal"
                    yylhs.value.emplace<Decimal128>();
                    break;

                case 116:  // "ObjectID"
                    yylhs.value.emplace<OID>();
                    break;

                case 128:  // "Timestamp"
                    yylhs.value.emplace<Timestamp>();
                    break;

                case 130:  // "maxKey"
                    yylhs.value.emplace<UserMaxKey>();
                    break;

                case 129:  // "minKey"
                    yylhs.value.emplace<UserMinKey>();
                    break;

                case 118:  // "null"
                    yylhs.value.emplace<UserNull>();
                    break;

                case 115:  // "undefined"
                    yylhs.value.emplace<UserUndefined>();
                    break;

                case 126:  // "arbitrary double"
                    yylhs.value.emplace<double>();
                    break;

                case 124:  // "arbitrary integer"
                    yylhs.value.emplace<int>();
                    break;

                case 125:  // "arbitrary long"
                    yylhs.value.emplace<long long>();
                    break;

                case 144:  // projectField
                case 145:  // expressionField
                case 146:  // valueField
                case 147:  // filterField
                case 261:  // onErrorArg
                case 262:  // onNullArg
                case 263:  // formatArg
                case 264:  // timezoneArg
                case 265:  // charsArg
                case 266:  // optionsArg
                case 278:  // sortSpec
                    yylhs.value.emplace<std::pair<CNode::Fieldname, CNode>>();
                    break;

                case 112:  // "fieldname"
                case 113:  // "string"
                case 131:  // "$-prefixed string"
                case 132:  // "$$-prefixed string"
                case 133:  // "$-prefixed fieldname"
                    yylhs.value.emplace<std::string>();
                    break;

                case 267:  // expressions
                case 268:  // values
                case 269:  // exprZeroToTwo
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
#line 299 "src/mongo/db/cst/grammar.yy"
                    {
                        *cst = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 1749 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 3:
#line 302 "src/mongo/db/cst/grammar.yy"
                    {
                        *cst = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 1757 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 4:
#line 305 "src/mongo/db/cst/grammar.yy"
                    {
                        *cst = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 1765 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 5:
#line 308 "src/mongo/db/cst/grammar.yy"
                    {
                        *cst = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 1773 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 6:
#line 311 "src/mongo/db/cst/grammar.yy"
                    {
                        *cst = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 1781 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 7:
#line 318 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[1].value.as<CNode>());
                    }
#line 1789 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 8:
#line 324 "src/mongo/db/cst/grammar.yy"
                    {
                    }
#line 1795 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 9:
#line 325 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() =
                            CNode{CNode::ArrayChildren{YY_MOVE(yystack_[2].value.as<CNode>())}};
                    }
#line 1803 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 10:
#line 333 "src/mongo/db/cst/grammar.yy"
                    {
                        lexer.sortObjTokens();
                    }
#line 1809 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 12:
#line 336 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 1815 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 13:
#line 336 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 1821 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 14:
#line 336 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 1827 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 15:
#line 336 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 1833 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 16:
#line 336 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 1839 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 17:
#line 336 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 1845 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 18:
#line 339 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{std::pair{
                            KeyFieldname::sample,
                            CNode{CNode::ObjectChildren{
                                {KeyFieldname::sizeArg, YY_MOVE(yystack_[1].value.as<CNode>())},
                            }}}}};
                    }
#line 1857 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 19:
#line 349 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            std::pair{KeyFieldname::inhibitOptimization, CNode::noopLeaf()}}};
                    }
#line 1865 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 20:
#line 355 "src/mongo/db/cst/grammar.yy"
                    {
                        auto pipeline = YY_MOVE(yystack_[1].value.as<CNode>());
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{std::pair{
                            KeyFieldname::unionWith,
                            CNode{CNode::ObjectChildren{
                                {KeyFieldname::collArg, YY_MOVE(yystack_[3].value.as<CNode>())},
                                {KeyFieldname::pipelineArg, std::move(pipeline)}}}}}};
                    }
#line 1878 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 21:
#line 365 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 1884 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 22:
#line 365 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 1890 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 23:
#line 365 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 1896 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 24:
#line 365 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 1902 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 25:
#line 369 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            std::pair{KeyFieldname::skip, YY_MOVE(yystack_[0].value.as<CNode>())}}};
                    }
#line 1910 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 26:
#line 374 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{std::pair{
                            KeyFieldname::limit, YY_MOVE(yystack_[0].value.as<CNode>())}}};
                    }
#line 1918 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 27:
#line 379 "src/mongo/db/cst/grammar.yy"
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
                            // Pass the location of the $project token to the error reporting
                            // function.
                            error(yystack_[3].location, inclusion.getStatus().reason());
                    }
#line 1936 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 28:
#line 395 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode::noopLeaf();
                    }
#line 1944 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 29:
#line 398 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[1].value.as<CNode>());
                        yylhs.value.as<CNode>().objectChildren().emplace_back(
                            YY_MOVE(yystack_[0].value.as<std::pair<CNode::Fieldname, CNode>>()));
                    }
#line 1953 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 30:
#line 405 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<std::pair<CNode::Fieldname, CNode>>() = {
                            KeyFieldname::id, YY_MOVE(yystack_[0].value.as<CNode>())};
                    }
#line 1961 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 31:
#line 408 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<std::pair<CNode::Fieldname, CNode>>() = {
                            YY_MOVE(yystack_[1].value.as<CNode::Fieldname>()),
                            YY_MOVE(yystack_[0].value.as<CNode>())};
                    }
#line 1969 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 32:
#line 414 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 1975 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 33:
#line 415 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 1981 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 34:
#line 416 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 1987 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 35:
#line 417 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 1993 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 36:
#line 418 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 1999 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 37:
#line 419 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 2005 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 38:
#line 420 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 2011 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 39:
#line 421 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 2017 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 40:
#line 422 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 2023 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 41:
#line 423 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 2029 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 42:
#line 424 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 2035 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 43:
#line 425 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{NonZeroKey{1}};
                    }
#line 2043 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 44:
#line 428 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{NonZeroKey{-1}};
                    }
#line 2051 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 45:
#line 431 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() =
                            CNode{NonZeroKey{YY_MOVE(yystack_[0].value.as<int>())}};
                    }
#line 2059 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 46:
#line 434 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{KeyValue::intZeroKey};
                    }
#line 2067 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 47:
#line 437 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{NonZeroKey{1ll}};
                    }
#line 2075 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 48:
#line 440 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{NonZeroKey{-1ll}};
                    }
#line 2083 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 49:
#line 443 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() =
                            CNode{NonZeroKey{YY_MOVE(yystack_[0].value.as<long long>())}};
                    }
#line 2091 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 50:
#line 446 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{KeyValue::longZeroKey};
                    }
#line 2099 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 51:
#line 449 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{NonZeroKey{1.0}};
                    }
#line 2107 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 52:
#line 452 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{NonZeroKey{-1.0}};
                    }
#line 2115 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 53:
#line 455 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() =
                            CNode{NonZeroKey{YY_MOVE(yystack_[0].value.as<double>())}};
                    }
#line 2123 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 54:
#line 458 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{KeyValue::doubleZeroKey};
                    }
#line 2131 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 55:
#line 461 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{NonZeroKey{1.0}};
                    }
#line 2139 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 56:
#line 464 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{NonZeroKey{-1.0}};
                    }
#line 2147 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 57:
#line 467 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() =
                            CNode{NonZeroKey{YY_MOVE(yystack_[0].value.as<Decimal128>())}};
                    }
#line 2155 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 58:
#line 470 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{KeyValue::decimalZeroKey};
                    }
#line 2163 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 59:
#line 473 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{KeyValue::trueKey};
                    }
#line 2171 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 60:
#line 476 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{KeyValue::falseKey};
                    }
#line 2179 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 61:
#line 479 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 2185 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 62:
#line 480 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 2191 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 63:
#line 481 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 2197 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 64:
#line 482 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() =
                            c_node_disambiguation::disambiguateCompoundProjection(
                                YY_MOVE(yystack_[0].value.as<CNode>()));
                        if (stdx::holds_alternative<CompoundInconsistentKey>(
                                yylhs.value.as<CNode>().payload))
                            // TODO SERVER-50498: error() instead of uasserting
                            uasserted(ErrorCodes::FailedToParse,
                                      "object project field cannot contain both inclusion and "
                                      "exclusion indicators");
                    }
#line 2208 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 65:
#line 491 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() =
                            YY_MOVE(yystack_[0].value.as<CNode::Fieldname>());
                    }
#line 2214 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 66:
#line 491 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() =
                            YY_MOVE(yystack_[0].value.as<CNode::Fieldname>());
                    }
#line 2220 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 67:
#line 491 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() =
                            YY_MOVE(yystack_[0].value.as<CNode::Fieldname>());
                    }
#line 2226 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 68:
#line 491 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() =
                            YY_MOVE(yystack_[0].value.as<CNode::Fieldname>());
                    }
#line 2232 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 69:
#line 495 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[1].value.as<CNode>());
                    }
#line 2240 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 70:
#line 501 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode::noopLeaf();
                    }
#line 2248 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 71:
#line 504 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[1].value.as<CNode>());
                        yylhs.value.as<CNode>().objectChildren().emplace_back(
                            YY_MOVE(yystack_[0].value.as<std::pair<CNode::Fieldname, CNode>>()));
                    }
#line 2257 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 72:
#line 510 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<std::pair<CNode::Fieldname, CNode>>() = {
                            YY_MOVE(yystack_[1].value.as<CNode::Fieldname>()),
                            YY_MOVE(yystack_[0].value.as<CNode>())};
                    }
#line 2265 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 73:
#line 516 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 2271 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 74:
#line 521 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() =
                            YY_MOVE(yystack_[0].value.as<CNode::Fieldname>());
                    }
#line 2277 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 75:
#line 521 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() =
                            YY_MOVE(yystack_[0].value.as<CNode::Fieldname>());
                    }
#line 2283 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 76:
#line 521 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() =
                            YY_MOVE(yystack_[0].value.as<CNode::Fieldname>());
                    }
#line 2289 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 77:
#line 525 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() =
                            UserFieldname{YY_MOVE(yystack_[0].value.as<std::string>())};
                    }
#line 2297 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 78:
#line 533 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() =
                            UserFieldname{"$_internalInhibitOptimization"};
                    }
#line 2305 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 79:
#line 536 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$unionWith"};
                    }
#line 2313 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 80:
#line 539 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$skip"};
                    }
#line 2321 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 81:
#line 542 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$limit"};
                    }
#line 2329 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 82:
#line 545 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$project"};
                    }
#line 2337 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 83:
#line 548 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$sample"};
                    }
#line 2345 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 84:
#line 557 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"coll"};
                    }
#line 2353 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 85:
#line 560 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"pipeline"};
                    }
#line 2361 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 86:
#line 563 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"size"};
                    }
#line 2369 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 87:
#line 566 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"input"};
                    }
#line 2377 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 88:
#line 569 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"to"};
                    }
#line 2385 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 89:
#line 572 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"onError"};
                    }
#line 2393 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 90:
#line 575 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"onNull"};
                    }
#line 2401 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 91:
#line 578 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"dateString"};
                    }
#line 2409 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 92:
#line 581 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"format"};
                    }
#line 2417 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 93:
#line 584 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"timezone"};
                    }
#line 2425 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 94:
#line 587 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"date"};
                    }
#line 2433 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 95:
#line 590 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"chars"};
                    }
#line 2441 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 96:
#line 593 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"regex"};
                    }
#line 2449 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 97:
#line 596 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"options"};
                    }
#line 2457 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 98:
#line 599 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"find"};
                    }
#line 2465 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 99:
#line 602 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"replacement"};
                    }
#line 2473 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 100:
#line 605 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"filter"};
                    }
#line 2481 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 101:
#line 608 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"q"};
                    }
#line 2489 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 102:
#line 616 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$add"};
                    }
#line 2497 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 103:
#line 619 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$atan2"};
                    }
#line 2505 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 104:
#line 622 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$and"};
                    }
#line 2513 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 105:
#line 625 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$const"};
                    }
#line 2521 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 106:
#line 628 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$literal"};
                    }
#line 2529 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 107:
#line 631 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$or"};
                    }
#line 2537 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 108:
#line 634 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$not"};
                    }
#line 2545 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 109:
#line 637 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$cmp"};
                    }
#line 2553 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 110:
#line 640 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$eq"};
                    }
#line 2561 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 111:
#line 643 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$gt"};
                    }
#line 2569 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 112:
#line 646 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$gte"};
                    }
#line 2577 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 113:
#line 649 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$lt"};
                    }
#line 2585 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 114:
#line 652 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$lte"};
                    }
#line 2593 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 115:
#line 655 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$ne"};
                    }
#line 2601 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 116:
#line 658 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$convert"};
                    }
#line 2609 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 117:
#line 661 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$toBool"};
                    }
#line 2617 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 118:
#line 664 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$toDate"};
                    }
#line 2625 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 119:
#line 667 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$toDecimal"};
                    }
#line 2633 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 120:
#line 670 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$toDouble"};
                    }
#line 2641 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 121:
#line 673 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$toInt"};
                    }
#line 2649 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 122:
#line 676 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$toLong"};
                    }
#line 2657 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 123:
#line 679 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$toObjectId"};
                    }
#line 2665 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 124:
#line 682 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$toString"};
                    }
#line 2673 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 125:
#line 685 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$type"};
                    }
#line 2681 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 126:
#line 688 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$abs"};
                    }
#line 2689 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 127:
#line 691 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$ceil"};
                    }
#line 2697 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 128:
#line 694 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$divide"};
                    }
#line 2705 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 129:
#line 697 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$exp"};
                    }
#line 2713 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 130:
#line 700 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$floor"};
                    }
#line 2721 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 131:
#line 703 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$ln"};
                    }
#line 2729 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 132:
#line 706 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$log"};
                    }
#line 2737 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 133:
#line 709 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$log10"};
                    }
#line 2745 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 134:
#line 712 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$mod"};
                    }
#line 2753 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 135:
#line 715 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$multiply"};
                    }
#line 2761 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 136:
#line 718 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$pow"};
                    }
#line 2769 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 137:
#line 721 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$round"};
                    }
#line 2777 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 138:
#line 724 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$sqrt"};
                    }
#line 2785 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 139:
#line 727 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$subtract"};
                    }
#line 2793 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 140:
#line 730 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$trunc"};
                    }
#line 2801 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 141:
#line 733 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$concat"};
                    }
#line 2809 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 142:
#line 736 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$dateFromString"};
                    }
#line 2817 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 143:
#line 739 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$dateToString"};
                    }
#line 2825 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 144:
#line 742 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$indexOfBytes"};
                    }
#line 2833 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 145:
#line 745 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$indexOfCP"};
                    }
#line 2841 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 146:
#line 748 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$ltrim"};
                    }
#line 2849 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 147:
#line 751 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$meta"};
                    }
#line 2857 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 148:
#line 754 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$regexFind"};
                    }
#line 2865 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 149:
#line 757 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$regexFindAll"};
                    }
#line 2873 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 150:
#line 760 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$regexMatch"};
                    }
#line 2881 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 151:
#line 763 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$replaceOne"};
                    }
#line 2889 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 152:
#line 766 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$replaceAll"};
                    }
#line 2897 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 153:
#line 769 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$rtrim"};
                    }
#line 2905 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 154:
#line 772 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$split"};
                    }
#line 2913 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 155:
#line 775 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$strLenBytes"};
                    }
#line 2921 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 156:
#line 778 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$strLenCP"};
                    }
#line 2929 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 157:
#line 781 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$strcasecmp"};
                    }
#line 2937 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 158:
#line 784 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$substr"};
                    }
#line 2945 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 159:
#line 787 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$substrBytes"};
                    }
#line 2953 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 160:
#line 790 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$substrCP"};
                    }
#line 2961 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 161:
#line 793 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$toLower"};
                    }
#line 2969 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 162:
#line 796 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$trim"};
                    }
#line 2977 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 163:
#line 799 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$toUpper"};
                    }
#line 2985 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 164:
#line 806 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() =
                            CNode{UserString{YY_MOVE(yystack_[0].value.as<std::string>())}};
                    }
#line 2993 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 165:
#line 811 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{UserString{"randVal"}};
                    }
#line 3001 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 166:
#line 814 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{UserString{"textScore"}};
                    }
#line 3009 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 167:
#line 820 "src/mongo/db/cst/grammar.yy"
                    {
                        std::string str = YY_MOVE(yystack_[0].value.as<std::string>());
                        if (str.size() == 1) {
                            error(yystack_[0].location, "'$' by iteslf is not a valid FieldPath");
                        }
                        yylhs.value.as<CNode>() = CNode{UserFieldPath{str.substr(1), false}};
                    }
#line 3021 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 168:
#line 828 "src/mongo/db/cst/grammar.yy"
                    {
                        std::string str = YY_MOVE(yystack_[0].value.as<std::string>()).substr(2);
                        auto status = c_node_validation::validateVariableName(str);
                        if (!status.isOK()) {
                            error(yystack_[0].location, status.reason());
                        }
                        yylhs.value.as<CNode>() = CNode{UserFieldPath{str, true}};
                    }
#line 3034 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 169:
#line 837 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() =
                            CNode{UserBinary{YY_MOVE(yystack_[0].value.as<BSONBinData>())}};
                    }
#line 3042 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 170:
#line 843 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{UserUndefined{}};
                    }
#line 3050 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 171:
#line 849 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{UserObjectId{}};
                    }
#line 3058 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 172:
#line 855 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() =
                            CNode{UserDate{YY_MOVE(yystack_[0].value.as<Date_t>())}};
                    }
#line 3066 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 173:
#line 861 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{UserNull{}};
                    }
#line 3074 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 174:
#line 867 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() =
                            CNode{UserRegex{YY_MOVE(yystack_[0].value.as<BSONRegEx>())}};
                    }
#line 3082 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 175:
#line 873 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() =
                            CNode{UserDBPointer{YY_MOVE(yystack_[0].value.as<BSONDBRef>())}};
                    }
#line 3090 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 176:
#line 879 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() =
                            CNode{UserJavascript{YY_MOVE(yystack_[0].value.as<BSONCode>())}};
                    }
#line 3098 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 177:
#line 885 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() =
                            CNode{UserSymbol{YY_MOVE(yystack_[0].value.as<BSONSymbol>())}};
                    }
#line 3106 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 178:
#line 891 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{UserJavascriptWithScope{
                            YY_MOVE(yystack_[0].value.as<BSONCodeWScope>())}};
                    }
#line 3114 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 179:
#line 897 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() =
                            CNode{UserTimestamp{YY_MOVE(yystack_[0].value.as<Timestamp>())}};
                    }
#line 3122 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 180:
#line 903 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() =
                            CNode{UserMinKey{YY_MOVE(yystack_[0].value.as<UserMinKey>())}};
                    }
#line 3130 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 181:
#line 909 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() =
                            CNode{UserMaxKey{YY_MOVE(yystack_[0].value.as<UserMaxKey>())}};
                    }
#line 3138 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 182:
#line 915 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() =
                            CNode{UserInt{YY_MOVE(yystack_[0].value.as<int>())}};
                    }
#line 3146 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 183:
#line 918 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{UserInt{0}};
                    }
#line 3154 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 184:
#line 921 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{UserInt{1}};
                    }
#line 3162 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 185:
#line 924 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{UserInt{-1}};
                    }
#line 3170 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 186:
#line 930 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() =
                            CNode{UserLong{YY_MOVE(yystack_[0].value.as<long long>())}};
                    }
#line 3178 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 187:
#line 933 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{UserLong{0ll}};
                    }
#line 3186 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 188:
#line 936 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{UserLong{1ll}};
                    }
#line 3194 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 189:
#line 939 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{UserLong{-1ll}};
                    }
#line 3202 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 190:
#line 945 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() =
                            CNode{UserDouble{YY_MOVE(yystack_[0].value.as<double>())}};
                    }
#line 3210 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 191:
#line 948 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{UserDouble{0.0}};
                    }
#line 3218 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 192:
#line 951 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{UserDouble{1.0}};
                    }
#line 3226 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 193:
#line 954 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{UserDouble{-1.0}};
                    }
#line 3234 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 194:
#line 960 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() =
                            CNode{UserDecimal{YY_MOVE(yystack_[0].value.as<Decimal128>())}};
                    }
#line 3242 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 195:
#line 963 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{UserDecimal{0.0}};
                    }
#line 3250 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 196:
#line 966 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{UserDecimal{1.0}};
                    }
#line 3258 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 197:
#line 969 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{UserDecimal{-1.0}};
                    }
#line 3266 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 198:
#line 975 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{UserBoolean{true}};
                    }
#line 3274 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 199:
#line 978 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{UserBoolean{false}};
                    }
#line 3282 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 200:
#line 984 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 3288 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 201:
#line 985 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 3294 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 202:
#line 986 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 3300 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 203:
#line 987 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 3306 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 204:
#line 988 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 3312 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 205:
#line 989 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 3318 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 206:
#line 990 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 3324 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 207:
#line 991 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 3330 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 208:
#line 992 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 3336 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 209:
#line 993 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 3342 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 210:
#line 994 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 3348 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 211:
#line 995 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 3354 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 212:
#line 996 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 3360 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 213:
#line 997 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 3366 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 214:
#line 998 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 3372 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 215:
#line 999 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 3378 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 216:
#line 1000 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 3384 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 217:
#line 1001 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 3390 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 218:
#line 1002 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 3396 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 219:
#line 1003 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 3402 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 220:
#line 1004 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 3408 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 221:
#line 1011 "src/mongo/db/cst/grammar.yy"
                    {
                    }
#line 3414 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 222:
#line 1012 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<std::vector<CNode>>() =
                            YY_MOVE(yystack_[0].value.as<std::vector<CNode>>());
                        yylhs.value.as<std::vector<CNode>>().emplace_back(
                            YY_MOVE(yystack_[1].value.as<CNode>()));
                    }
#line 3423 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 223:
#line 1019 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 3429 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 224:
#line 1019 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 3435 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 225:
#line 1023 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() =
                            CNode{CNode::ArrayChildren{YY_MOVE(yystack_[2].value.as<CNode>()),
                                                       YY_MOVE(yystack_[1].value.as<CNode>())}};
                    }
#line 3443 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 226:
#line 1028 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 3449 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 227:
#line 1028 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 3455 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 228:
#line 1028 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 3461 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 229:
#line 1028 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 3467 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 230:
#line 1028 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 3473 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 231:
#line 1028 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 3479 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 232:
#line 1029 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 3485 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 233:
#line 1029 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 3491 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 234:
#line 1035 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() =
                            CNode{YY_MOVE(yystack_[1].value.as<std::vector<CNode>>())};
                    }
#line 3499 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 235:
#line 1043 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[1].value.as<CNode>());
                    }
#line 3507 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 236:
#line 1049 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode::noopLeaf();
                    }
#line 3515 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 237:
#line 1052 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[1].value.as<CNode>());
                        yylhs.value.as<CNode>().objectChildren().emplace_back(
                            YY_MOVE(yystack_[0].value.as<std::pair<CNode::Fieldname, CNode>>()));
                    }
#line 3524 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 238:
#line 1059 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<std::pair<CNode::Fieldname, CNode>>() = {
                            YY_MOVE(yystack_[1].value.as<CNode::Fieldname>()),
                            YY_MOVE(yystack_[0].value.as<CNode>())};
                    }
#line 3532 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 239:
#line 1066 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() =
                            YY_MOVE(yystack_[0].value.as<CNode::Fieldname>());
                    }
#line 3538 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 240:
#line 1066 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() =
                            YY_MOVE(yystack_[0].value.as<CNode::Fieldname>());
                    }
#line 3544 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 241:
#line 1066 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() =
                            YY_MOVE(yystack_[0].value.as<CNode::Fieldname>());
                    }
#line 3550 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 242:
#line 1066 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() =
                            YY_MOVE(yystack_[0].value.as<CNode::Fieldname>());
                    }
#line 3556 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 243:
#line 1070 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"_id"};
                    }
#line 3564 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 244:
#line 1076 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 3570 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 245:
#line 1076 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 3576 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 246:
#line 1076 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 3582 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 247:
#line 1076 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 3588 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 248:
#line 1076 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 3594 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 249:
#line 1076 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 3600 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 250:
#line 1076 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 3606 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 251:
#line 1076 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 3612 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 252:
#line 1076 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 3618 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 253:
#line 1076 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 3624 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 254:
#line 1076 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 3630 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 255:
#line 1076 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 3636 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 256:
#line 1076 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 3642 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 257:
#line 1077 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 3648 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 258:
#line 1077 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 3654 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 259:
#line 1077 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 3660 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 260:
#line 1077 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 3666 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 261:
#line 1081 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::add, YY_MOVE(yystack_[1].value.as<CNode>())}}};
                    }
#line 3675 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 262:
#line 1088 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::atan2, YY_MOVE(yystack_[1].value.as<CNode>())}}};
                    }
#line 3684 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 263:
#line 1094 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::abs, YY_MOVE(yystack_[1].value.as<CNode>())}}};
                    }
#line 3692 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 264:
#line 1099 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::ceil, YY_MOVE(yystack_[1].value.as<CNode>())}}};
                    }
#line 3700 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 265:
#line 1104 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::divide,
                             CNode{CNode::ArrayChildren{YY_MOVE(yystack_[3].value.as<CNode>()),
                                                        YY_MOVE(yystack_[2].value.as<CNode>())}}}}};
                    }
#line 3709 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 266:
#line 1110 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::exponent, YY_MOVE(yystack_[1].value.as<CNode>())}}};
                    }
#line 3717 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 267:
#line 1115 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::floor, YY_MOVE(yystack_[1].value.as<CNode>())}}};
                    }
#line 3725 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 268:
#line 1120 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::ln, YY_MOVE(yystack_[1].value.as<CNode>())}}};
                    }
#line 3733 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 269:
#line 1125 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::log,
                             CNode{CNode::ArrayChildren{YY_MOVE(yystack_[3].value.as<CNode>()),
                                                        YY_MOVE(yystack_[2].value.as<CNode>())}}}}};
                    }
#line 3742 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 270:
#line 1131 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::logten, YY_MOVE(yystack_[1].value.as<CNode>())}}};
                    }
#line 3750 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 271:
#line 1136 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::mod,
                             CNode{CNode::ArrayChildren{YY_MOVE(yystack_[3].value.as<CNode>()),
                                                        YY_MOVE(yystack_[2].value.as<CNode>())}}}}};
                    }
#line 3759 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 272:
#line 1142 "src/mongo/db/cst/grammar.yy"
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
#line 3771 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 273:
#line 1151 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::pow,
                             CNode{CNode::ArrayChildren{YY_MOVE(yystack_[3].value.as<CNode>()),
                                                        YY_MOVE(yystack_[2].value.as<CNode>())}}}}};
                    }
#line 3780 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 274:
#line 1157 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::round,
                             CNode{CNode::ArrayChildren{YY_MOVE(yystack_[3].value.as<CNode>()),
                                                        YY_MOVE(yystack_[2].value.as<CNode>())}}}}};
                    }
#line 3789 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 275:
#line 1163 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::sqrt, YY_MOVE(yystack_[1].value.as<CNode>())}}};
                    }
#line 3797 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 276:
#line 1168 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::subtract,
                             CNode{CNode::ArrayChildren{YY_MOVE(yystack_[3].value.as<CNode>()),
                                                        YY_MOVE(yystack_[2].value.as<CNode>())}}}}};
                    }
#line 3806 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 277:
#line 1174 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::trunc,
                             CNode{CNode::ArrayChildren{YY_MOVE(yystack_[3].value.as<CNode>()),
                                                        YY_MOVE(yystack_[2].value.as<CNode>())}}}}};
                    }
#line 3815 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 278:
#line 1180 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 3821 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 279:
#line 1180 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 3827 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 280:
#line 1180 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 3833 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 281:
#line 1184 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::andExpr, YY_MOVE(yystack_[1].value.as<CNode>())}}};
                    }
#line 3842 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 282:
#line 1191 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::orExpr, YY_MOVE(yystack_[1].value.as<CNode>())}}};
                    }
#line 3851 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 283:
#line 1198 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::notExpr,
                             CNode{CNode::ArrayChildren{YY_MOVE(yystack_[2].value.as<CNode>())}}}}};
                    }
#line 3860 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 284:
#line 1205 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 3866 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 285:
#line 1205 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 3872 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 286:
#line 1205 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 3878 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 287:
#line 1205 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 3884 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 288:
#line 1205 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 3890 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 289:
#line 1205 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 3896 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 290:
#line 1205 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 3902 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 291:
#line 1206 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 3908 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 292:
#line 1206 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 3914 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 293:
#line 1206 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 3920 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 294:
#line 1206 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 3926 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 295:
#line 1206 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 3932 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 296:
#line 1206 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 3938 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 297:
#line 1206 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 3944 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 298:
#line 1206 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 3950 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 299:
#line 1207 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 3956 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 300:
#line 1207 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 3962 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 301:
#line 1207 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 3968 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 302:
#line 1207 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 3974 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 303:
#line 1207 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 3980 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 304:
#line 1207 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 3986 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 305:
#line 1207 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 3992 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 306:
#line 1211 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::concat, CNode{CNode::ArrayChildren{}}}}};
                        auto&& others = YY_MOVE(yystack_[2].value.as<std::vector<CNode>>());
                        auto&& array =
                            yylhs.value.as<CNode>().objectChildren()[0].second.arrayChildren();
                        array.insert(array.end(), others.begin(), others.end());
                    }
#line 4004 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 307:
#line 1221 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<std::pair<CNode::Fieldname, CNode>>() =
                            std::pair{KeyFieldname::formatArg, CNode{KeyValue::absentKey}};
                    }
#line 4012 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 308:
#line 1224 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<std::pair<CNode::Fieldname, CNode>>() = std::pair{
                            KeyFieldname::formatArg, YY_MOVE(yystack_[0].value.as<CNode>())};
                    }
#line 4020 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 309:
#line 1230 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<std::pair<CNode::Fieldname, CNode>>() =
                            std::pair{KeyFieldname::timezoneArg, CNode{KeyValue::absentKey}};
                    }
#line 4028 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 310:
#line 1233 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<std::pair<CNode::Fieldname, CNode>>() = std::pair{
                            KeyFieldname::timezoneArg, YY_MOVE(yystack_[0].value.as<CNode>())};
                    }
#line 4036 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 311:
#line 1240 "src/mongo/db/cst/grammar.yy"
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
#line 4046 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 312:
#line 1249 "src/mongo/db/cst/grammar.yy"
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
#line 4056 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 313:
#line 1257 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<std::vector<CNode>>() = CNode::ArrayChildren{};
                    }
#line 4064 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 314:
#line 1260 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<std::vector<CNode>>() =
                            CNode::ArrayChildren{YY_MOVE(yystack_[0].value.as<CNode>())};
                    }
#line 4072 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 315:
#line 1263 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<std::vector<CNode>>() =
                            CNode::ArrayChildren{YY_MOVE(yystack_[1].value.as<CNode>()),
                                                 YY_MOVE(yystack_[0].value.as<CNode>())};
                    }
#line 4080 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 316:
#line 1270 "src/mongo/db/cst/grammar.yy"
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
#line 4092 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 317:
#line 1281 "src/mongo/db/cst/grammar.yy"
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
#line 4104 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 318:
#line 1291 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<std::pair<CNode::Fieldname, CNode>>() =
                            std::pair{KeyFieldname::charsArg, CNode{KeyValue::absentKey}};
                    }
#line 4112 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 319:
#line 1294 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<std::pair<CNode::Fieldname, CNode>>() = std::pair{
                            KeyFieldname::charsArg, YY_MOVE(yystack_[0].value.as<CNode>())};
                    }
#line 4120 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 320:
#line 1300 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::ltrim,
                             CNode{CNode::ObjectChildren{
                                 {KeyFieldname::inputArg, YY_MOVE(yystack_[2].value.as<CNode>())},
                                 YY_MOVE(yystack_[4]
                                             .value.as<std::pair<CNode::Fieldname, CNode>>())}}}}};
                    }
#line 4130 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 321:
#line 1308 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::rtrim,
                             CNode{CNode::ObjectChildren{
                                 {KeyFieldname::inputArg, YY_MOVE(yystack_[2].value.as<CNode>())},
                                 YY_MOVE(yystack_[4]
                                             .value.as<std::pair<CNode::Fieldname, CNode>>())}}}}};
                    }
#line 4140 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 322:
#line 1316 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::trim,
                             CNode{CNode::ObjectChildren{
                                 {KeyFieldname::inputArg, YY_MOVE(yystack_[2].value.as<CNode>())},
                                 YY_MOVE(yystack_[4]
                                             .value.as<std::pair<CNode::Fieldname, CNode>>())}}}}};
                    }
#line 4150 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 323:
#line 1324 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<std::pair<CNode::Fieldname, CNode>>() =
                            std::pair{KeyFieldname::optionsArg, CNode{KeyValue::absentKey}};
                    }
#line 4158 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 324:
#line 1327 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<std::pair<CNode::Fieldname, CNode>>() = std::pair{
                            KeyFieldname::optionsArg, YY_MOVE(yystack_[0].value.as<CNode>())};
                    }
#line 4166 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 325:
#line 1332 "src/mongo/db/cst/grammar.yy"
                    {
                        // Note that the order of these arguments must match the constructor for the
                        // regex expression.
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::inputArg, YY_MOVE(yystack_[4].value.as<CNode>())},
                            {KeyFieldname::regexArg, YY_MOVE(yystack_[1].value.as<CNode>())},
                            YY_MOVE(yystack_[3].value.as<std::pair<CNode::Fieldname, CNode>>())}};
                    }
#line 4178 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 326:
#line 1341 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::regexFind, YY_MOVE(yystack_[1].value.as<CNode>())}}};
                    }
#line 4186 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 327:
#line 1347 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::regexFindAll, YY_MOVE(yystack_[1].value.as<CNode>())}}};
                    }
#line 4194 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 328:
#line 1353 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::regexMatch, YY_MOVE(yystack_[1].value.as<CNode>())}}};
                    }
#line 4202 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 329:
#line 1360 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::replaceOne,
                             CNode{CNode::ObjectChildren{
                                 {KeyFieldname::inputArg, YY_MOVE(yystack_[4].value.as<CNode>())},
                                 {KeyFieldname::findArg, YY_MOVE(yystack_[6].value.as<CNode>())},
                                 {KeyFieldname::replacementArg,
                                  YY_MOVE(yystack_[2].value.as<CNode>())}}}}}};
                    }
#line 4213 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 330:
#line 1370 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::replaceAll,
                             CNode{CNode::ObjectChildren{
                                 {KeyFieldname::inputArg, YY_MOVE(yystack_[4].value.as<CNode>())},
                                 {KeyFieldname::findArg, YY_MOVE(yystack_[6].value.as<CNode>())},
                                 {KeyFieldname::replacementArg,
                                  YY_MOVE(yystack_[2].value.as<CNode>())}}}}}};
                    }
#line 4224 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 331:
#line 1379 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::split,
                             CNode{CNode::ArrayChildren{YY_MOVE(yystack_[3].value.as<CNode>()),
                                                        YY_MOVE(yystack_[2].value.as<CNode>())}}}}};
                    }
#line 4233 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 332:
#line 1386 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::strLenBytes, YY_MOVE(yystack_[1].value.as<CNode>())}}};
                    }
#line 4242 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 333:
#line 1393 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::strLenCP, YY_MOVE(yystack_[1].value.as<CNode>())}}};
                    }
#line 4251 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 334:
#line 1401 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::strcasecmp,
                             CNode{CNode::ArrayChildren{YY_MOVE(yystack_[3].value.as<CNode>()),
                                                        YY_MOVE(yystack_[2].value.as<CNode>())}}}}};
                    }
#line 4260 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 335:
#line 1409 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::substr,
                             CNode{CNode::ArrayChildren{YY_MOVE(yystack_[4].value.as<CNode>()),
                                                        YY_MOVE(yystack_[3].value.as<CNode>()),
                                                        YY_MOVE(yystack_[2].value.as<CNode>())}}}}};
                    }
#line 4269 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 336:
#line 1417 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::substrBytes,
                             CNode{CNode::ArrayChildren{YY_MOVE(yystack_[4].value.as<CNode>()),
                                                        YY_MOVE(yystack_[3].value.as<CNode>()),
                                                        YY_MOVE(yystack_[2].value.as<CNode>())}}}}};
                    }
#line 4278 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 337:
#line 1425 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::substrCP,
                             CNode{CNode::ArrayChildren{YY_MOVE(yystack_[4].value.as<CNode>()),
                                                        YY_MOVE(yystack_[3].value.as<CNode>()),
                                                        YY_MOVE(yystack_[2].value.as<CNode>())}}}}};
                    }
#line 4287 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 338:
#line 1432 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::toLower, YY_MOVE(yystack_[1].value.as<CNode>())}}};
                    }
#line 4295 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 339:
#line 1438 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::toUpper, YY_MOVE(yystack_[1].value.as<CNode>())}}};
                    }
#line 4303 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 340:
#line 1444 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{KeyValue::randVal};
                    }
#line 4311 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 341:
#line 1447 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{KeyValue::textScore};
                    }
#line 4319 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 342:
#line 1453 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::meta, YY_MOVE(yystack_[1].value.as<CNode>())}}};
                    }
#line 4327 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 343:
#line 1459 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[1].value.as<CNode>());
                    }
#line 4335 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 344:
#line 1464 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode::noopLeaf();
                    }
#line 4343 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 345:
#line 1467 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[1].value.as<CNode>());
                        yylhs.value.as<CNode>().objectChildren().emplace_back(
                            YY_MOVE(yystack_[0].value.as<std::pair<CNode::Fieldname, CNode>>()));
                    }
#line 4352 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 346:
#line 1474 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{KeyValue::intOneKey};
                    }
#line 4360 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 347:
#line 1477 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{KeyValue::intNegOneKey};
                    }
#line 4368 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 348:
#line 1480 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{KeyValue::longOneKey};
                    }
#line 4376 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 349:
#line 1483 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{KeyValue::longNegOneKey};
                    }
#line 4384 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 350:
#line 1486 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{KeyValue::doubleOneKey};
                    }
#line 4392 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 351:
#line 1489 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{KeyValue::doubleNegOneKey};
                    }
#line 4400 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 352:
#line 1492 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{KeyValue::decimalOneKey};
                    }
#line 4408 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 353:
#line 1495 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{KeyValue::decimalNegOneKey};
                    }
#line 4416 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 354:
#line 1500 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<std::pair<CNode::Fieldname, CNode>>() = {
                            YY_MOVE(yystack_[1].value.as<CNode::Fieldname>()),
                            YY_MOVE(yystack_[0].value.as<CNode>())};
                    }
#line 4424 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 355:
#line 1502 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<std::pair<CNode::Fieldname, CNode>>() = {
                            YY_MOVE(yystack_[1].value.as<CNode::Fieldname>()),
                            YY_MOVE(yystack_[0].value.as<CNode>())};
                    }
#line 4432 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 356:
#line 1508 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 4438 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 357:
#line 1508 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 4444 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 358:
#line 1512 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::constExpr,
                             CNode{CNode::ArrayChildren{YY_MOVE(yystack_[2].value.as<CNode>())}}}}};
                    }
#line 4453 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 359:
#line 1519 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::literal,
                             CNode{CNode::ArrayChildren{YY_MOVE(yystack_[2].value.as<CNode>())}}}}};
                    }
#line 4462 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 360:
#line 1526 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 4468 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 361:
#line 1526 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 4474 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 362:
#line 1530 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 4480 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 363:
#line 1530 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 4486 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 364:
#line 1534 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() =
                            CNode{YY_MOVE(yystack_[1].value.as<std::vector<CNode>>())};
                    }
#line 4494 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 365:
#line 1540 "src/mongo/db/cst/grammar.yy"
                    {
                    }
#line 4500 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 366:
#line 1541 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<std::vector<CNode>>() =
                            YY_MOVE(yystack_[0].value.as<std::vector<CNode>>());
                        yylhs.value.as<std::vector<CNode>>().emplace_back(
                            YY_MOVE(yystack_[1].value.as<CNode>()));
                    }
#line 4509 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 367:
#line 1548 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[1].value.as<CNode>());
                    }
#line 4517 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 368:
#line 1554 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode::noopLeaf();
                    }
#line 4525 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 369:
#line 1557 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[1].value.as<CNode>());
                        yylhs.value.as<CNode>().objectChildren().emplace_back(
                            YY_MOVE(yystack_[0].value.as<std::pair<CNode::Fieldname, CNode>>()));
                    }
#line 4534 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 370:
#line 1564 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<std::pair<CNode::Fieldname, CNode>>() = {
                            YY_MOVE(yystack_[1].value.as<CNode::Fieldname>()),
                            YY_MOVE(yystack_[0].value.as<CNode>())};
                    }
#line 4542 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 371:
#line 1571 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() =
                            YY_MOVE(yystack_[0].value.as<CNode::Fieldname>());
                    }
#line 4548 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 372:
#line 1572 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() =
                            YY_MOVE(yystack_[0].value.as<CNode::Fieldname>());
                    }
#line 4554 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 373:
#line 1573 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() =
                            YY_MOVE(yystack_[0].value.as<CNode::Fieldname>());
                    }
#line 4560 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 374:
#line 1574 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() =
                            YY_MOVE(yystack_[0].value.as<CNode::Fieldname>());
                    }
#line 4566 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 375:
#line 1575 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() =
                            YY_MOVE(yystack_[0].value.as<CNode::Fieldname>());
                    }
#line 4572 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 376:
#line 1578 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 4578 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 377:
#line 1578 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 4584 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 378:
#line 1578 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 4590 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 379:
#line 1578 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 4596 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 380:
#line 1578 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 4602 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 381:
#line 1578 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 4608 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 382:
#line 1578 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 4614 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 383:
#line 1580 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::cmp, YY_MOVE(yystack_[1].value.as<CNode>())}}};
                    }
#line 4623 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 384:
#line 1585 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::eq, YY_MOVE(yystack_[1].value.as<CNode>())}}};
                    }
#line 4632 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 385:
#line 1590 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::gt, YY_MOVE(yystack_[1].value.as<CNode>())}}};
                    }
#line 4641 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 386:
#line 1595 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::gte, YY_MOVE(yystack_[1].value.as<CNode>())}}};
                    }
#line 4650 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 387:
#line 1600 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::lt, YY_MOVE(yystack_[1].value.as<CNode>())}}};
                    }
#line 4659 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 388:
#line 1605 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::lte, YY_MOVE(yystack_[1].value.as<CNode>())}}};
                    }
#line 4668 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 389:
#line 1610 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::ne, YY_MOVE(yystack_[1].value.as<CNode>())}}};
                    }
#line 4677 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 390:
#line 1616 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 4683 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 391:
#line 1617 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 4689 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 392:
#line 1618 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 4695 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 393:
#line 1619 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 4701 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 394:
#line 1620 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 4707 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 395:
#line 1621 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 4713 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 396:
#line 1622 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 4719 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 397:
#line 1623 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 4725 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 398:
#line 1624 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 4731 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 399:
#line 1625 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 4737 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 400:
#line 1630 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<std::pair<CNode::Fieldname, CNode>>() =
                            std::pair{KeyFieldname::onErrorArg, CNode{KeyValue::absentKey}};
                    }
#line 4745 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 401:
#line 1633 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<std::pair<CNode::Fieldname, CNode>>() = std::pair{
                            KeyFieldname::onErrorArg, YY_MOVE(yystack_[0].value.as<CNode>())};
                    }
#line 4753 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 402:
#line 1640 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<std::pair<CNode::Fieldname, CNode>>() =
                            std::pair{KeyFieldname::onNullArg, CNode{KeyValue::absentKey}};
                    }
#line 4761 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 403:
#line 1643 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<std::pair<CNode::Fieldname, CNode>>() = std::pair{
                            KeyFieldname::onNullArg, YY_MOVE(yystack_[0].value.as<CNode>())};
                    }
#line 4769 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 404:
#line 1650 "src/mongo/db/cst/grammar.yy"
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
#line 4780 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 405:
#line 1659 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::toBool, YY_MOVE(yystack_[1].value.as<CNode>())}}};
                    }
#line 4788 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 406:
#line 1664 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::toDate, YY_MOVE(yystack_[1].value.as<CNode>())}}};
                    }
#line 4796 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 407:
#line 1669 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::toDecimal, YY_MOVE(yystack_[1].value.as<CNode>())}}};
                    }
#line 4804 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 408:
#line 1674 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::toDouble, YY_MOVE(yystack_[1].value.as<CNode>())}}};
                    }
#line 4812 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 409:
#line 1679 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::toInt, YY_MOVE(yystack_[1].value.as<CNode>())}}};
                    }
#line 4820 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 410:
#line 1684 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::toLong, YY_MOVE(yystack_[1].value.as<CNode>())}}};
                    }
#line 4828 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 411:
#line 1689 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::toObjectId, YY_MOVE(yystack_[1].value.as<CNode>())}}};
                    }
#line 4836 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 412:
#line 1694 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::toString, YY_MOVE(yystack_[1].value.as<CNode>())}}};
                    }
#line 4844 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 413:
#line 1699 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::type, YY_MOVE(yystack_[1].value.as<CNode>())}}};
                    }
#line 4852 "src/mongo/db/cst/parser_gen.cpp"
                    break;


#line 4856 "src/mongo/db/cst/parser_gen.cpp"

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

void ParserGen::error(const syntax_error& yyexc) {
    error(yyexc.location, yyexc.what());
}

// Generate an error message.
std::string ParserGen::yysyntax_error_(state_type yystate, const symbol_type& yyla) const {
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


const short ParserGen::yypact_ninf_ = -619;

const signed char ParserGen::yytable_ninf_ = -1;

const short ParserGen::yypact_[] = {
    77,   -82,  -75,  -82,  -82,  -71,  38,   -619, -619, -28,  -619, -619, -619, -619, -619, -619,
    849,  174,  10,   413,  -619, -619, -619, -619, -619, -619, -619, -619, -619, -619, -619, -619,
    -619, -619, -619, -619, -619, -619, -619, -619, -619, 850,  -619, -619, -619, -619, -26,  18,
    -22,  -13,  18,   -619, 41,   -619, -619, -619, -619, -619, -619, -619, -619, -619, -619, -619,
    -619, -619, -619, -619, -619, -619, -619, -619, -619, -619, -619, -619, -619, -619, -619, -619,
    -619, -619, -619, -619, -619, -619, -619, -619, -619, -619, -619, -619, -619, -619, -619, -619,
    -619, -619, -619, -619, -619, -619, -619, -619, -619, -619, -619, -619, -619, -619, -619, -619,
    -619, -619, -619, -619, -619, -619, -619, -619, -619, -619, -619, -619, -619, -619, -619, -619,
    -619, -619, -619, -619, -619, -619, 132,  -619, -619, -619, -619, -619, -619, -619, -619, -619,
    -619, -619, -619, -619, -619, -619, -619, 850,  -619, -619, -619, -619, -619, -619, -619, -619,
    -619, -619, -619, -619, -619, -619, -619, -619, -619, -619, -619, -619, -619, -619, -619, -619,
    -619, -619, -619, -619, -619, -619, -619, -619, -619, -619, -619, -619, -619, -619, -619, -619,
    -619, -619, -619, -619, -619, -619, -619, -619, -619, 47,   -619, -619, -619, -619, -619, -619,
    60,   -619, 86,   9,    -28,  -619, -619, -619, -619, -619, -619, -619, -619, 35,   -619, -619,
    850,  68,   523,  -619, 633,  18,   -56,  -619, -619, -49,  -619, -619, -619, 850,  -619, -619,
    1062, 1062, -619, -619, -619, -619, -619, 73,   120,  -619, -619, 102,  -619, -619, -619, -619,
    -619, -619, -619, -619, -619, -619, -619, -619, -619, -619, -619, 956,  743,  -619, -619, -619,
    -619, -619, -619, -619, -619, -619, -619, -619, -619, -619, -619, -619, -619, -619, -619, -619,
    -619, -619, -619, -619, -619, -619, -619, -619, -619, -619, -619, -619, -619, -619, -619, -619,
    -619, -619, -619, -619, -619, -619, -619, -619, -619, -619, -619, -619, -619, -619, -619, -619,
    -619, -619, -619, -619, -619, -619, -619, -619, -619, -619, -619, -619, -619, -619, -619, -619,
    -619, -619, -619, -619, -619, -619, -619, -619, -619, -619, -619, -619, -619, -619, -619, -619,
    -619, -619, -619, -619, -619, -619, -619, -619, 6,    -619, -619, 956,  -619, 115,  956,  72,
    72,   88,   956,  88,   114,  116,  -619, -619, -619, 118,  88,   956,  956,  88,   88,   119,
    121,  122,  956,  125,  956,  88,   88,   -619, 130,  131,  88,   134,  72,   137,  -619, -619,
    -619, -619, -619, 138,  -619, 141,  956,  142,  956,  956,  144,  147,  148,  149,  956,  956,
    956,  956,  956,  956,  956,  956,  956,  956,  -619, 151,  956,  19,   160,  -619, -619, 177,
    198,  199,  956,  203,  205,  207,  956,  850,  153,  166,  244,  956,  219,  220,  221,  222,
    223,  956,  956,  850,  225,  956,  226,  227,  228,  267,  956,  956,  230,  956,  231,  956,
    232,  264,  236,  237,  271,  273,  956,  267,  956,  241,  956,  242,  243,  956,  956,  956,
    956,  245,  246,  247,  249,  250,  254,  256,  257,  258,  259,  267,  956,  261,  -619, 956,
    -619, -619, -619, -619, -619, -619, -619, -619, -619, 956,  -619, -619, -619, 265,  266,  956,
    956,  956,  956,  -619, -619, -619, -619, -619, 956,  956,  268,  -619, 956,  -619, -619, -619,
    956,  275,  956,  956,  -619, 269,  -619, 956,  -619, 956,  -619, -619, 956,  956,  956,  294,
    956,  -619, 956,  -619, -619, 956,  956,  956,  956,  -619, -619, -619, -619, -619, -619, -619,
    -619, -619, -619, 300,  956,  -619, -619, 272,  270,  274,  302,  305,  305,  278,  956,  956,
    280,  282,  -619, 956,  286,  956,  287,  289,  314,  320,  321,  293,  956,  296,  297,  956,
    956,  956,  298,  956,  299,  -619, -619, -619, 956,  322,  956,  323,  323,  301,  956,  303,
    306,  -619, 304,  308,  311,  307,  -619, 313,  956,  324,  956,  956,  315,  316,  317,  318,
    325,  326,  327,  319,  328,  329,  -619, 956,  333,  -619, 956,  302,  322,  -619, -619, 334,
    335,  -619, 336,  -619, 337,  -619, -619, 956,  343,  345,  -619, 338,  -619, -619, 339,  340,
    341,  -619, 342,  -619, -619, 956,  -619, 322,  344,  -619, -619, -619, -619, 346,  956,  956,
    -619, -619, -619, -619, -619, 347,  348,  349,  -619, 350,  351,  352,  353,  -619, 354,  355,
    -619, -619, -619, -619};

const short ParserGen::yydefact_[] = {
    0,   0,   0,   0,   0,   0,   0,   70,  3,   8,   2,   5,   4,   344, 6,   1,   0,   0,   0,
    0,   95,  84,  94,  91,  100, 98,  92,  87,  89,  90,  97,  85,  101, 96,  99,  86,  93,  88,
    69,  243, 77,  0,   76,  75,  74,  71,  0,   0,   0,   0,   0,   10,  0,   12,  13,  14,  15,
    16,  17,  7,   126, 102, 104, 103, 127, 109, 141, 105, 116, 142, 143, 128, 343, 110, 129, 130,
    111, 112, 144, 145, 106, 131, 132, 133, 113, 114, 146, 147, 134, 135, 115, 108, 107, 136, 148,
    149, 150, 152, 151, 137, 153, 154, 138, 78,  81,  82,  83,  80,  79,  157, 155, 156, 158, 159,
    160, 139, 117, 118, 119, 120, 121, 122, 161, 123, 124, 163, 162, 140, 125, 372, 373, 374, 371,
    375, 0,   345, 199, 198, 197, 196, 195, 193, 192, 191, 185, 184, 183, 189, 188, 187, 165, 365,
    368, 166, 164, 169, 170, 171, 172, 173, 174, 175, 176, 177, 178, 182, 186, 190, 194, 179, 180,
    181, 167, 168, 209, 210, 211, 212, 213, 218, 214, 215, 216, 219, 220, 73,  200, 201, 203, 204,
    205, 217, 206, 207, 208, 360, 361, 362, 363, 202, 72,  0,   21,  22,  23,  24,  26,  28,  0,
    25,  0,   0,   8,   353, 352, 351, 350, 347, 346, 349, 348, 0,   354, 355, 365, 0,   0,   19,
    0,   0,   0,   11,  9,   0,   366, 364, 367, 0,   369, 27,  0,   0,   66,  67,  68,  65,  29,
    0,   0,   340, 341, 0,   370, 60,  59,  56,  55,  58,  52,  51,  54,  44,  43,  46,  48,  47,
    50,  221, 236, 45,  49,  53,  57,  39,  40,  41,  42,  61,  62,  63,  32,  33,  34,  35,  36,
    37,  38,  30,  64,  226, 227, 228, 244, 245, 229, 278, 279, 280, 230, 356, 357, 233, 284, 285,
    286, 287, 288, 289, 290, 291, 292, 293, 294, 295, 296, 297, 298, 299, 300, 301, 302, 303, 305,
    304, 231, 376, 377, 378, 379, 380, 381, 382, 232, 390, 391, 392, 393, 394, 395, 396, 397, 398,
    399, 246, 247, 248, 249, 250, 251, 252, 253, 254, 255, 256, 257, 258, 259, 260, 31,  18,  0,
    342, 223, 221, 224, 0,   0,   0,   0,   0,   0,   0,   0,   0,   10,  10,  10,  0,   0,   0,
    0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   10,  0,   0,   0,   0,   0,   0,   10,
    10,  10,  10,  10,  0,   10,  0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,
    0,   0,   0,   0,   0,   0,   10,  0,   0,   0,   0,   222, 234, 0,   0,   0,   0,   0,   0,
    0,   221, 0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,
    0,   318, 0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   318, 0,   0,   0,
    0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   318, 0,   0,
    235, 0,   240, 241, 239, 242, 237, 20,  263, 261, 281, 0,   262, 264, 383, 0,   0,   0,   0,
    0,   0,   384, 266, 267, 385, 386, 0,   0,   0,   268, 0,   270, 387, 388, 0,   0,   0,   0,
    389, 0,   282, 0,   326, 0,   327, 328, 0,   0,   0,   0,   0,   275, 0,   332, 333, 0,   0,
    0,   0,   405, 406, 407, 408, 409, 410, 338, 411, 412, 339, 0,   0,   413, 238, 0,   0,   0,
    400, 307, 307, 0,   313, 313, 0,   0,   319, 0,   0,   221, 0,   0,   323, 0,   0,   0,   0,
    0,   0,   0,   0,   0,   0,   0,   0,   225, 306, 358, 0,   402, 0,   309, 309, 0,   314, 0,
    0,   359, 0,   0,   0,   0,   283, 0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,
    0,   0,   0,   401, 0,   0,   308, 0,   400, 402, 265, 315, 0,   0,   269, 0,   271, 0,   273,
    324, 0,   0,   0,   274, 0,   331, 334, 0,   0,   0,   276, 0,   277, 403, 0,   310, 402, 0,
    316, 317, 320, 272, 0,   0,   0,   321, 335, 336, 337, 322, 0,   0,   0,   325, 0,   0,   0,
    0,   312, 0,   0,   404, 311, 330, 329};

const short ParserGen::yypgoto_[] = {
    -619, -619, -619, -221, -619, -16,  139,  -15,  -14,  145,  -619, -619, -619, -619, -189,
    -174, -152, -143, -35,  -132, -34,  -41,  -27,  -125, -112, -36,  -165, -619, -107, -105,
    -101, -619, -91,  -85,  -70,  -37,  -619, -619, -619, -619, -619, -619, 165,  -619, -619,
    -619, -619, -619, -619, -619, -619, 146,  -40,  -296, -61,  -230, -346, -619, -619, -619,
    -619, -619, -619, -619, -619, -619, -619, -619, -619, -619, -619, -619, -619, -619, -619,
    -619, -619, -619, -619, -209, -619, -619, -619, -619, -619, -619, -619, -619, -619, -619,
    -619, -619, -619, -619, -619, -619, -619, -619, -619, -619, -619, -619, -619, -619, -619,
    -619, -619, -619, -619, -619, -619, -619, -619, -619, -619, -619, -619, -619, -619, -619,
    -619, -619, -619, -619, -619, -619, -619, -246, -618, -172, -203, -410, -619, -352, 180,
    -170, 194,  -619, -619, -619, -619, -619, -619, -619, -619, -619, -48,  -619};

const short ParserGen::yydefgoto_[] = {
    -1,  241, 495, 129, 41,  130, 131, 132, 133, 134, 246, 500, 238, 45,  174, 175, 176, 177, 178,
    179, 180, 181, 182, 183, 184, 224, 186, 187, 188, 189, 190, 191, 192, 193, 194, 362, 196, 197,
    198, 226, 199, 10,  18,  52,  53,  54,  55,  56,  57,  58,  228, 287, 206, 363, 364, 435, 289,
    290, 427, 291, 292, 293, 294, 295, 296, 297, 298, 299, 300, 301, 302, 303, 304, 305, 306, 307,
    308, 309, 310, 464, 311, 312, 313, 314, 315, 316, 317, 318, 319, 320, 321, 322, 323, 324, 325,
    326, 327, 328, 329, 330, 331, 332, 333, 334, 335, 336, 337, 338, 339, 340, 341, 342, 343, 344,
    345, 346, 347, 348, 349, 350, 351, 352, 353, 354, 355, 356, 357, 601, 632, 603, 635, 529, 617,
    365, 225, 607, 8,   16,  200, 14,  19,  222, 223, 251, 135, 6,   465, 211};

const short ParserGen::yytable_[] = {
    42,  43,  44,  210, 195, 185, 204, 242, 7,   204, 209, 429, 202, 203, 9,   202, 203, 150, 664,
    13,  205, 432, 433, 205, 249, 20,  21,  22,  23,  24,  25,  26,  27,  28,  29,  30,  31,  32,
    15,  33,  34,  35,  153, 36,  37,  678, 141, 142, 143, 250, 462, 273, 273, 59,  138, 139, 140,
    154, 141, 142, 143, 543, 17,  494, 201, 248, 274, 274, 207, 39,  431, 144, 145, 146, 436, 280,
    280, 208, 147, 148, 149, 563, 229, 445, 446, 212, 509, 1,   275, 275, 452, 227, 454, 230, 2,
    3,   4,   276, 276, 231, 5,   233, 103, 104, 105, 106, 107, 108, 277, 277, 473, 235, 475, 476,
    195, 278, 278, 359, 481, 482, 483, 484, 485, 486, 487, 488, 489, 490, 279, 279, 493, 40,  167,
    281, 281, 282, 282, 360, 505, 283, 283, 437, 165, 166, 167, 168, 361, 514, 444, 284, 284, 447,
    448, 520, 521, 285, 285, 524, 430, 455, 456, 267, 530, 531, 460, 533, 511, 535, 213, 214, 286,
    286, 215, 216, 542, 512, 544, 434, 546, 288, 288, 549, 550, 551, 552, 217, 218, 195, 204, 247,
    466, 467, 219, 220, 202, 203, 564, 11,  12,  566, 195, 252, 205, 438, 501, 439, 496, 443, 449,
    567, 450, 451, 243, 245, 453, 570, 571, 572, 573, 458, 459, 502, 221, 461, 574, 575, 463, 470,
    577, 613, 472, 474, 578, 477, 580, 581, 478, 479, 480, 583, 492, 584, 503, 504, 585, 586, 587,
    506, 589, 507, 590, 508, 513, 591, 592, 593, 594, 46,  47,  48,  49,  50,  51,  515, 516, 517,
    518, 519, 596, 523, 525, 526, 527, 528, 532, 534, 536, 537, 606, 606, 538, 539, 540, 611, 541,
    545, 547, 548, 579, 553, 554, 555, 621, 556, 557, 624, 625, 626, 558, 628, 559, 560, 561, 562,
    630, 565, 633, 588, 568, 569, 638, 576, 582, 595, 598, 597, 600, 602, 599, 428, 646, 605, 648,
    649, 609, 610, 440, 441, 442, 612, 616, 614, 615, 618, 619, 660, 620, 631, 662, 622, 623, 627,
    629, 457, 647, 637, 639, 634, 641, 640, 644, 669, 642, 468, 469, 643, 471, 645, 661, 650, 651,
    652, 653, 657, 670, 677, 671, 244, 654, 655, 656, 237, 658, 659, 681, 682, 491, 232, 665, 666,
    667, 668, 672, 673, 674, 675, 676, 358, 679, 663, 680, 683, 684, 685, 686, 687, 688, 689, 690,
    691, 604, 636, 195, 510, 234, 608, 0,   0,   0,   0,   0,   497, 498, 499, 195, 522, 60,  61,
    62,  20,  21,  22,  23,  24,  25,  26,  27,  28,  29,  30,  31,  32,  0,   33,  34,  35,  0,
    36,  37,  63,  0,   0,   64,  65,  66,  67,  68,  69,  70,  0,   0,   0,   71,  0,   0,   0,
    0,   72,  73,  74,  75,  76,  77,  39,  78,  79,  0,   0,   0,   80,  81,  82,  83,  0,   0,
    0,   84,  85,  86,  87,  88,  89,  90,  91,  92,  93,  0,   94,  95,  96,  97,  98,  99,  100,
    101, 102, 103, 104, 105, 106, 107, 108, 0,   0,   109, 110, 111, 112, 113, 114, 115, 0,   116,
    117, 118, 119, 120, 121, 122, 123, 124, 125, 126, 127, 128, 40,  60,  61,  62,  20,  21,  22,
    23,  24,  25,  26,  27,  28,  29,  30,  31,  32,  0,   33,  34,  35,  0,   36,  37,  63,  0,
    0,   64,  65,  66,  67,  68,  69,  70,  0,   0,   0,   71,  0,   0,   0,   0,   236, 73,  74,
    75,  76,  77,  39,  78,  79,  0,   0,   0,   80,  81,  82,  83,  0,   0,   0,   84,  85,  86,
    87,  88,  89,  90,  91,  92,  93,  0,   94,  95,  96,  97,  98,  99,  100, 101, 102, 103, 104,
    105, 106, 107, 108, 0,   0,   109, 110, 111, 112, 113, 114, 115, 0,   116, 117, 118, 119, 120,
    121, 122, 123, 124, 125, 126, 127, 128, 40,  60,  61,  62,  20,  21,  22,  23,  24,  25,  26,
    27,  28,  29,  30,  31,  32,  0,   33,  34,  35,  0,   36,  37,  63,  0,   0,   64,  65,  66,
    67,  68,  69,  70,  0,   0,   0,   71,  0,   0,   0,   0,   239, 73,  74,  75,  76,  77,  240,
    78,  79,  0,   0,   0,   80,  81,  82,  83,  0,   0,   0,   84,  85,  86,  87,  88,  89,  90,
    91,  92,  93,  0,   94,  95,  96,  97,  98,  99,  100, 101, 102, 103, 104, 105, 106, 107, 108,
    0,   0,   109, 110, 111, 112, 113, 114, 115, 0,   116, 117, 118, 119, 120, 121, 122, 123, 124,
    125, 126, 127, 128, 40,  366, 367, 368, 0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,
    0,   0,   0,   0,   0,   0,   0,   0,   0,   369, 0,   0,   370, 371, 372, 373, 374, 375, 376,
    0,   0,   0,   377, 0,   0,   0,   0,   0,   378, 379, 380, 381, 382, 0,   383, 384, 0,   0,
    0,   385, 386, 387, 388, 0,   0,   0,   389, 390, 391, 0,   392, 393, 394, 395, 396, 397, 0,
    398, 399, 400, 401, 402, 403, 404, 405, 406, 0,   0,   0,   0,   0,   0,   0,   0,   407, 408,
    409, 410, 411, 412, 413, 0,   414, 415, 416, 417, 418, 419, 420, 421, 422, 423, 424, 425, 426,
    20,  21,  22,  23,  24,  25,  26,  27,  28,  29,  30,  31,  32,  0,   33,  34,  35,  0,   36,
    37,  0,   0,   136, 137, 0,   0,   0,   0,   0,   0,   0,   138, 139, 140, 0,   141, 142, 143,
    38,  0,   0,   0,   0,   0,   39,  0,   0,   0,   144, 145, 146, 0,   0,   0,   0,   147, 148,
    149, 0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   150, 0,   0,   0,   0,   0,   0,   0,
    0,   0,   0,   0,   0,   0,   0,   0,   151, 152, 0,   0,   0,   0,   0,   0,   0,   153, 0,
    0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   40,  0,   154, 155, 156, 157, 158, 159,
    160, 161, 162, 163, 164, 165, 166, 167, 168, 169, 170, 171, 172, 173, 136, 137, 0,   0,   0,
    0,   0,   0,   0,   138, 139, 140, 0,   141, 142, 143, 0,   0,   0,   0,   0,   0,   0,   0,
    0,   0,   144, 145, 146, 0,   0,   0,   0,   147, 148, 149, 0,   0,   0,   0,   0,   0,   0,
    0,   0,   0,   150, 0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,
    267, 268, 0,   0,   0,   0,   0,   0,   0,   153, 0,   0,   0,   0,   0,   0,   0,   0,   0,
    0,   0,   0,   0,   0,   154, 155, 156, 157, 158, 159, 160, 161, 162, 163, 164, 165, 166, 167,
    168, 169, 170, 171, 172, 173, 253, 254, 0,   0,   0,   0,   0,   0,   0,   255, 256, 257, 0,
    258, 259, 260, 0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   261, 262, 263, 0,   0,   0,
    0,   264, 265, 266, 0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   150, 0,   0,   0,   0,
    0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   267, 268, 0,   0,   0,   0,   0,   0,
    0,   153, 0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   154, 155, 156,
    157, 158, 159, 160, 161, 162, 163, 164, 269, 270, 271, 272, 169, 170, 171};

const short ParserGen::yycheck_[] = {
    16,  16,  16,  51,  41,  41,  47,  228, 90,  50,  50,  363, 47,  47,  89,  50,  50,  73,  636,
    90,  47,  367, 368, 50,  73,  6,   7,   8,   9,   10,  11,  12,  13,  14,  15,  16,  17,  18,
    0,   20,  21,  22,  98,  24,  25,  663, 40,  41,  42,  98,  396, 240, 241, 43,  36,  37,  38,
    113, 40,  41,  42,  471, 90,  44,  90,  230, 240, 241, 90,  50,  366, 53,  54,  55,  370, 240,
    241, 90,  60,  61,  62,  491, 22,  379, 380, 44,  438, 10,  240, 241, 386, 44,  388, 7,   17,
    18,  19,  240, 241, 90,  23,  66,  83,  84,  85,  86,  87,  88,  240, 241, 406, 43,  408, 409,
    151, 240, 241, 44,  414, 415, 416, 417, 418, 419, 420, 421, 422, 423, 240, 241, 426, 112, 126,
    240, 241, 240, 241, 17,  434, 240, 241, 371, 124, 125, 126, 127, 44,  443, 378, 240, 241, 381,
    382, 449, 450, 240, 241, 453, 43,  389, 390, 89,  458, 459, 394, 461, 13,  463, 36,  37,  240,
    241, 40,  41,  470, 9,   472, 89,  474, 240, 241, 477, 478, 479, 480, 53,  54,  224, 229, 229,
    399, 400, 60,  61,  229, 229, 492, 3,   4,   495, 237, 237, 229, 89,  44,  89,  427, 89,  89,
    505, 89,  89,  228, 228, 89,  511, 512, 513, 514, 89,  89,  44,  90,  89,  520, 521, 89,  89,
    524, 581, 89,  89,  528, 89,  530, 531, 89,  89,  89,  535, 89,  537, 44,  44,  540, 541, 542,
    44,  544, 44,  546, 44,  8,   549, 550, 551, 552, 83,  84,  85,  86,  87,  88,  44,  44,  44,
    44,  44,  564, 44,  44,  44,  44,  6,   44,  44,  44,  13,  574, 575, 44,  44,  11,  579, 11,
    44,  44,  44,  13,  44,  44,  44,  588, 44,  44,  591, 592, 593, 44,  595, 44,  44,  44,  44,
    600, 44,  602, 13,  43,  43,  606, 43,  43,  13,  44,  43,  14,  12,  44,  360, 616, 43,  618,
    619, 44,  43,  374, 375, 376, 43,  16,  44,  43,  13,  13,  631, 43,  15,  634, 43,  43,  43,
    43,  391, 20,  44,  43,  24,  44,  43,  43,  647, 44,  401, 402, 44,  404, 44,  25,  44,  44,
    44,  44,  44,  21,  661, 21,  228, 43,  43,  43,  226, 44,  44,  670, 671, 424, 212, 44,  44,
    44,  44,  44,  44,  44,  44,  44,  241, 44,  635, 44,  44,  44,  44,  44,  44,  44,  44,  44,
    44,  572, 604, 439, 439, 224, 575, -1,  -1,  -1,  -1,  -1,  427, 427, 427, 451, 451, 3,   4,
    5,   6,   7,   8,   9,   10,  11,  12,  13,  14,  15,  16,  17,  18,  -1,  20,  21,  22,  -1,
    24,  25,  26,  -1,  -1,  29,  30,  31,  32,  33,  34,  35,  -1,  -1,  -1,  39,  -1,  -1,  -1,
    -1,  44,  45,  46,  47,  48,  49,  50,  51,  52,  -1,  -1,  -1,  56,  57,  58,  59,  -1,  -1,
    -1,  63,  64,  65,  66,  67,  68,  69,  70,  71,  72,  -1,  74,  75,  76,  77,  78,  79,  80,
    81,  82,  83,  84,  85,  86,  87,  88,  -1,  -1,  91,  92,  93,  94,  95,  96,  97,  -1,  99,
    100, 101, 102, 103, 104, 105, 106, 107, 108, 109, 110, 111, 112, 3,   4,   5,   6,   7,   8,
    9,   10,  11,  12,  13,  14,  15,  16,  17,  18,  -1,  20,  21,  22,  -1,  24,  25,  26,  -1,
    -1,  29,  30,  31,  32,  33,  34,  35,  -1,  -1,  -1,  39,  -1,  -1,  -1,  -1,  44,  45,  46,
    47,  48,  49,  50,  51,  52,  -1,  -1,  -1,  56,  57,  58,  59,  -1,  -1,  -1,  63,  64,  65,
    66,  67,  68,  69,  70,  71,  72,  -1,  74,  75,  76,  77,  78,  79,  80,  81,  82,  83,  84,
    85,  86,  87,  88,  -1,  -1,  91,  92,  93,  94,  95,  96,  97,  -1,  99,  100, 101, 102, 103,
    104, 105, 106, 107, 108, 109, 110, 111, 112, 3,   4,   5,   6,   7,   8,   9,   10,  11,  12,
    13,  14,  15,  16,  17,  18,  -1,  20,  21,  22,  -1,  24,  25,  26,  -1,  -1,  29,  30,  31,
    32,  33,  34,  35,  -1,  -1,  -1,  39,  -1,  -1,  -1,  -1,  44,  45,  46,  47,  48,  49,  50,
    51,  52,  -1,  -1,  -1,  56,  57,  58,  59,  -1,  -1,  -1,  63,  64,  65,  66,  67,  68,  69,
    70,  71,  72,  -1,  74,  75,  76,  77,  78,  79,  80,  81,  82,  83,  84,  85,  86,  87,  88,
    -1,  -1,  91,  92,  93,  94,  95,  96,  97,  -1,  99,  100, 101, 102, 103, 104, 105, 106, 107,
    108, 109, 110, 111, 112, 3,   4,   5,   -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,
    -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  26,  -1,  -1,  29,  30,  31,  32,  33,  34,  35,
    -1,  -1,  -1,  39,  -1,  -1,  -1,  -1,  -1,  45,  46,  47,  48,  49,  -1,  51,  52,  -1,  -1,
    -1,  56,  57,  58,  59,  -1,  -1,  -1,  63,  64,  65,  -1,  67,  68,  69,  70,  71,  72,  -1,
    74,  75,  76,  77,  78,  79,  80,  81,  82,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  91,  92,
    93,  94,  95,  96,  97,  -1,  99,  100, 101, 102, 103, 104, 105, 106, 107, 108, 109, 110, 111,
    6,   7,   8,   9,   10,  11,  12,  13,  14,  15,  16,  17,  18,  -1,  20,  21,  22,  -1,  24,
    25,  -1,  -1,  27,  28,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  36,  37,  38,  -1,  40,  41,  42,
    44,  -1,  -1,  -1,  -1,  -1,  50,  -1,  -1,  -1,  53,  54,  55,  -1,  -1,  -1,  -1,  60,  61,
    62,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  73,  -1,  -1,  -1,  -1,  -1,  -1,  -1,
    -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  89,  90,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  98,  -1,
    -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  112, -1,  113, 114, 115, 116, 117, 118,
    119, 120, 121, 122, 123, 124, 125, 126, 127, 128, 129, 130, 131, 132, 27,  28,  -1,  -1,  -1,
    -1,  -1,  -1,  -1,  36,  37,  38,  -1,  40,  41,  42,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,
    -1,  -1,  53,  54,  55,  -1,  -1,  -1,  -1,  60,  61,  62,  -1,  -1,  -1,  -1,  -1,  -1,  -1,
    -1,  -1,  -1,  73,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,
    89,  90,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  98,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,
    -1,  -1,  -1,  -1,  -1,  113, 114, 115, 116, 117, 118, 119, 120, 121, 122, 123, 124, 125, 126,
    127, 128, 129, 130, 131, 132, 27,  28,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  36,  37,  38,  -1,
    40,  41,  42,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  53,  54,  55,  -1,  -1,  -1,
    -1,  60,  61,  62,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  73,  -1,  -1,  -1,  -1,
    -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  89,  90,  -1,  -1,  -1,  -1,  -1,  -1,
    -1,  98,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  113, 114, 115,
    116, 117, 118, 119, 120, 121, 122, 123, 124, 125, 126, 127, 128, 129, 130};

const short ParserGen::yystos_[] = {
    0,   10,  17,  18,  19,  23,  279, 90,  270, 89,  175, 270, 270, 90,  273, 0,   271, 90,  176,
    274, 6,   7,   8,   9,   10,  11,  12,  13,  14,  15,  16,  17,  18,  20,  21,  22,  24,  25,
    44,  50,  112, 138, 139, 141, 142, 147, 83,  84,  85,  86,  87,  88,  177, 178, 179, 180, 181,
    182, 183, 43,  3,   4,   5,   26,  29,  30,  31,  32,  33,  34,  35,  39,  44,  45,  46,  47,
    48,  49,  51,  52,  56,  57,  58,  59,  63,  64,  65,  66,  67,  68,  69,  70,  71,  72,  74,
    75,  76,  77,  78,  79,  80,  81,  82,  83,  84,  85,  86,  87,  88,  91,  92,  93,  94,  95,
    96,  97,  99,  100, 101, 102, 103, 104, 105, 106, 107, 108, 109, 110, 111, 137, 139, 140, 141,
    142, 143, 278, 27,  28,  36,  37,  38,  40,  41,  42,  53,  54,  55,  60,  61,  62,  73,  89,
    90,  98,  113, 114, 115, 116, 117, 118, 119, 120, 121, 122, 123, 124, 125, 126, 127, 128, 129,
    130, 131, 132, 148, 149, 150, 151, 152, 153, 154, 155, 156, 157, 158, 159, 160, 161, 162, 163,
    164, 165, 166, 167, 168, 169, 170, 171, 172, 174, 272, 90,  152, 154, 155, 156, 186, 90,  90,
    186, 280, 281, 44,  36,  37,  40,  41,  53,  54,  60,  61,  90,  275, 276, 159, 268, 173, 44,
    184, 22,  7,   90,  176, 66,  268, 43,  44,  143, 146, 44,  50,  135, 137, 139, 140, 141, 144,
    186, 160, 73,  98,  277, 159, 27,  28,  36,  37,  38,  40,  41,  42,  53,  54,  55,  60,  61,
    62,  89,  90,  124, 125, 126, 127, 148, 149, 150, 151, 153, 157, 158, 160, 162, 163, 164, 166,
    167, 168, 185, 188, 190, 191, 193, 194, 195, 196, 197, 198, 199, 200, 201, 202, 203, 204, 205,
    206, 207, 208, 209, 210, 211, 212, 214, 215, 216, 217, 218, 219, 220, 221, 222, 223, 224, 225,
    226, 227, 228, 229, 230, 231, 232, 233, 234, 235, 236, 237, 238, 239, 240, 241, 242, 243, 244,
    245, 246, 247, 248, 249, 250, 251, 252, 253, 254, 255, 256, 257, 258, 259, 260, 185, 44,  17,
    44,  169, 187, 188, 267, 3,   4,   5,   26,  29,  30,  31,  32,  33,  34,  35,  39,  45,  46,
    47,  48,  49,  51,  52,  56,  57,  58,  59,  63,  64,  65,  67,  68,  69,  70,  71,  72,  74,
    75,  76,  77,  78,  79,  80,  81,  82,  91,  92,  93,  94,  95,  96,  97,  99,  100, 101, 102,
    103, 104, 105, 106, 107, 108, 109, 110, 111, 192, 155, 267, 43,  187, 190, 190, 89,  189, 187,
    189, 89,  89,  280, 280, 280, 89,  189, 187, 187, 189, 189, 89,  89,  89,  187, 89,  187, 189,
    189, 280, 89,  89,  189, 89,  190, 89,  213, 280, 213, 213, 280, 280, 89,  280, 89,  187, 89,
    187, 187, 89,  89,  89,  89,  187, 187, 187, 187, 187, 187, 187, 187, 187, 187, 280, 89,  187,
    44,  136, 137, 139, 141, 142, 145, 44,  44,  44,  44,  187, 44,  44,  44,  267, 159, 13,  9,
    8,   187, 44,  44,  44,  44,  44,  187, 187, 159, 44,  187, 44,  44,  44,  6,   265, 187, 187,
    44,  187, 44,  187, 44,  13,  44,  44,  11,  11,  187, 265, 187, 44,  187, 44,  44,  187, 187,
    187, 187, 44,  44,  44,  44,  44,  44,  44,  44,  44,  44,  265, 187, 44,  187, 187, 43,  43,
    187, 187, 187, 187, 187, 187, 43,  187, 187, 13,  187, 187, 43,  187, 187, 187, 187, 187, 13,
    187, 187, 187, 187, 187, 187, 13,  187, 43,  44,  44,  14,  261, 12,  263, 263, 43,  187, 269,
    269, 44,  43,  187, 43,  267, 44,  43,  16,  266, 13,  13,  43,  187, 43,  43,  187, 187, 187,
    43,  187, 43,  187, 15,  262, 187, 24,  264, 264, 44,  187, 43,  43,  44,  44,  44,  43,  44,
    187, 20,  187, 187, 44,  44,  44,  44,  43,  43,  43,  44,  44,  44,  187, 25,  187, 261, 262,
    44,  44,  44,  44,  187, 21,  21,  44,  44,  44,  44,  44,  187, 262, 44,  44,  187, 187, 44,
    44,  44,  44,  44,  44,  44,  44,  44};

const short ParserGen::yyr1_[] = {
    0,   134, 279, 279, 279, 279, 279, 175, 176, 176, 281, 280, 177, 177, 177, 177, 177, 177, 183,
    178, 179, 186, 186, 186, 186, 180, 181, 182, 184, 184, 144, 144, 185, 185, 185, 185, 185, 185,
    185, 185, 185, 185, 185, 185, 185, 185, 185, 185, 185, 185, 185, 185, 185, 185, 185, 185, 185,
    185, 185, 185, 185, 185, 185, 185, 185, 135, 135, 135, 135, 270, 271, 271, 147, 272, 138, 138,
    138, 141, 137, 137, 137, 137, 137, 137, 139, 139, 139, 139, 139, 139, 139, 139, 139, 139, 139,
    139, 139, 139, 139, 139, 139, 139, 140, 140, 140, 140, 140, 140, 140, 140, 140, 140, 140, 140,
    140, 140, 140, 140, 140, 140, 140, 140, 140, 140, 140, 140, 140, 140, 140, 140, 140, 140, 140,
    140, 140, 140, 140, 140, 140, 140, 140, 140, 140, 140, 140, 140, 140, 140, 140, 140, 140, 140,
    140, 140, 140, 140, 140, 140, 140, 140, 140, 140, 140, 140, 160, 160, 160, 161, 174, 162, 163,
    164, 166, 167, 168, 148, 149, 150, 151, 153, 157, 158, 152, 152, 152, 152, 154, 154, 154, 154,
    155, 155, 155, 155, 156, 156, 156, 156, 165, 165, 169, 169, 169, 169, 169, 169, 169, 169, 169,
    169, 169, 169, 169, 169, 169, 169, 169, 169, 169, 169, 169, 267, 267, 187, 187, 189, 188, 188,
    188, 188, 188, 188, 188, 188, 190, 191, 192, 192, 145, 136, 136, 136, 136, 142, 193, 193, 193,
    193, 193, 193, 193, 193, 193, 193, 193, 193, 193, 193, 193, 193, 193, 194, 195, 246, 247, 248,
    249, 250, 251, 252, 253, 254, 255, 256, 257, 258, 259, 260, 196, 196, 196, 197, 198, 199, 203,
    203, 203, 203, 203, 203, 203, 203, 203, 203, 203, 203, 203, 203, 203, 203, 203, 203, 203, 203,
    203, 203, 204, 263, 263, 264, 264, 205, 206, 269, 269, 269, 207, 208, 265, 265, 209, 216, 226,
    266, 266, 213, 210, 211, 212, 214, 215, 217, 218, 219, 220, 221, 222, 223, 224, 225, 277, 277,
    275, 273, 274, 274, 276, 276, 276, 276, 276, 276, 276, 276, 278, 278, 200, 200, 201, 202, 159,
    159, 170, 170, 171, 268, 268, 172, 173, 173, 146, 143, 143, 143, 143, 143, 227, 227, 227, 227,
    227, 227, 227, 228, 229, 230, 231, 232, 233, 234, 235, 235, 235, 235, 235, 235, 235, 235, 235,
    235, 261, 261, 262, 262, 236, 237, 238, 239, 240, 241, 242, 243, 244, 245};

const signed char ParserGen::yyr2_[] = {
    0,  2, 2, 2, 2, 2, 2, 3, 0, 4, 0, 2,  1,  1, 1,  1, 1, 1, 5, 3, 7, 1, 1, 1, 1, 2, 2, 4, 0, 2,
    2,  2, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,  1,  1, 1,  1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,
    1,  1, 1, 1, 1, 1, 1, 1, 1, 3, 0, 2,  2,  1, 1,  1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,
    1,  1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,  1,  1, 1,  1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,
    1,  1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,  1,  1, 1,  1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,
    1,  1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,  1,  1, 1,  1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,
    1,  1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,  1,  1, 1,  1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,
    1,  1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 0,  2,  1, 1,  4, 1, 1, 1, 1, 1, 1, 1, 1, 3, 3, 0, 2, 2, 1,
    1,  1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,  1,  1, 1,  1, 1, 1, 1, 1, 1, 4, 4, 4, 4, 7, 4, 4, 4, 7,
    4,  7, 8, 7, 7, 4, 7, 7, 1, 1, 1, 4,  4,  6, 1,  1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,
    1,  1, 1, 1, 1, 1, 6, 0, 2, 0, 2, 11, 10, 0, 1,  2, 8, 8, 0, 2, 8, 8, 8, 0, 2, 7, 4, 4, 4, 11,
    11, 7, 4, 4, 7, 8, 8, 8, 4, 4, 1, 1,  4,  3, 0,  2, 1, 1, 1, 1, 1, 1, 1, 1, 2, 2, 1, 1, 6, 6,
    1,  1, 1, 1, 3, 0, 2, 3, 0, 2, 2, 1,  1,  1, 1,  1, 1, 1, 1, 1, 1, 1, 1, 4, 4, 4, 4, 4, 4, 4,
    1,  1, 1, 1, 1, 1, 1, 1, 1, 1, 0, 2,  0,  2, 11, 4, 4, 4, 4, 4, 4, 4, 4, 4};


// YYTNAME[SYMBOL-NUM] -- String name of the symbol SYMBOL-NUM.
// First, the terminals, then, starting at \a yyntokens_, nonterminals.
const char* const ParserGen::yytname_[] = {"\"EOF\"",
                                           "error",
                                           "$undefined",
                                           "ABS",
                                           "ADD",
                                           "AND",
                                           "\"chars argument\"",
                                           "\"coll argument\"",
                                           "\"date argument\"",
                                           "\"dateString argument\"",
                                           "\"filter\"",
                                           "\"find argument\"",
                                           "\"format argument\"",
                                           "\"input argument\"",
                                           "\"onError argument\"",
                                           "\"onNull argument\"",
                                           "\"options argument\"",
                                           "\"pipeline argument\"",
                                           "\"q\"",
                                           "\"query\"",
                                           "\"regex argument\"",
                                           "\"replacement argument\"",
                                           "\"size argument\"",
                                           "\"sort argument\"",
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
                                           "\"-1 (decimal)\"",
                                           "\"1 (decimal)\"",
                                           "\"zero (decimal)\"",
                                           "DIVIDE",
                                           "\"-1 (double)\"",
                                           "\"1 (double)\"",
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
                                           "\"-1 (int)\"",
                                           "\"1 (int)\"",
                                           "\"zero (int)\"",
                                           "LITERAL",
                                           "LN",
                                           "LOG",
                                           "LOGTEN",
                                           "\"-1 (long)\"",
                                           "\"1 (long)\"",
                                           "\"zero (long)\"",
                                           "LT",
                                           "LTE",
                                           "LTRIM",
                                           "META",
                                           "MOD",
                                           "MULTIPLY",
                                           "NE",
                                           "NOT",
                                           "OR",
                                           "POW",
                                           "\"randVal\"",
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
                                           "\"textScore\"",
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
                                           "\"arbitrary integer\"",
                                           "\"arbitrary long\"",
                                           "\"arbitrary double\"",
                                           "\"arbitrary decimal\"",
                                           "\"Timestamp\"",
                                           "\"minKey\"",
                                           "\"maxKey\"",
                                           "\"$-prefixed string\"",
                                           "\"$$-prefixed string\"",
                                           "\"$-prefixed fieldname\"",
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
                                           "fieldPath",
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
                                           "variable",
                                           "pipeline",
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
                                           "sortSpecs",
                                           "specList",
                                           "metaSort",
                                           "oneOrNegOne",
                                           "metaSortKeyword",
                                           "sortSpec",
                                           "start",
                                           "START_ORDERED_OBJECT",
                                           "$@1",
                                           YY_NULLPTR};

#if YYDEBUG
const short ParserGen::yyrline_[] = {
    0,    299,  299,  302,  305,  308,  311,  318,  324,  325,  333,  333,  336,  336,  336,  336,
    336,  336,  339,  349,  355,  365,  365,  365,  365,  369,  374,  379,  395,  398,  405,  408,
    414,  415,  416,  417,  418,  419,  420,  421,  422,  423,  424,  425,  428,  431,  434,  437,
    440,  443,  446,  449,  452,  455,  458,  461,  464,  467,  470,  473,  476,  479,  480,  481,
    482,  491,  491,  491,  491,  495,  501,  504,  510,  516,  521,  521,  521,  525,  533,  536,
    539,  542,  545,  548,  557,  560,  563,  566,  569,  572,  575,  578,  581,  584,  587,  590,
    593,  596,  599,  602,  605,  608,  616,  619,  622,  625,  628,  631,  634,  637,  640,  643,
    646,  649,  652,  655,  658,  661,  664,  667,  670,  673,  676,  679,  682,  685,  688,  691,
    694,  697,  700,  703,  706,  709,  712,  715,  718,  721,  724,  727,  730,  733,  736,  739,
    742,  745,  748,  751,  754,  757,  760,  763,  766,  769,  772,  775,  778,  781,  784,  787,
    790,  793,  796,  799,  806,  811,  814,  820,  828,  837,  843,  849,  855,  861,  867,  873,
    879,  885,  891,  897,  903,  909,  915,  918,  921,  924,  930,  933,  936,  939,  945,  948,
    951,  954,  960,  963,  966,  969,  975,  978,  984,  985,  986,  987,  988,  989,  990,  991,
    992,  993,  994,  995,  996,  997,  998,  999,  1000, 1001, 1002, 1003, 1004, 1011, 1012, 1019,
    1019, 1023, 1028, 1028, 1028, 1028, 1028, 1028, 1029, 1029, 1035, 1043, 1049, 1052, 1059, 1066,
    1066, 1066, 1066, 1070, 1076, 1076, 1076, 1076, 1076, 1076, 1076, 1076, 1076, 1076, 1076, 1076,
    1076, 1077, 1077, 1077, 1077, 1081, 1088, 1094, 1099, 1104, 1110, 1115, 1120, 1125, 1131, 1136,
    1142, 1151, 1157, 1163, 1168, 1174, 1180, 1180, 1180, 1184, 1191, 1198, 1205, 1205, 1205, 1205,
    1205, 1205, 1205, 1206, 1206, 1206, 1206, 1206, 1206, 1206, 1206, 1207, 1207, 1207, 1207, 1207,
    1207, 1207, 1211, 1221, 1224, 1230, 1233, 1239, 1248, 1257, 1260, 1263, 1269, 1280, 1291, 1294,
    1300, 1308, 1316, 1324, 1327, 1332, 1341, 1347, 1353, 1359, 1369, 1379, 1386, 1393, 1400, 1408,
    1416, 1424, 1432, 1438, 1444, 1447, 1453, 1459, 1464, 1467, 1474, 1477, 1480, 1483, 1486, 1489,
    1492, 1495, 1500, 1502, 1508, 1508, 1512, 1519, 1526, 1526, 1530, 1530, 1534, 1540, 1541, 1548,
    1554, 1557, 1564, 1571, 1572, 1573, 1574, 1575, 1578, 1578, 1578, 1578, 1578, 1578, 1578, 1580,
    1585, 1590, 1595, 1600, 1605, 1610, 1616, 1617, 1618, 1619, 1620, 1621, 1622, 1623, 1624, 1625,
    1630, 1633, 1640, 1643, 1649, 1659, 1664, 1669, 1674, 1679, 1684, 1689, 1694, 1699};

// Print the state stack on the debug stream.
void ParserGen::yystack_print_() {
    *yycdebug_ << "Stack now";
    for (stack_type::const_iterator i = yystack_.begin(), i_end = yystack_.end(); i != i_end; ++i)
        *yycdebug_ << ' ' << int(i->state);
    *yycdebug_ << '\n';
}

// Report on the debug stream that the rule \a yyrule is going to be reduced.
void ParserGen::yy_reduce_print_(int yyrule) {
    int yylno = yyrline_[yyrule];
    int yynrhs = yyr2_[yyrule];
    // Print the symbols being reduced, and their result.
    *yycdebug_ << "Reducing stack by rule " << yyrule - 1 << " (line " << yylno << "):\n";
    // The symbols being reduced.
    for (int yyi = 0; yyi < yynrhs; yyi++)
        YY_SYMBOL_PRINT("   $" << yyi + 1 << " =", yystack_[(yynrhs) - (yyi + 1)]);
}
#endif  // YYDEBUG


#line 57 "src/mongo/db/cst/grammar.yy"
}  // namespace mongo
#line 5887 "src/mongo/db/cst/parser_gen.cpp"

#line 1703 "src/mongo/db/cst/grammar.yy"
