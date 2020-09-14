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
        case 115:  // "BinData"
            value.YY_MOVE_OR_COPY<BSONBinData>(YY_MOVE(that.value));
            break;

        case 122:  // "Code"
            value.YY_MOVE_OR_COPY<BSONCode>(YY_MOVE(that.value));
            break;

        case 124:  // "CodeWScope"
            value.YY_MOVE_OR_COPY<BSONCodeWScope>(YY_MOVE(that.value));
            break;

        case 121:  // "dbPointer"
            value.YY_MOVE_OR_COPY<BSONDBRef>(YY_MOVE(that.value));
            break;

        case 120:  // "regex"
            value.YY_MOVE_OR_COPY<BSONRegEx>(YY_MOVE(that.value));
            break;

        case 123:  // "Symbol"
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
        case 270:  // match
        case 271:  // predicates
        case 272:  // compoundMatchExprs
        case 273:  // predValue
        case 274:  // additionalExprs
        case 280:  // sortSpecs
        case 281:  // specList
        case 282:  // metaSort
        case 283:  // oneOrNegOne
        case 284:  // metaSortKeyword
            value.YY_MOVE_OR_COPY<CNode>(YY_MOVE(that.value));
            break;

        case 136:  // projectionFieldname
        case 137:  // expressionFieldname
        case 138:  // stageAsUserFieldname
        case 139:  // predFieldname
        case 140:  // argAsUserFieldname
        case 141:  // aggExprAsUserFieldname
        case 142:  // invariableUserFieldname
        case 143:  // idAsUserFieldname
        case 144:  // valueFieldname
        case 279:  // logicalExprField
            value.YY_MOVE_OR_COPY<CNode::Fieldname>(YY_MOVE(that.value));
            break;

        case 118:  // "Date"
            value.YY_MOVE_OR_COPY<Date_t>(YY_MOVE(that.value));
            break;

        case 128:  // "arbitrary decimal"
            value.YY_MOVE_OR_COPY<Decimal128>(YY_MOVE(that.value));
            break;

        case 117:  // "ObjectID"
            value.YY_MOVE_OR_COPY<OID>(YY_MOVE(that.value));
            break;

        case 129:  // "Timestamp"
            value.YY_MOVE_OR_COPY<Timestamp>(YY_MOVE(that.value));
            break;

        case 131:  // "maxKey"
            value.YY_MOVE_OR_COPY<UserMaxKey>(YY_MOVE(that.value));
            break;

        case 130:  // "minKey"
            value.YY_MOVE_OR_COPY<UserMinKey>(YY_MOVE(that.value));
            break;

        case 119:  // "null"
            value.YY_MOVE_OR_COPY<UserNull>(YY_MOVE(that.value));
            break;

        case 116:  // "undefined"
            value.YY_MOVE_OR_COPY<UserUndefined>(YY_MOVE(that.value));
            break;

        case 127:  // "arbitrary double"
            value.YY_MOVE_OR_COPY<double>(YY_MOVE(that.value));
            break;

        case 125:  // "arbitrary integer"
            value.YY_MOVE_OR_COPY<int>(YY_MOVE(that.value));
            break;

        case 126:  // "arbitrary long"
            value.YY_MOVE_OR_COPY<long long>(YY_MOVE(that.value));
            break;

        case 145:  // projectField
        case 146:  // expressionField
        case 147:  // valueField
        case 261:  // onErrorArg
        case 262:  // onNullArg
        case 263:  // formatArg
        case 264:  // timezoneArg
        case 265:  // charsArg
        case 266:  // optionsArg
        case 275:  // predicate
        case 276:  // logicalExpr
        case 277:  // operatorExpression
        case 278:  // notExpr
        case 285:  // sortSpec
            value.YY_MOVE_OR_COPY<std::pair<CNode::Fieldname, CNode>>(YY_MOVE(that.value));
            break;

        case 113:  // "fieldname"
        case 114:  // "string"
        case 132:  // "$-prefixed string"
        case 133:  // "$$-prefixed string"
        case 134:  // "$-prefixed fieldname"
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
        case 115:  // "BinData"
            value.move<BSONBinData>(YY_MOVE(that.value));
            break;

        case 122:  // "Code"
            value.move<BSONCode>(YY_MOVE(that.value));
            break;

        case 124:  // "CodeWScope"
            value.move<BSONCodeWScope>(YY_MOVE(that.value));
            break;

        case 121:  // "dbPointer"
            value.move<BSONDBRef>(YY_MOVE(that.value));
            break;

        case 120:  // "regex"
            value.move<BSONRegEx>(YY_MOVE(that.value));
            break;

        case 123:  // "Symbol"
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
        case 270:  // match
        case 271:  // predicates
        case 272:  // compoundMatchExprs
        case 273:  // predValue
        case 274:  // additionalExprs
        case 280:  // sortSpecs
        case 281:  // specList
        case 282:  // metaSort
        case 283:  // oneOrNegOne
        case 284:  // metaSortKeyword
            value.move<CNode>(YY_MOVE(that.value));
            break;

        case 136:  // projectionFieldname
        case 137:  // expressionFieldname
        case 138:  // stageAsUserFieldname
        case 139:  // predFieldname
        case 140:  // argAsUserFieldname
        case 141:  // aggExprAsUserFieldname
        case 142:  // invariableUserFieldname
        case 143:  // idAsUserFieldname
        case 144:  // valueFieldname
        case 279:  // logicalExprField
            value.move<CNode::Fieldname>(YY_MOVE(that.value));
            break;

        case 118:  // "Date"
            value.move<Date_t>(YY_MOVE(that.value));
            break;

        case 128:  // "arbitrary decimal"
            value.move<Decimal128>(YY_MOVE(that.value));
            break;

        case 117:  // "ObjectID"
            value.move<OID>(YY_MOVE(that.value));
            break;

        case 129:  // "Timestamp"
            value.move<Timestamp>(YY_MOVE(that.value));
            break;

        case 131:  // "maxKey"
            value.move<UserMaxKey>(YY_MOVE(that.value));
            break;

        case 130:  // "minKey"
            value.move<UserMinKey>(YY_MOVE(that.value));
            break;

        case 119:  // "null"
            value.move<UserNull>(YY_MOVE(that.value));
            break;

        case 116:  // "undefined"
            value.move<UserUndefined>(YY_MOVE(that.value));
            break;

        case 127:  // "arbitrary double"
            value.move<double>(YY_MOVE(that.value));
            break;

        case 125:  // "arbitrary integer"
            value.move<int>(YY_MOVE(that.value));
            break;

        case 126:  // "arbitrary long"
            value.move<long long>(YY_MOVE(that.value));
            break;

        case 145:  // projectField
        case 146:  // expressionField
        case 147:  // valueField
        case 261:  // onErrorArg
        case 262:  // onNullArg
        case 263:  // formatArg
        case 264:  // timezoneArg
        case 265:  // charsArg
        case 266:  // optionsArg
        case 275:  // predicate
        case 276:  // logicalExpr
        case 277:  // operatorExpression
        case 278:  // notExpr
        case 285:  // sortSpec
            value.move<std::pair<CNode::Fieldname, CNode>>(YY_MOVE(that.value));
            break;

        case 113:  // "fieldname"
        case 114:  // "string"
        case 132:  // "$-prefixed string"
        case 133:  // "$$-prefixed string"
        case 134:  // "$-prefixed fieldname"
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
        case 115:  // "BinData"
            value.copy<BSONBinData>(that.value);
            break;

        case 122:  // "Code"
            value.copy<BSONCode>(that.value);
            break;

        case 124:  // "CodeWScope"
            value.copy<BSONCodeWScope>(that.value);
            break;

        case 121:  // "dbPointer"
            value.copy<BSONDBRef>(that.value);
            break;

        case 120:  // "regex"
            value.copy<BSONRegEx>(that.value);
            break;

        case 123:  // "Symbol"
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
        case 270:  // match
        case 271:  // predicates
        case 272:  // compoundMatchExprs
        case 273:  // predValue
        case 274:  // additionalExprs
        case 280:  // sortSpecs
        case 281:  // specList
        case 282:  // metaSort
        case 283:  // oneOrNegOne
        case 284:  // metaSortKeyword
            value.copy<CNode>(that.value);
            break;

        case 136:  // projectionFieldname
        case 137:  // expressionFieldname
        case 138:  // stageAsUserFieldname
        case 139:  // predFieldname
        case 140:  // argAsUserFieldname
        case 141:  // aggExprAsUserFieldname
        case 142:  // invariableUserFieldname
        case 143:  // idAsUserFieldname
        case 144:  // valueFieldname
        case 279:  // logicalExprField
            value.copy<CNode::Fieldname>(that.value);
            break;

        case 118:  // "Date"
            value.copy<Date_t>(that.value);
            break;

        case 128:  // "arbitrary decimal"
            value.copy<Decimal128>(that.value);
            break;

        case 117:  // "ObjectID"
            value.copy<OID>(that.value);
            break;

        case 129:  // "Timestamp"
            value.copy<Timestamp>(that.value);
            break;

        case 131:  // "maxKey"
            value.copy<UserMaxKey>(that.value);
            break;

        case 130:  // "minKey"
            value.copy<UserMinKey>(that.value);
            break;

        case 119:  // "null"
            value.copy<UserNull>(that.value);
            break;

        case 116:  // "undefined"
            value.copy<UserUndefined>(that.value);
            break;

        case 127:  // "arbitrary double"
            value.copy<double>(that.value);
            break;

        case 125:  // "arbitrary integer"
            value.copy<int>(that.value);
            break;

        case 126:  // "arbitrary long"
            value.copy<long long>(that.value);
            break;

        case 145:  // projectField
        case 146:  // expressionField
        case 147:  // valueField
        case 261:  // onErrorArg
        case 262:  // onNullArg
        case 263:  // formatArg
        case 264:  // timezoneArg
        case 265:  // charsArg
        case 266:  // optionsArg
        case 275:  // predicate
        case 276:  // logicalExpr
        case 277:  // operatorExpression
        case 278:  // notExpr
        case 285:  // sortSpec
            value.copy<std::pair<CNode::Fieldname, CNode>>(that.value);
            break;

        case 113:  // "fieldname"
        case 114:  // "string"
        case 132:  // "$-prefixed string"
        case 133:  // "$$-prefixed string"
        case 134:  // "$-prefixed fieldname"
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
        case 115:  // "BinData"
            value.move<BSONBinData>(that.value);
            break;

        case 122:  // "Code"
            value.move<BSONCode>(that.value);
            break;

        case 124:  // "CodeWScope"
            value.move<BSONCodeWScope>(that.value);
            break;

        case 121:  // "dbPointer"
            value.move<BSONDBRef>(that.value);
            break;

        case 120:  // "regex"
            value.move<BSONRegEx>(that.value);
            break;

        case 123:  // "Symbol"
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
        case 270:  // match
        case 271:  // predicates
        case 272:  // compoundMatchExprs
        case 273:  // predValue
        case 274:  // additionalExprs
        case 280:  // sortSpecs
        case 281:  // specList
        case 282:  // metaSort
        case 283:  // oneOrNegOne
        case 284:  // metaSortKeyword
            value.move<CNode>(that.value);
            break;

        case 136:  // projectionFieldname
        case 137:  // expressionFieldname
        case 138:  // stageAsUserFieldname
        case 139:  // predFieldname
        case 140:  // argAsUserFieldname
        case 141:  // aggExprAsUserFieldname
        case 142:  // invariableUserFieldname
        case 143:  // idAsUserFieldname
        case 144:  // valueFieldname
        case 279:  // logicalExprField
            value.move<CNode::Fieldname>(that.value);
            break;

        case 118:  // "Date"
            value.move<Date_t>(that.value);
            break;

        case 128:  // "arbitrary decimal"
            value.move<Decimal128>(that.value);
            break;

        case 117:  // "ObjectID"
            value.move<OID>(that.value);
            break;

        case 129:  // "Timestamp"
            value.move<Timestamp>(that.value);
            break;

        case 131:  // "maxKey"
            value.move<UserMaxKey>(that.value);
            break;

        case 130:  // "minKey"
            value.move<UserMinKey>(that.value);
            break;

        case 119:  // "null"
            value.move<UserNull>(that.value);
            break;

        case 116:  // "undefined"
            value.move<UserUndefined>(that.value);
            break;

        case 127:  // "arbitrary double"
            value.move<double>(that.value);
            break;

        case 125:  // "arbitrary integer"
            value.move<int>(that.value);
            break;

        case 126:  // "arbitrary long"
            value.move<long long>(that.value);
            break;

        case 145:  // projectField
        case 146:  // expressionField
        case 147:  // valueField
        case 261:  // onErrorArg
        case 262:  // onNullArg
        case 263:  // formatArg
        case 264:  // timezoneArg
        case 265:  // charsArg
        case 266:  // optionsArg
        case 275:  // predicate
        case 276:  // logicalExpr
        case 277:  // operatorExpression
        case 278:  // notExpr
        case 285:  // sortSpec
            value.move<std::pair<CNode::Fieldname, CNode>>(that.value);
            break;

        case 113:  // "fieldname"
        case 114:  // "string"
        case 132:  // "$-prefixed string"
        case 133:  // "$$-prefixed string"
        case 134:  // "$-prefixed fieldname"
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
                case 115:  // "BinData"
                    yylhs.value.emplace<BSONBinData>();
                    break;

                case 122:  // "Code"
                    yylhs.value.emplace<BSONCode>();
                    break;

                case 124:  // "CodeWScope"
                    yylhs.value.emplace<BSONCodeWScope>();
                    break;

                case 121:  // "dbPointer"
                    yylhs.value.emplace<BSONDBRef>();
                    break;

                case 120:  // "regex"
                    yylhs.value.emplace<BSONRegEx>();
                    break;

                case 123:  // "Symbol"
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
                case 270:  // match
                case 271:  // predicates
                case 272:  // compoundMatchExprs
                case 273:  // predValue
                case 274:  // additionalExprs
                case 280:  // sortSpecs
                case 281:  // specList
                case 282:  // metaSort
                case 283:  // oneOrNegOne
                case 284:  // metaSortKeyword
                    yylhs.value.emplace<CNode>();
                    break;

                case 136:  // projectionFieldname
                case 137:  // expressionFieldname
                case 138:  // stageAsUserFieldname
                case 139:  // predFieldname
                case 140:  // argAsUserFieldname
                case 141:  // aggExprAsUserFieldname
                case 142:  // invariableUserFieldname
                case 143:  // idAsUserFieldname
                case 144:  // valueFieldname
                case 279:  // logicalExprField
                    yylhs.value.emplace<CNode::Fieldname>();
                    break;

                case 118:  // "Date"
                    yylhs.value.emplace<Date_t>();
                    break;

                case 128:  // "arbitrary decimal"
                    yylhs.value.emplace<Decimal128>();
                    break;

                case 117:  // "ObjectID"
                    yylhs.value.emplace<OID>();
                    break;

                case 129:  // "Timestamp"
                    yylhs.value.emplace<Timestamp>();
                    break;

                case 131:  // "maxKey"
                    yylhs.value.emplace<UserMaxKey>();
                    break;

                case 130:  // "minKey"
                    yylhs.value.emplace<UserMinKey>();
                    break;

                case 119:  // "null"
                    yylhs.value.emplace<UserNull>();
                    break;

                case 116:  // "undefined"
                    yylhs.value.emplace<UserUndefined>();
                    break;

                case 127:  // "arbitrary double"
                    yylhs.value.emplace<double>();
                    break;

                case 125:  // "arbitrary integer"
                    yylhs.value.emplace<int>();
                    break;

                case 126:  // "arbitrary long"
                    yylhs.value.emplace<long long>();
                    break;

                case 145:  // projectField
                case 146:  // expressionField
                case 147:  // valueField
                case 261:  // onErrorArg
                case 262:  // onNullArg
                case 263:  // formatArg
                case 264:  // timezoneArg
                case 265:  // charsArg
                case 266:  // optionsArg
                case 275:  // predicate
                case 276:  // logicalExpr
                case 277:  // operatorExpression
                case 278:  // notExpr
                case 285:  // sortSpec
                    yylhs.value.emplace<std::pair<CNode::Fieldname, CNode>>();
                    break;

                case 113:  // "fieldname"
                case 114:  // "string"
                case 132:  // "$-prefixed string"
                case 133:  // "$$-prefixed string"
                case 134:  // "$-prefixed fieldname"
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
#line 304 "src/mongo/db/cst/grammar.yy"
                    {
                        *cst = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 1779 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 3:
#line 307 "src/mongo/db/cst/grammar.yy"
                    {
                        *cst = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 1787 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 4:
#line 310 "src/mongo/db/cst/grammar.yy"
                    {
                        *cst = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 1795 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 5:
#line 313 "src/mongo/db/cst/grammar.yy"
                    {
                        *cst = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 1803 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 6:
#line 316 "src/mongo/db/cst/grammar.yy"
                    {
                        *cst = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 1811 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 7:
#line 323 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[1].value.as<CNode>());
                    }
#line 1819 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 8:
#line 329 "src/mongo/db/cst/grammar.yy"
                    {
                    }
#line 1825 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 9:
#line 330 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() =
                            CNode{CNode::ArrayChildren{YY_MOVE(yystack_[2].value.as<CNode>())}};
                    }
#line 1833 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 10:
#line 338 "src/mongo/db/cst/grammar.yy"
                    {
                        lexer.sortObjTokens();
                    }
#line 1839 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 12:
#line 341 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 1845 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 13:
#line 341 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 1851 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 14:
#line 341 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 1857 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 15:
#line 341 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 1863 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 16:
#line 341 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 1869 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 17:
#line 341 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 1875 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 18:
#line 344 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{std::pair{
                            KeyFieldname::sample,
                            CNode{CNode::ObjectChildren{
                                {KeyFieldname::sizeArg, YY_MOVE(yystack_[1].value.as<CNode>())},
                            }}}}};
                    }
#line 1887 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 19:
#line 354 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            std::pair{KeyFieldname::inhibitOptimization, CNode::noopLeaf()}}};
                    }
#line 1895 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 20:
#line 360 "src/mongo/db/cst/grammar.yy"
                    {
                        auto pipeline = YY_MOVE(yystack_[1].value.as<CNode>());
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{std::pair{
                            KeyFieldname::unionWith,
                            CNode{CNode::ObjectChildren{
                                {KeyFieldname::collArg, YY_MOVE(yystack_[3].value.as<CNode>())},
                                {KeyFieldname::pipelineArg, std::move(pipeline)}}}}}};
                    }
#line 1908 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 21:
#line 370 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 1914 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 22:
#line 370 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 1920 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 23:
#line 370 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 1926 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 24:
#line 370 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 1932 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 25:
#line 374 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            std::pair{KeyFieldname::skip, YY_MOVE(yystack_[0].value.as<CNode>())}}};
                    }
#line 1940 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 26:
#line 379 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{std::pair{
                            KeyFieldname::limit, YY_MOVE(yystack_[0].value.as<CNode>())}}};
                    }
#line 1948 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 27:
#line 384 "src/mongo/db/cst/grammar.yy"
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
#line 1966 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 28:
#line 400 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode::noopLeaf();
                    }
#line 1974 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 29:
#line 403 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[1].value.as<CNode>());
                        yylhs.value.as<CNode>().objectChildren().emplace_back(
                            YY_MOVE(yystack_[0].value.as<std::pair<CNode::Fieldname, CNode>>()));
                    }
#line 1983 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 30:
#line 410 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<std::pair<CNode::Fieldname, CNode>>() = {
                            KeyFieldname::id, YY_MOVE(yystack_[0].value.as<CNode>())};
                    }
#line 1991 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 31:
#line 413 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<std::pair<CNode::Fieldname, CNode>>() = {
                            YY_MOVE(yystack_[1].value.as<CNode::Fieldname>()),
                            YY_MOVE(yystack_[0].value.as<CNode>())};
                    }
#line 1999 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 32:
#line 419 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 2005 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 33:
#line 420 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 2011 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 34:
#line 421 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 2017 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 35:
#line 422 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 2023 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 36:
#line 423 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 2029 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 37:
#line 424 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 2035 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 38:
#line 425 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 2041 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 39:
#line 426 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 2047 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 40:
#line 427 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 2053 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 41:
#line 428 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 2059 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 42:
#line 429 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 2065 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 43:
#line 430 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{NonZeroKey{1}};
                    }
#line 2073 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 44:
#line 433 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{NonZeroKey{-1}};
                    }
#line 2081 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 45:
#line 436 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() =
                            CNode{NonZeroKey{YY_MOVE(yystack_[0].value.as<int>())}};
                    }
#line 2089 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 46:
#line 439 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{KeyValue::intZeroKey};
                    }
#line 2097 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 47:
#line 442 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{NonZeroKey{1ll}};
                    }
#line 2105 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 48:
#line 445 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{NonZeroKey{-1ll}};
                    }
#line 2113 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 49:
#line 448 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() =
                            CNode{NonZeroKey{YY_MOVE(yystack_[0].value.as<long long>())}};
                    }
#line 2121 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 50:
#line 451 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{KeyValue::longZeroKey};
                    }
#line 2129 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 51:
#line 454 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{NonZeroKey{1.0}};
                    }
#line 2137 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 52:
#line 457 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{NonZeroKey{-1.0}};
                    }
#line 2145 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 53:
#line 460 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() =
                            CNode{NonZeroKey{YY_MOVE(yystack_[0].value.as<double>())}};
                    }
#line 2153 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 54:
#line 463 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{KeyValue::doubleZeroKey};
                    }
#line 2161 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 55:
#line 466 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{NonZeroKey{1.0}};
                    }
#line 2169 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 56:
#line 469 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{NonZeroKey{-1.0}};
                    }
#line 2177 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 57:
#line 472 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() =
                            CNode{NonZeroKey{YY_MOVE(yystack_[0].value.as<Decimal128>())}};
                    }
#line 2185 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 58:
#line 475 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{KeyValue::decimalZeroKey};
                    }
#line 2193 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 59:
#line 478 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{KeyValue::trueKey};
                    }
#line 2201 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 60:
#line 481 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{KeyValue::falseKey};
                    }
#line 2209 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 61:
#line 484 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 2215 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 62:
#line 485 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 2221 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 63:
#line 486 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 2227 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 64:
#line 487 "src/mongo/db/cst/grammar.yy"
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
#line 2238 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 65:
#line 496 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() =
                            YY_MOVE(yystack_[0].value.as<CNode::Fieldname>());
                    }
#line 2244 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 66:
#line 496 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() =
                            YY_MOVE(yystack_[0].value.as<CNode::Fieldname>());
                    }
#line 2250 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 67:
#line 496 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() =
                            YY_MOVE(yystack_[0].value.as<CNode::Fieldname>());
                    }
#line 2256 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 68:
#line 496 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() =
                            YY_MOVE(yystack_[0].value.as<CNode::Fieldname>());
                    }
#line 2262 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 69:
#line 500 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[1].value.as<CNode>());
                    }
#line 2270 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 70:
#line 506 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode::noopLeaf();
                    }
#line 2278 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 71:
#line 509 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[1].value.as<CNode>());
                        yylhs.value.as<CNode>().objectChildren().emplace_back(
                            YY_MOVE(yystack_[0].value.as<std::pair<CNode::Fieldname, CNode>>()));
                    }
#line 2287 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 72:
#line 515 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<std::pair<CNode::Fieldname, CNode>>() = {
                            YY_MOVE(yystack_[1].value.as<CNode::Fieldname>()),
                            YY_MOVE(yystack_[0].value.as<CNode>())};
                    }
#line 2295 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 73:
#line 518 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<std::pair<CNode::Fieldname, CNode>>() =
                            YY_MOVE(yystack_[0].value.as<std::pair<CNode::Fieldname, CNode>>());
                    }
#line 2303 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 74:
#line 527 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 2309 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 75:
#line 528 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[1].value.as<CNode>());
                    }
#line 2317 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 76:
#line 534 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode::noopLeaf();
                    }
#line 2325 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 77:
#line 537 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[1].value.as<CNode>());
                        yylhs.value.as<CNode>().objectChildren().emplace_back(
                            YY_MOVE(yystack_[0].value.as<std::pair<CNode::Fieldname, CNode>>()));
                    }
#line 2334 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 78:
#line 544 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<std::pair<CNode::Fieldname, CNode>>() =
                            YY_MOVE(yystack_[0].value.as<std::pair<CNode::Fieldname, CNode>>());
                    }
#line 2340 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 79:
#line 547 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<std::pair<CNode::Fieldname, CNode>>() = std::pair{
                            KeyFieldname::notExpr, YY_MOVE(yystack_[0].value.as<CNode>())};
                    }
#line 2348 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 80:
#line 551 "src/mongo/db/cst/grammar.yy"
                    {
                        auto&& exprs = YY_MOVE(yystack_[1].value.as<CNode>());
                        exprs.objectChildren().emplace_back(
                            YY_MOVE(yystack_[2].value.as<std::pair<CNode::Fieldname, CNode>>()));

                        yylhs.value.as<std::pair<CNode::Fieldname, CNode>>() =
                            std::pair{KeyFieldname::notExpr, std::move(exprs)};
                    }
#line 2359 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 81:
#line 560 "src/mongo/db/cst/grammar.yy"
                    {
                        auto&& children = YY_MOVE(yystack_[1].value.as<CNode>());
                        children.arrayChildren().emplace_back(
                            YY_MOVE(yystack_[2].value.as<CNode>()));
                        yylhs.value.as<std::pair<CNode::Fieldname, CNode>>() = {
                            YY_MOVE(yystack_[4].value.as<CNode::Fieldname>()), std::move(children)};
                    }
#line 2369 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 82:
#line 568 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = KeyFieldname::andExpr;
                    }
#line 2375 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 83:
#line 569 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = KeyFieldname::orExpr;
                    }
#line 2381 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 84:
#line 570 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = KeyFieldname::norExpr;
                    }
#line 2387 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 85:
#line 573 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ArrayChildren{}};
                    }
#line 2395 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 86:
#line 576 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[1].value.as<CNode>());
                        yylhs.value.as<CNode>().arrayChildren().emplace_back(
                            YY_MOVE(yystack_[0].value.as<CNode>()));
                    }
#line 2404 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 87:
#line 583 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() =
                            YY_MOVE(yystack_[0].value.as<CNode::Fieldname>());
                    }
#line 2410 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 88:
#line 583 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() =
                            YY_MOVE(yystack_[0].value.as<CNode::Fieldname>());
                    }
#line 2416 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 89:
#line 583 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() =
                            YY_MOVE(yystack_[0].value.as<CNode::Fieldname>());
                    }
#line 2422 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 90:
#line 586 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() =
                            UserFieldname{YY_MOVE(yystack_[0].value.as<std::string>())};
                    }
#line 2430 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 91:
#line 594 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() =
                            UserFieldname{"$_internalInhibitOptimization"};
                    }
#line 2438 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 92:
#line 597 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$unionWith"};
                    }
#line 2446 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 93:
#line 600 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$skip"};
                    }
#line 2454 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 94:
#line 603 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$limit"};
                    }
#line 2462 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 95:
#line 606 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$project"};
                    }
#line 2470 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 96:
#line 609 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$sample"};
                    }
#line 2478 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 97:
#line 618 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"coll"};
                    }
#line 2486 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 98:
#line 621 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"pipeline"};
                    }
#line 2494 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 99:
#line 624 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"size"};
                    }
#line 2502 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 100:
#line 627 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"input"};
                    }
#line 2510 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 101:
#line 630 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"to"};
                    }
#line 2518 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 102:
#line 633 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"onError"};
                    }
#line 2526 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 103:
#line 636 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"onNull"};
                    }
#line 2534 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 104:
#line 639 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"dateString"};
                    }
#line 2542 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 105:
#line 642 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"format"};
                    }
#line 2550 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 106:
#line 645 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"timezone"};
                    }
#line 2558 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 107:
#line 648 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"date"};
                    }
#line 2566 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 108:
#line 651 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"chars"};
                    }
#line 2574 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 109:
#line 654 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"regex"};
                    }
#line 2582 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 110:
#line 657 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"options"};
                    }
#line 2590 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 111:
#line 660 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"find"};
                    }
#line 2598 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 112:
#line 663 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"replacement"};
                    }
#line 2606 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 113:
#line 666 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"filter"};
                    }
#line 2614 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 114:
#line 669 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"q"};
                    }
#line 2622 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 115:
#line 677 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$add"};
                    }
#line 2630 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 116:
#line 680 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$atan2"};
                    }
#line 2638 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 117:
#line 683 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$and"};
                    }
#line 2646 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 118:
#line 686 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$const"};
                    }
#line 2654 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 119:
#line 689 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$literal"};
                    }
#line 2662 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 120:
#line 692 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$or"};
                    }
#line 2670 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 121:
#line 695 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$not"};
                    }
#line 2678 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 122:
#line 698 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$cmp"};
                    }
#line 2686 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 123:
#line 701 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$eq"};
                    }
#line 2694 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 124:
#line 704 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$gt"};
                    }
#line 2702 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 125:
#line 707 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$gte"};
                    }
#line 2710 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 126:
#line 710 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$lt"};
                    }
#line 2718 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 127:
#line 713 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$lte"};
                    }
#line 2726 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 128:
#line 716 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$ne"};
                    }
#line 2734 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 129:
#line 719 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$convert"};
                    }
#line 2742 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 130:
#line 722 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$toBool"};
                    }
#line 2750 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 131:
#line 725 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$toDate"};
                    }
#line 2758 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 132:
#line 728 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$toDecimal"};
                    }
#line 2766 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 133:
#line 731 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$toDouble"};
                    }
#line 2774 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 134:
#line 734 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$toInt"};
                    }
#line 2782 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 135:
#line 737 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$toLong"};
                    }
#line 2790 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 136:
#line 740 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$toObjectId"};
                    }
#line 2798 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 137:
#line 743 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$toString"};
                    }
#line 2806 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 138:
#line 746 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$type"};
                    }
#line 2814 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 139:
#line 749 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$abs"};
                    }
#line 2822 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 140:
#line 752 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$ceil"};
                    }
#line 2830 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 141:
#line 755 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$divide"};
                    }
#line 2838 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 142:
#line 758 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$exp"};
                    }
#line 2846 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 143:
#line 761 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$floor"};
                    }
#line 2854 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 144:
#line 764 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$ln"};
                    }
#line 2862 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 145:
#line 767 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$log"};
                    }
#line 2870 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 146:
#line 770 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$log10"};
                    }
#line 2878 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 147:
#line 773 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$mod"};
                    }
#line 2886 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 148:
#line 776 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$multiply"};
                    }
#line 2894 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 149:
#line 779 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$pow"};
                    }
#line 2902 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 150:
#line 782 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$round"};
                    }
#line 2910 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 151:
#line 785 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$sqrt"};
                    }
#line 2918 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 152:
#line 788 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$subtract"};
                    }
#line 2926 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 153:
#line 791 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$trunc"};
                    }
#line 2934 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 154:
#line 794 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$concat"};
                    }
#line 2942 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 155:
#line 797 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$dateFromString"};
                    }
#line 2950 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 156:
#line 800 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$dateToString"};
                    }
#line 2958 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 157:
#line 803 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$indexOfBytes"};
                    }
#line 2966 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 158:
#line 806 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$indexOfCP"};
                    }
#line 2974 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 159:
#line 809 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$ltrim"};
                    }
#line 2982 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 160:
#line 812 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$meta"};
                    }
#line 2990 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 161:
#line 815 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$regexFind"};
                    }
#line 2998 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 162:
#line 818 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$regexFindAll"};
                    }
#line 3006 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 163:
#line 821 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$regexMatch"};
                    }
#line 3014 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 164:
#line 824 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$replaceOne"};
                    }
#line 3022 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 165:
#line 827 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$replaceAll"};
                    }
#line 3030 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 166:
#line 830 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$rtrim"};
                    }
#line 3038 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 167:
#line 833 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$split"};
                    }
#line 3046 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 168:
#line 836 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$strLenBytes"};
                    }
#line 3054 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 169:
#line 839 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$strLenCP"};
                    }
#line 3062 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 170:
#line 842 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$strcasecmp"};
                    }
#line 3070 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 171:
#line 845 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$substr"};
                    }
#line 3078 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 172:
#line 848 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$substrBytes"};
                    }
#line 3086 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 173:
#line 851 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$substrCP"};
                    }
#line 3094 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 174:
#line 854 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$toLower"};
                    }
#line 3102 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 175:
#line 857 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$trim"};
                    }
#line 3110 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 176:
#line 860 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$toUpper"};
                    }
#line 3118 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 177:
#line 867 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() =
                            CNode{UserString{YY_MOVE(yystack_[0].value.as<std::string>())}};
                    }
#line 3126 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 178:
#line 872 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{UserString{"randVal"}};
                    }
#line 3134 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 179:
#line 875 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{UserString{"textScore"}};
                    }
#line 3142 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 180:
#line 881 "src/mongo/db/cst/grammar.yy"
                    {
                        std::string str = YY_MOVE(yystack_[0].value.as<std::string>());
                        if (str.size() == 1) {
                            error(yystack_[0].location, "'$' by iteslf is not a valid FieldPath");
                        }
                        yylhs.value.as<CNode>() = CNode{UserFieldPath{str.substr(1), false}};
                    }
#line 3154 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 181:
#line 889 "src/mongo/db/cst/grammar.yy"
                    {
                        std::string str = YY_MOVE(yystack_[0].value.as<std::string>()).substr(2);
                        auto status = c_node_validation::validateVariableName(str);
                        if (!status.isOK()) {
                            error(yystack_[0].location, status.reason());
                        }
                        yylhs.value.as<CNode>() = CNode{UserFieldPath{str, true}};
                    }
#line 3167 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 182:
#line 898 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() =
                            CNode{UserBinary{YY_MOVE(yystack_[0].value.as<BSONBinData>())}};
                    }
#line 3175 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 183:
#line 904 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{UserUndefined{}};
                    }
#line 3183 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 184:
#line 910 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{UserObjectId{}};
                    }
#line 3191 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 185:
#line 916 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() =
                            CNode{UserDate{YY_MOVE(yystack_[0].value.as<Date_t>())}};
                    }
#line 3199 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 186:
#line 922 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{UserNull{}};
                    }
#line 3207 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 187:
#line 928 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() =
                            CNode{UserRegex{YY_MOVE(yystack_[0].value.as<BSONRegEx>())}};
                    }
#line 3215 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 188:
#line 934 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() =
                            CNode{UserDBPointer{YY_MOVE(yystack_[0].value.as<BSONDBRef>())}};
                    }
#line 3223 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 189:
#line 940 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() =
                            CNode{UserJavascript{YY_MOVE(yystack_[0].value.as<BSONCode>())}};
                    }
#line 3231 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 190:
#line 946 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() =
                            CNode{UserSymbol{YY_MOVE(yystack_[0].value.as<BSONSymbol>())}};
                    }
#line 3239 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 191:
#line 952 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{UserJavascriptWithScope{
                            YY_MOVE(yystack_[0].value.as<BSONCodeWScope>())}};
                    }
#line 3247 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 192:
#line 958 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() =
                            CNode{UserTimestamp{YY_MOVE(yystack_[0].value.as<Timestamp>())}};
                    }
#line 3255 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 193:
#line 964 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() =
                            CNode{UserMinKey{YY_MOVE(yystack_[0].value.as<UserMinKey>())}};
                    }
#line 3263 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 194:
#line 970 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() =
                            CNode{UserMaxKey{YY_MOVE(yystack_[0].value.as<UserMaxKey>())}};
                    }
#line 3271 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 195:
#line 976 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() =
                            CNode{UserInt{YY_MOVE(yystack_[0].value.as<int>())}};
                    }
#line 3279 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 196:
#line 979 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{UserInt{0}};
                    }
#line 3287 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 197:
#line 982 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{UserInt{1}};
                    }
#line 3295 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 198:
#line 985 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{UserInt{-1}};
                    }
#line 3303 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 199:
#line 991 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() =
                            CNode{UserLong{YY_MOVE(yystack_[0].value.as<long long>())}};
                    }
#line 3311 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 200:
#line 994 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{UserLong{0ll}};
                    }
#line 3319 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 201:
#line 997 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{UserLong{1ll}};
                    }
#line 3327 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 202:
#line 1000 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{UserLong{-1ll}};
                    }
#line 3335 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 203:
#line 1006 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() =
                            CNode{UserDouble{YY_MOVE(yystack_[0].value.as<double>())}};
                    }
#line 3343 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 204:
#line 1009 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{UserDouble{0.0}};
                    }
#line 3351 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 205:
#line 1012 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{UserDouble{1.0}};
                    }
#line 3359 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 206:
#line 1015 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{UserDouble{-1.0}};
                    }
#line 3367 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 207:
#line 1021 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() =
                            CNode{UserDecimal{YY_MOVE(yystack_[0].value.as<Decimal128>())}};
                    }
#line 3375 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 208:
#line 1024 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{UserDecimal{0.0}};
                    }
#line 3383 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 209:
#line 1027 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{UserDecimal{1.0}};
                    }
#line 3391 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 210:
#line 1030 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{UserDecimal{-1.0}};
                    }
#line 3399 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 211:
#line 1036 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{UserBoolean{true}};
                    }
#line 3407 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 212:
#line 1039 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{UserBoolean{false}};
                    }
#line 3415 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 213:
#line 1045 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 3421 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 214:
#line 1046 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 3427 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 215:
#line 1047 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 3433 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 216:
#line 1048 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 3439 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 217:
#line 1049 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 3445 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 218:
#line 1050 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 3451 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 219:
#line 1051 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 3457 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 220:
#line 1052 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 3463 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 221:
#line 1053 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 3469 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 222:
#line 1054 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 3475 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 223:
#line 1055 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 3481 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 224:
#line 1056 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 3487 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 225:
#line 1057 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 3493 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 226:
#line 1058 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 3499 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 227:
#line 1059 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 3505 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 228:
#line 1060 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 3511 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 229:
#line 1061 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 3517 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 230:
#line 1062 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 3523 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 231:
#line 1063 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 3529 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 232:
#line 1064 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 3535 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 233:
#line 1065 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 3541 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 234:
#line 1072 "src/mongo/db/cst/grammar.yy"
                    {
                    }
#line 3547 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 235:
#line 1073 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<std::vector<CNode>>() =
                            YY_MOVE(yystack_[0].value.as<std::vector<CNode>>());
                        yylhs.value.as<std::vector<CNode>>().emplace_back(
                            YY_MOVE(yystack_[1].value.as<CNode>()));
                    }
#line 3556 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 236:
#line 1080 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 3562 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 237:
#line 1080 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 3568 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 238:
#line 1084 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() =
                            CNode{CNode::ArrayChildren{YY_MOVE(yystack_[2].value.as<CNode>()),
                                                       YY_MOVE(yystack_[1].value.as<CNode>())}};
                    }
#line 3576 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 239:
#line 1089 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 3582 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 240:
#line 1089 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 3588 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 241:
#line 1089 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 3594 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 242:
#line 1089 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 3600 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 243:
#line 1089 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 3606 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 244:
#line 1089 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 3612 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 245:
#line 1090 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 3618 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 246:
#line 1090 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 3624 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 247:
#line 1096 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() =
                            CNode{YY_MOVE(yystack_[1].value.as<std::vector<CNode>>())};
                    }
#line 3632 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 248:
#line 1104 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[1].value.as<CNode>());
                    }
#line 3640 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 249:
#line 1110 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode::noopLeaf();
                    }
#line 3648 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 250:
#line 1113 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[1].value.as<CNode>());
                        yylhs.value.as<CNode>().objectChildren().emplace_back(
                            YY_MOVE(yystack_[0].value.as<std::pair<CNode::Fieldname, CNode>>()));
                    }
#line 3657 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 251:
#line 1120 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<std::pair<CNode::Fieldname, CNode>>() = {
                            YY_MOVE(yystack_[1].value.as<CNode::Fieldname>()),
                            YY_MOVE(yystack_[0].value.as<CNode>())};
                    }
#line 3665 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 252:
#line 1127 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() =
                            YY_MOVE(yystack_[0].value.as<CNode::Fieldname>());
                    }
#line 3671 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 253:
#line 1127 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() =
                            YY_MOVE(yystack_[0].value.as<CNode::Fieldname>());
                    }
#line 3677 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 254:
#line 1127 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() =
                            YY_MOVE(yystack_[0].value.as<CNode::Fieldname>());
                    }
#line 3683 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 255:
#line 1127 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() =
                            YY_MOVE(yystack_[0].value.as<CNode::Fieldname>());
                    }
#line 3689 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 256:
#line 1131 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"_id"};
                    }
#line 3697 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 257:
#line 1137 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 3703 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 258:
#line 1137 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 3709 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 259:
#line 1137 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 3715 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 260:
#line 1137 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 3721 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 261:
#line 1137 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 3727 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 262:
#line 1137 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 3733 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 263:
#line 1137 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 3739 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 264:
#line 1137 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 3745 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 265:
#line 1137 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 3751 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 266:
#line 1137 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 3757 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 267:
#line 1137 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 3763 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 268:
#line 1137 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 3769 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 269:
#line 1137 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 3775 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 270:
#line 1138 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 3781 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 271:
#line 1138 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 3787 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 272:
#line 1138 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 3793 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 273:
#line 1138 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 3799 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 274:
#line 1142 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::add, YY_MOVE(yystack_[1].value.as<CNode>())}}};
                    }
#line 3808 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 275:
#line 1149 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::atan2, YY_MOVE(yystack_[1].value.as<CNode>())}}};
                    }
#line 3817 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 276:
#line 1155 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::abs, YY_MOVE(yystack_[1].value.as<CNode>())}}};
                    }
#line 3825 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 277:
#line 1160 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::ceil, YY_MOVE(yystack_[1].value.as<CNode>())}}};
                    }
#line 3833 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 278:
#line 1165 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::divide,
                             CNode{CNode::ArrayChildren{YY_MOVE(yystack_[3].value.as<CNode>()),
                                                        YY_MOVE(yystack_[2].value.as<CNode>())}}}}};
                    }
#line 3842 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 279:
#line 1171 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::exponent, YY_MOVE(yystack_[1].value.as<CNode>())}}};
                    }
#line 3850 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 280:
#line 1176 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::floor, YY_MOVE(yystack_[1].value.as<CNode>())}}};
                    }
#line 3858 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 281:
#line 1181 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::ln, YY_MOVE(yystack_[1].value.as<CNode>())}}};
                    }
#line 3866 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 282:
#line 1186 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::log,
                             CNode{CNode::ArrayChildren{YY_MOVE(yystack_[3].value.as<CNode>()),
                                                        YY_MOVE(yystack_[2].value.as<CNode>())}}}}};
                    }
#line 3875 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 283:
#line 1192 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::logten, YY_MOVE(yystack_[1].value.as<CNode>())}}};
                    }
#line 3883 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 284:
#line 1197 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::mod,
                             CNode{CNode::ArrayChildren{YY_MOVE(yystack_[3].value.as<CNode>()),
                                                        YY_MOVE(yystack_[2].value.as<CNode>())}}}}};
                    }
#line 3892 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 285:
#line 1203 "src/mongo/db/cst/grammar.yy"
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
#line 3904 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 286:
#line 1212 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::pow,
                             CNode{CNode::ArrayChildren{YY_MOVE(yystack_[3].value.as<CNode>()),
                                                        YY_MOVE(yystack_[2].value.as<CNode>())}}}}};
                    }
#line 3913 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 287:
#line 1218 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::round,
                             CNode{CNode::ArrayChildren{YY_MOVE(yystack_[3].value.as<CNode>()),
                                                        YY_MOVE(yystack_[2].value.as<CNode>())}}}}};
                    }
#line 3922 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 288:
#line 1224 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::sqrt, YY_MOVE(yystack_[1].value.as<CNode>())}}};
                    }
#line 3930 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 289:
#line 1229 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::subtract,
                             CNode{CNode::ArrayChildren{YY_MOVE(yystack_[3].value.as<CNode>()),
                                                        YY_MOVE(yystack_[2].value.as<CNode>())}}}}};
                    }
#line 3939 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 290:
#line 1235 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::trunc,
                             CNode{CNode::ArrayChildren{YY_MOVE(yystack_[3].value.as<CNode>()),
                                                        YY_MOVE(yystack_[2].value.as<CNode>())}}}}};
                    }
#line 3948 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 291:
#line 1241 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 3954 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 292:
#line 1241 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 3960 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 293:
#line 1241 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 3966 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 294:
#line 1245 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::andExpr, YY_MOVE(yystack_[1].value.as<CNode>())}}};
                    }
#line 3975 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 295:
#line 1252 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::orExpr, YY_MOVE(yystack_[1].value.as<CNode>())}}};
                    }
#line 3984 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 296:
#line 1259 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::notExpr,
                             CNode{CNode::ArrayChildren{YY_MOVE(yystack_[2].value.as<CNode>())}}}}};
                    }
#line 3993 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 297:
#line 1266 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 3999 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 298:
#line 1266 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 4005 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 299:
#line 1266 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 4011 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 300:
#line 1266 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 4017 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 301:
#line 1266 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 4023 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 302:
#line 1266 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 4029 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 303:
#line 1266 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 4035 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 304:
#line 1267 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 4041 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 305:
#line 1267 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 4047 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 306:
#line 1267 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 4053 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 307:
#line 1267 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 4059 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 308:
#line 1267 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 4065 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 309:
#line 1267 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 4071 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 310:
#line 1267 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 4077 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 311:
#line 1267 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 4083 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 312:
#line 1268 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 4089 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 313:
#line 1268 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 4095 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 314:
#line 1268 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 4101 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 315:
#line 1268 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 4107 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 316:
#line 1268 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 4113 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 317:
#line 1268 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 4119 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 318:
#line 1268 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 4125 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 319:
#line 1272 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::concat, CNode{CNode::ArrayChildren{}}}}};
                        auto&& others = YY_MOVE(yystack_[2].value.as<std::vector<CNode>>());
                        auto&& array =
                            yylhs.value.as<CNode>().objectChildren()[0].second.arrayChildren();
                        array.insert(array.end(), others.begin(), others.end());
                    }
#line 4137 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 320:
#line 1282 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<std::pair<CNode::Fieldname, CNode>>() =
                            std::pair{KeyFieldname::formatArg, CNode{KeyValue::absentKey}};
                    }
#line 4145 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 321:
#line 1285 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<std::pair<CNode::Fieldname, CNode>>() = std::pair{
                            KeyFieldname::formatArg, YY_MOVE(yystack_[0].value.as<CNode>())};
                    }
#line 4153 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 322:
#line 1291 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<std::pair<CNode::Fieldname, CNode>>() =
                            std::pair{KeyFieldname::timezoneArg, CNode{KeyValue::absentKey}};
                    }
#line 4161 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 323:
#line 1294 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<std::pair<CNode::Fieldname, CNode>>() = std::pair{
                            KeyFieldname::timezoneArg, YY_MOVE(yystack_[0].value.as<CNode>())};
                    }
#line 4169 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 324:
#line 1301 "src/mongo/db/cst/grammar.yy"
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
#line 4179 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 325:
#line 1310 "src/mongo/db/cst/grammar.yy"
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
#line 4189 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 326:
#line 1318 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<std::vector<CNode>>() = CNode::ArrayChildren{};
                    }
#line 4197 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 327:
#line 1321 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<std::vector<CNode>>() =
                            CNode::ArrayChildren{YY_MOVE(yystack_[0].value.as<CNode>())};
                    }
#line 4205 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 328:
#line 1324 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<std::vector<CNode>>() =
                            CNode::ArrayChildren{YY_MOVE(yystack_[1].value.as<CNode>()),
                                                 YY_MOVE(yystack_[0].value.as<CNode>())};
                    }
#line 4213 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 329:
#line 1331 "src/mongo/db/cst/grammar.yy"
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
#line 4225 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 330:
#line 1342 "src/mongo/db/cst/grammar.yy"
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
#line 4237 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 331:
#line 1352 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<std::pair<CNode::Fieldname, CNode>>() =
                            std::pair{KeyFieldname::charsArg, CNode{KeyValue::absentKey}};
                    }
#line 4245 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 332:
#line 1355 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<std::pair<CNode::Fieldname, CNode>>() = std::pair{
                            KeyFieldname::charsArg, YY_MOVE(yystack_[0].value.as<CNode>())};
                    }
#line 4253 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 333:
#line 1361 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::ltrim,
                             CNode{CNode::ObjectChildren{
                                 {KeyFieldname::inputArg, YY_MOVE(yystack_[2].value.as<CNode>())},
                                 YY_MOVE(yystack_[4]
                                             .value.as<std::pair<CNode::Fieldname, CNode>>())}}}}};
                    }
#line 4263 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 334:
#line 1369 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::rtrim,
                             CNode{CNode::ObjectChildren{
                                 {KeyFieldname::inputArg, YY_MOVE(yystack_[2].value.as<CNode>())},
                                 YY_MOVE(yystack_[4]
                                             .value.as<std::pair<CNode::Fieldname, CNode>>())}}}}};
                    }
#line 4273 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 335:
#line 1377 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::trim,
                             CNode{CNode::ObjectChildren{
                                 {KeyFieldname::inputArg, YY_MOVE(yystack_[2].value.as<CNode>())},
                                 YY_MOVE(yystack_[4]
                                             .value.as<std::pair<CNode::Fieldname, CNode>>())}}}}};
                    }
#line 4283 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 336:
#line 1385 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<std::pair<CNode::Fieldname, CNode>>() =
                            std::pair{KeyFieldname::optionsArg, CNode{KeyValue::absentKey}};
                    }
#line 4291 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 337:
#line 1388 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<std::pair<CNode::Fieldname, CNode>>() = std::pair{
                            KeyFieldname::optionsArg, YY_MOVE(yystack_[0].value.as<CNode>())};
                    }
#line 4299 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 338:
#line 1393 "src/mongo/db/cst/grammar.yy"
                    {
                        // Note that the order of these arguments must match the constructor for the
                        // regex expression.
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::inputArg, YY_MOVE(yystack_[4].value.as<CNode>())},
                            {KeyFieldname::regexArg, YY_MOVE(yystack_[1].value.as<CNode>())},
                            YY_MOVE(yystack_[3].value.as<std::pair<CNode::Fieldname, CNode>>())}};
                    }
#line 4311 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 339:
#line 1402 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::regexFind, YY_MOVE(yystack_[1].value.as<CNode>())}}};
                    }
#line 4319 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 340:
#line 1408 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::regexFindAll, YY_MOVE(yystack_[1].value.as<CNode>())}}};
                    }
#line 4327 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 341:
#line 1414 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::regexMatch, YY_MOVE(yystack_[1].value.as<CNode>())}}};
                    }
#line 4335 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 342:
#line 1421 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::replaceOne,
                             CNode{CNode::ObjectChildren{
                                 {KeyFieldname::inputArg, YY_MOVE(yystack_[4].value.as<CNode>())},
                                 {KeyFieldname::findArg, YY_MOVE(yystack_[6].value.as<CNode>())},
                                 {KeyFieldname::replacementArg,
                                  YY_MOVE(yystack_[2].value.as<CNode>())}}}}}};
                    }
#line 4346 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 343:
#line 1431 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::replaceAll,
                             CNode{CNode::ObjectChildren{
                                 {KeyFieldname::inputArg, YY_MOVE(yystack_[4].value.as<CNode>())},
                                 {KeyFieldname::findArg, YY_MOVE(yystack_[6].value.as<CNode>())},
                                 {KeyFieldname::replacementArg,
                                  YY_MOVE(yystack_[2].value.as<CNode>())}}}}}};
                    }
#line 4357 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 344:
#line 1440 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::split,
                             CNode{CNode::ArrayChildren{YY_MOVE(yystack_[3].value.as<CNode>()),
                                                        YY_MOVE(yystack_[2].value.as<CNode>())}}}}};
                    }
#line 4366 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 345:
#line 1447 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::strLenBytes, YY_MOVE(yystack_[1].value.as<CNode>())}}};
                    }
#line 4375 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 346:
#line 1454 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::strLenCP, YY_MOVE(yystack_[1].value.as<CNode>())}}};
                    }
#line 4384 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 347:
#line 1462 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::strcasecmp,
                             CNode{CNode::ArrayChildren{YY_MOVE(yystack_[3].value.as<CNode>()),
                                                        YY_MOVE(yystack_[2].value.as<CNode>())}}}}};
                    }
#line 4393 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 348:
#line 1470 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::substr,
                             CNode{CNode::ArrayChildren{YY_MOVE(yystack_[4].value.as<CNode>()),
                                                        YY_MOVE(yystack_[3].value.as<CNode>()),
                                                        YY_MOVE(yystack_[2].value.as<CNode>())}}}}};
                    }
#line 4402 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 349:
#line 1478 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::substrBytes,
                             CNode{CNode::ArrayChildren{YY_MOVE(yystack_[4].value.as<CNode>()),
                                                        YY_MOVE(yystack_[3].value.as<CNode>()),
                                                        YY_MOVE(yystack_[2].value.as<CNode>())}}}}};
                    }
#line 4411 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 350:
#line 1486 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::substrCP,
                             CNode{CNode::ArrayChildren{YY_MOVE(yystack_[4].value.as<CNode>()),
                                                        YY_MOVE(yystack_[3].value.as<CNode>()),
                                                        YY_MOVE(yystack_[2].value.as<CNode>())}}}}};
                    }
#line 4420 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 351:
#line 1493 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::toLower, YY_MOVE(yystack_[1].value.as<CNode>())}}};
                    }
#line 4428 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 352:
#line 1499 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::toUpper, YY_MOVE(yystack_[1].value.as<CNode>())}}};
                    }
#line 4436 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 353:
#line 1505 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{KeyValue::randVal};
                    }
#line 4444 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 354:
#line 1508 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{KeyValue::textScore};
                    }
#line 4452 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 355:
#line 1514 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::meta, YY_MOVE(yystack_[1].value.as<CNode>())}}};
                    }
#line 4460 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 356:
#line 1520 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[1].value.as<CNode>());
                    }
#line 4468 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 357:
#line 1525 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode::noopLeaf();
                    }
#line 4476 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 358:
#line 1528 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[1].value.as<CNode>());
                        yylhs.value.as<CNode>().objectChildren().emplace_back(
                            YY_MOVE(yystack_[0].value.as<std::pair<CNode::Fieldname, CNode>>()));
                    }
#line 4485 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 359:
#line 1535 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{KeyValue::intOneKey};
                    }
#line 4493 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 360:
#line 1538 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{KeyValue::intNegOneKey};
                    }
#line 4501 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 361:
#line 1541 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{KeyValue::longOneKey};
                    }
#line 4509 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 362:
#line 1544 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{KeyValue::longNegOneKey};
                    }
#line 4517 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 363:
#line 1547 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{KeyValue::doubleOneKey};
                    }
#line 4525 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 364:
#line 1550 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{KeyValue::doubleNegOneKey};
                    }
#line 4533 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 365:
#line 1553 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{KeyValue::decimalOneKey};
                    }
#line 4541 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 366:
#line 1556 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{KeyValue::decimalNegOneKey};
                    }
#line 4549 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 367:
#line 1561 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<std::pair<CNode::Fieldname, CNode>>() = {
                            YY_MOVE(yystack_[1].value.as<CNode::Fieldname>()),
                            YY_MOVE(yystack_[0].value.as<CNode>())};
                    }
#line 4557 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 368:
#line 1563 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<std::pair<CNode::Fieldname, CNode>>() = {
                            YY_MOVE(yystack_[1].value.as<CNode::Fieldname>()),
                            YY_MOVE(yystack_[0].value.as<CNode>())};
                    }
#line 4565 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 369:
#line 1569 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 4571 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 370:
#line 1569 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 4577 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 371:
#line 1573 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::constExpr,
                             CNode{CNode::ArrayChildren{YY_MOVE(yystack_[2].value.as<CNode>())}}}}};
                    }
#line 4586 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 372:
#line 1580 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::literal,
                             CNode{CNode::ArrayChildren{YY_MOVE(yystack_[2].value.as<CNode>())}}}}};
                    }
#line 4595 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 373:
#line 1587 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 4601 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 374:
#line 1587 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 4607 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 375:
#line 1591 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 4613 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 376:
#line 1591 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 4619 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 377:
#line 1595 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() =
                            CNode{YY_MOVE(yystack_[1].value.as<std::vector<CNode>>())};
                    }
#line 4627 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 378:
#line 1601 "src/mongo/db/cst/grammar.yy"
                    {
                    }
#line 4633 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 379:
#line 1602 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<std::vector<CNode>>() =
                            YY_MOVE(yystack_[0].value.as<std::vector<CNode>>());
                        yylhs.value.as<std::vector<CNode>>().emplace_back(
                            YY_MOVE(yystack_[1].value.as<CNode>()));
                    }
#line 4642 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 380:
#line 1609 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[1].value.as<CNode>());
                    }
#line 4650 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 381:
#line 1615 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode::noopLeaf();
                    }
#line 4658 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 382:
#line 1618 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[1].value.as<CNode>());
                        yylhs.value.as<CNode>().objectChildren().emplace_back(
                            YY_MOVE(yystack_[0].value.as<std::pair<CNode::Fieldname, CNode>>()));
                    }
#line 4667 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 383:
#line 1625 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<std::pair<CNode::Fieldname, CNode>>() = {
                            YY_MOVE(yystack_[1].value.as<CNode::Fieldname>()),
                            YY_MOVE(yystack_[0].value.as<CNode>())};
                    }
#line 4675 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 384:
#line 1632 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() =
                            YY_MOVE(yystack_[0].value.as<CNode::Fieldname>());
                    }
#line 4681 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 385:
#line 1633 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() =
                            YY_MOVE(yystack_[0].value.as<CNode::Fieldname>());
                    }
#line 4687 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 386:
#line 1634 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() =
                            YY_MOVE(yystack_[0].value.as<CNode::Fieldname>());
                    }
#line 4693 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 387:
#line 1635 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() =
                            YY_MOVE(yystack_[0].value.as<CNode::Fieldname>());
                    }
#line 4699 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 388:
#line 1636 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() =
                            YY_MOVE(yystack_[0].value.as<CNode::Fieldname>());
                    }
#line 4705 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 389:
#line 1639 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 4711 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 390:
#line 1639 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 4717 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 391:
#line 1639 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 4723 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 392:
#line 1639 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 4729 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 393:
#line 1639 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 4735 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 394:
#line 1639 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 4741 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 395:
#line 1639 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 4747 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 396:
#line 1641 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::cmp, YY_MOVE(yystack_[1].value.as<CNode>())}}};
                    }
#line 4756 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 397:
#line 1646 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::eq, YY_MOVE(yystack_[1].value.as<CNode>())}}};
                    }
#line 4765 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 398:
#line 1651 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::gt, YY_MOVE(yystack_[1].value.as<CNode>())}}};
                    }
#line 4774 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 399:
#line 1656 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::gte, YY_MOVE(yystack_[1].value.as<CNode>())}}};
                    }
#line 4783 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 400:
#line 1661 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::lt, YY_MOVE(yystack_[1].value.as<CNode>())}}};
                    }
#line 4792 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 401:
#line 1666 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::lte, YY_MOVE(yystack_[1].value.as<CNode>())}}};
                    }
#line 4801 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 402:
#line 1671 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::ne, YY_MOVE(yystack_[1].value.as<CNode>())}}};
                    }
#line 4810 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 403:
#line 1677 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 4816 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 404:
#line 1678 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 4822 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 405:
#line 1679 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 4828 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 406:
#line 1680 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 4834 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 407:
#line 1681 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 4840 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 408:
#line 1682 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 4846 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 409:
#line 1683 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 4852 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 410:
#line 1684 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 4858 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 411:
#line 1685 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 4864 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 412:
#line 1686 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 4870 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 413:
#line 1691 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<std::pair<CNode::Fieldname, CNode>>() =
                            std::pair{KeyFieldname::onErrorArg, CNode{KeyValue::absentKey}};
                    }
#line 4878 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 414:
#line 1694 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<std::pair<CNode::Fieldname, CNode>>() = std::pair{
                            KeyFieldname::onErrorArg, YY_MOVE(yystack_[0].value.as<CNode>())};
                    }
#line 4886 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 415:
#line 1701 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<std::pair<CNode::Fieldname, CNode>>() =
                            std::pair{KeyFieldname::onNullArg, CNode{KeyValue::absentKey}};
                    }
#line 4894 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 416:
#line 1704 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<std::pair<CNode::Fieldname, CNode>>() = std::pair{
                            KeyFieldname::onNullArg, YY_MOVE(yystack_[0].value.as<CNode>())};
                    }
#line 4902 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 417:
#line 1711 "src/mongo/db/cst/grammar.yy"
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
#line 4913 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 418:
#line 1720 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::toBool, YY_MOVE(yystack_[1].value.as<CNode>())}}};
                    }
#line 4921 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 419:
#line 1725 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::toDate, YY_MOVE(yystack_[1].value.as<CNode>())}}};
                    }
#line 4929 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 420:
#line 1730 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::toDecimal, YY_MOVE(yystack_[1].value.as<CNode>())}}};
                    }
#line 4937 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 421:
#line 1735 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::toDouble, YY_MOVE(yystack_[1].value.as<CNode>())}}};
                    }
#line 4945 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 422:
#line 1740 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::toInt, YY_MOVE(yystack_[1].value.as<CNode>())}}};
                    }
#line 4953 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 423:
#line 1745 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::toLong, YY_MOVE(yystack_[1].value.as<CNode>())}}};
                    }
#line 4961 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 424:
#line 1750 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::toObjectId, YY_MOVE(yystack_[1].value.as<CNode>())}}};
                    }
#line 4969 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 425:
#line 1755 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::toString, YY_MOVE(yystack_[1].value.as<CNode>())}}};
                    }
#line 4977 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 426:
#line 1760 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::type, YY_MOVE(yystack_[1].value.as<CNode>())}}};
                    }
#line 4985 "src/mongo/db/cst/parser_gen.cpp"
                    break;


#line 4989 "src/mongo/db/cst/parser_gen.cpp"

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


const short ParserGen::yypact_ninf_ = -611;

const signed char ParserGen::yytable_ninf_ = -1;

const short ParserGen::yypact_[] = {
    67,   -49,  -42,  -49,  -49,  -47,  106,  -611, -611, 17,   -611, -611, -611, -611, -611, -611,
    865,  71,   70,   417,  -611, -611, -611, -611, -611, -611, -611, -611, -611, -611, -611, -611,
    -611, -611, -611, -611, -611, -611, -611, -611, -611, -611, -611, -611, 1105, -611, -611, -611,
    -611, -611, 25,   26,   92,   29,   34,   92,   -611, 83,   -611, -611, -611, -611, -611, -611,
    -611, -611, -611, -611, -611, -611, -611, -611, -611, -611, -611, -611, -611, -611, -611, -611,
    -611, -611, -611, -611, -611, -611, -611, -611, -611, -611, -611, -611, -611, -611, -611, -611,
    -611, -611, -611, -611, -611, -611, -611, -611, -611, -611, -611, -611, -611, -611, -611, -611,
    -611, -611, -611, -611, -611, -611, -611, -611, -611, -611, -611, -611, -611, -611, -611, -611,
    -611, -611, -611, -611, -611, -611, -611, -611, -611, -611, -611, 35,   -611, -611, -611, -611,
    -611, -611, -611, -611, -611, -611, -611, -611, -611, -611, -611, -611, -611, -611, -611, -611,
    -611, -611, -611, -611, -611, -611, -611, -611, -611, -611, -611, -611, -611, -611, -611, -611,
    -611, -611, -611, -611, -611, -611, -611, -611, -611, -611, -611, -611, -611, -611, -611, -611,
    -611, -611, -611, -611, -611, -611, -611, -611, -611, -49,  87,   -611, -611, -611, -611, -611,
    -611, 113,  -611, 137,  60,   17,   -611, -611, -611, -611, -611, -611, -611, -611, 95,   -611,
    -611, -17,  -611, -611, 535,  92,   -46,  -611, -611, -63,  -611, -83,  -611, -611, -18,  -611,
    243,  243,  -611, -611, -611, -611, -611, 126,  156,  -611, -611, 130,  105,  -611, -611, -611,
    -611, -611, -611, -611, -611, -611, -611, -611, -611, -611, -611, -611, -611, -611, 891,  757,
    -611, -611, -611, -611, -611, -611, -611, -611, -611, -611, -611, -611, -611, -611, -611, -611,
    -611, -611, -611, -611, -611, -611, -611, -611, -611, -611, -611, -611, -611, -611, -611, -611,
    -611, -611, -611, -611, -611, -611, -611, -611, -611, -611, -611, -611, -611, -611, -611, -611,
    -611, -611, -611, -611, -611, -611, -611, -611, -611, -611, -611, -611, -611, -611, -611, -611,
    -611, -611, -611, -611, -611, -611, -611, -611, -611, -611, -611, -611, -611, -611, -611, -611,
    -611, -611, -611, -611, -611, -611, -611, -611, -611, -611, -611, -8,   -611, -611, -611, 891,
    -611, 134,  891,  88,   88,   90,   891,  90,   93,   109,  -611, -611, -611, 114,  90,   891,
    891,  90,   90,   115,  116,  117,  891,  118,  891,  90,   90,   -611, 120,  121,  90,   122,
    88,   131,  -611, -611, -611, -611, -611, 132,  -611, 133,  891,  136,  891,  891,  140,  141,
    147,  148,  891,  891,  891,  891,  891,  891,  891,  891,  891,  891,  -611, 149,  891,  886,
    152,  -1,   -611, -611, 180,  183,  196,  891,  197,  206,  207,  891,  998,  172,  245,  244,
    891,  211,  212,  213,  214,  216,  891,  891,  998,  218,  891,  219,  220,  222,  261,  891,
    891,  224,  891,  225,  891,  228,  260,  230,  232,  271,  275,  891,  261,  891,  246,  891,
    248,  249,  891,  891,  891,  891,  251,  255,  256,  257,  258,  262,  264,  265,  266,  267,
    261,  891,  268,  -611, 891,  -611, -611, -611, -611, -611, -611, -611, -611, -611, -611, 891,
    -611, -611, -611, 270,  998,  -611, 272,  -611, -611, -611, -611, 891,  891,  891,  891,  -611,
    -611, -611, -611, -611, 891,  891,  273,  -611, 891,  -611, -611, -611, 891,  274,  891,  891,
    -611, 276,  -611, 891,  -611, 891,  -611, -611, 891,  891,  891,  301,  891,  -611, 891,  -611,
    -611, 891,  891,  891,  891,  -611, -611, -611, -611, -611, -611, -611, -611, -611, -611, 307,
    891,  -611, -611, 279,  280,  998,  282,  646,  285,  309,  277,  277,  289,  891,  891,  291,
    293,  -611, 891,  294,  891,  295,  297,  322,  328,  331,  302,  891,  303,  304,  891,  891,
    891,  305,  891,  306,  -611, -611, -611, -611, -611, 998,  -611, -611, 891,  335,  891,  327,
    327,  308,  891,  312,  332,  -611, 333,  334,  336,  338,  -611, 339,  891,  359,  891,  891,
    340,  341,  342,  343,  345,  346,  347,  348,  349,  350,  -611, -611, 891,  263,  -611, 891,
    309,  335,  -611, -611, 351,  352,  -611, 353,  -611, 354,  -611, -611, 891,  361,  370,  -611,
    355,  -611, -611, 357,  358,  360,  -611, 362,  -611, -611, 891,  -611, 335,  363,  -611, -611,
    -611, -611, 364,  891,  891,  -611, -611, -611, -611, -611, 365,  366,  367,  -611, 369,  371,
    375,  392,  -611, 396,  400,  -611, -611, -611, -611};

const short ParserGen::yydefact_[] = {
    0,   0,   0,   0,   0,   0,   0,   70,  3,   8,   2,   5,   4,   357, 6,   1,   0,   0,   0,
    0,   82,  108, 97,  107, 104, 113, 111, 105, 100, 102, 103, 110, 98,  114, 109, 112, 99,  106,
    101, 69,  256, 84,  83,  90,  0,   88,  89,  87,  71,  73,  0,   0,   0,   0,   0,   0,   10,
    0,   12,  13,  14,  15,  16,  17,  7,   139, 115, 117, 116, 140, 122, 154, 118, 129, 155, 156,
    141, 356, 123, 142, 143, 124, 125, 157, 158, 119, 144, 145, 146, 126, 127, 159, 160, 147, 148,
    128, 121, 120, 149, 161, 162, 163, 165, 164, 150, 166, 167, 151, 91,  94,  95,  96,  93,  92,
    170, 168, 169, 171, 172, 173, 152, 130, 131, 132, 133, 134, 135, 174, 136, 137, 176, 175, 153,
    138, 385, 386, 387, 384, 388, 0,   358, 212, 211, 210, 209, 208, 206, 205, 204, 198, 197, 196,
    202, 201, 200, 178, 76,  179, 177, 182, 183, 184, 185, 186, 187, 188, 189, 190, 191, 195, 199,
    203, 207, 192, 193, 194, 180, 181, 222, 223, 224, 225, 226, 231, 227, 228, 229, 232, 233, 213,
    214, 216, 217, 218, 230, 219, 220, 221, 74,  215, 72,  0,   0,   21,  22,  23,  24,  26,  28,
    0,   25,  0,   0,   8,   366, 365, 364, 363, 360, 359, 362, 361, 0,   367, 368, 0,   85,  19,
    0,   0,   0,   11,  9,   0,   75,  0,   77,  78,  0,   27,  0,   0,   66,  67,  68,  65,  29,
    0,   0,   353, 354, 0,   0,   79,  81,  86,  60,  59,  56,  55,  58,  52,  51,  54,  44,  43,
    46,  48,  47,  50,  234, 249, 45,  49,  53,  57,  39,  40,  41,  42,  61,  62,  63,  32,  33,
    34,  35,  36,  37,  38,  30,  64,  239, 240, 241, 257, 258, 242, 291, 292, 293, 243, 369, 370,
    246, 297, 298, 299, 300, 301, 302, 303, 304, 305, 306, 307, 308, 309, 310, 311, 312, 313, 314,
    315, 316, 318, 317, 244, 389, 390, 391, 392, 393, 394, 395, 245, 403, 404, 405, 406, 407, 408,
    409, 410, 411, 412, 259, 260, 261, 262, 263, 264, 265, 266, 267, 268, 269, 270, 271, 272, 273,
    31,  18,  0,   355, 76,  236, 234, 237, 0,   0,   0,   0,   0,   0,   0,   0,   0,   10,  10,
    10,  0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   10,  0,   0,   0,
    0,   0,   0,   10,  10,  10,  10,  10,  0,   10,  0,   0,   0,   0,   0,   0,   0,   0,   0,
    0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   10,  0,   0,   0,   0,   0,   235, 247, 0,
    0,   0,   0,   0,   0,   0,   234, 0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,
    0,   0,   0,   0,   0,   0,   331, 0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,
    0,   331, 0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,
    0,   0,   331, 0,   0,   248, 0,   253, 254, 252, 255, 250, 20,  80,  276, 274, 294, 0,   275,
    277, 396, 0,   378, 381, 0,   373, 374, 375, 376, 0,   0,   0,   0,   397, 279, 280, 398, 399,
    0,   0,   0,   281, 0,   283, 400, 401, 0,   0,   0,   0,   402, 0,   295, 0,   339, 0,   340,
    341, 0,   0,   0,   0,   0,   288, 0,   345, 346, 0,   0,   0,   0,   418, 419, 420, 421, 422,
    423, 351, 424, 425, 352, 0,   0,   426, 251, 0,   0,   378, 0,   0,   0,   413, 320, 320, 0,
    326, 326, 0,   0,   332, 0,   0,   234, 0,   0,   336, 0,   0,   0,   0,   0,   0,   0,   0,
    0,   0,   0,   0,   238, 319, 379, 377, 380, 0,   382, 371, 0,   415, 0,   322, 322, 0,   327,
    0,   0,   372, 0,   0,   0,   0,   296, 0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,
    0,   0,   0,   0,   383, 414, 0,   0,   321, 0,   413, 415, 278, 328, 0,   0,   282, 0,   284,
    0,   286, 337, 0,   0,   0,   287, 0,   344, 347, 0,   0,   0,   289, 0,   290, 416, 0,   323,
    415, 0,   329, 330, 333, 285, 0,   0,   0,   334, 348, 349, 350, 335, 0,   0,   0,   338, 0,
    0,   0,   0,   325, 0,   0,   417, 324, 343, 342};

const short ParserGen::yypgoto_[] = {
    -611, -611, -611, -222, -611, -15,  175,  -14,  -13,  -178, -611, -611, -611, -200, -185, -161,
    -158, -40,  -149, -35,  -45,  -31,  -142, -140, -430, -195, -611, -129, -103, -100, -611, -98,
    -92,  -131, -44,  -611, -611, -611, -611, -611, -611, 201,  -611, -611, -611, -611, -611, -611,
    -611, -611, 204,  -39,  -361, -74,  -150, -333, -611, -611, -611, -611, -611, -611, -611, -611,
    -611, -611, -611, -611, -611, -611, -611, -611, -611, -611, -611, -611, -611, -611, -235, -611,
    -611, -611, -611, -611, -611, -611, -611, -611, -611, -611, -611, -611, -611, -611, -611, -611,
    -611, -611, -611, -611, -611, -611, -611, -611, -611, -611, -611, -611, -611, -611, -611, -611,
    -611, -611, -611, -611, -611, -611, -611, -611, -611, -611, -611, -611, -611, -611, -203, -610,
    -133, -169, -446, -611, -362, -124, -132, 15,   -611, 94,   -611, -611, -611, -611, 208,  -611,
    -611, -611, -611, -611, -611, -611, -611, -611, -52,  -611};

const short ParserGen::yydefgoto_[] = {
    -1,  241, 500, 134, 44,  135, 136, 137, 138, 139, 246, 505, 618, 178, 179, 180, 181, 182,
    183, 184, 185, 186, 187, 188, 581, 189, 190, 191, 192, 193, 194, 195, 196, 197, 366, 520,
    521, 522, 583, 199, 10,  18,  57,  58,  59,  60,  61,  62,  63,  228, 290, 207, 367, 368,
    440, 292, 293, 431, 294, 295, 296, 297, 298, 299, 300, 301, 302, 303, 304, 305, 306, 307,
    308, 309, 310, 311, 312, 313, 469, 314, 315, 316, 317, 318, 319, 320, 321, 322, 323, 324,
    325, 326, 327, 328, 329, 330, 331, 332, 333, 334, 335, 336, 337, 338, 339, 340, 341, 342,
    343, 344, 345, 346, 347, 348, 349, 350, 351, 352, 353, 354, 355, 356, 357, 358, 359, 360,
    621, 653, 623, 656, 541, 637, 369, 582, 627, 8,   16,  225, 200, 238, 48,  49,  236, 237,
    50,  14,  19,  223, 224, 251, 140, 6,   470, 212};

const short ParserGen::yytable_[] = {
    198, 45,  46,  47,  211, 434, 242, 205, 252, 436, 205, 249, 203, 441, 518, 203, 210, 204, 11,
    12,  204, 206, 450, 451, 206, 254, 534, 234, 155, 457, 555, 459, 146, 147, 148, 248, 250, 164,
    437, 438, 276, 276, 7,   507, 13,  283, 283, 685, 9,   478, 575, 480, 481, 157, 235, 277, 277,
    486, 487, 488, 489, 490, 491, 492, 493, 494, 495, 467, 158, 498, 235, 214, 215, 7,   699, 216,
    217, 1,   511, 278, 278, 515, 279, 279, 2,   3,   4,   526, 218, 219, 5,   280, 280, 532, 533,
    220, 221, 536, 281, 281, 282, 282, 542, 543, 253, 545, 15,  547, 17,  289, 289, 284, 284, 64,
    554, 201, 556, 202, 558, 171, 208, 561, 562, 563, 564, 209, 222, 213, 143, 144, 145, 227, 146,
    147, 148, 229, 576, 285, 285, 578, 286, 286, 287, 287, 230, 149, 150, 151, 288, 288, 579, 231,
    152, 153, 154, 51,  52,  53,  54,  55,  56,  233, 585, 586, 587, 588, 291, 291, 471, 472, 362,
    589, 590, 363, 364, 592, 235, 435, 270, 593, 439, 595, 596, 443, 205, 523, 598, 650, 599, 203,
    247, 600, 601, 602, 204, 604, 506, 605, 206, 444, 606, 607, 608, 609, 448, 454, 455, 456, 458,
    501, 463, 464, 466, 243, 245, 611, 226, 169, 170, 171, 172, 468, 475, 477, 508, 442, 479, 509,
    626, 626, 482, 483, 449, 631, 633, 452, 453, 484, 485, 497, 510, 512, 641, 460, 461, 644, 645,
    646, 465, 648, 513, 514, 525, 255, 524, 527, 528, 529, 530, 651, 531, 654, 535, 537, 538, 659,
    539, 540, 544, 546, 256, 257, 548, 549, 550, 667, 551, 669, 670, 258, 259, 260, 552, 261, 262,
    263, 553, 594, 682, 622, 557, 681, 559, 560, 683, 565, 264, 265, 266, 566, 567, 568, 569, 267,
    268, 269, 570, 690, 571, 572, 573, 574, 577, 580, 603, 584, 591, 155, 432, 597, 610, 698, 612,
    620, 613, 615, 445, 446, 447, 619, 702, 703, 625, 270, 271, 629, 630, 632, 636, 634, 635, 638,
    157, 462, 639, 640, 642, 643, 647, 649, 652, 655, 658, 473, 474, 660, 476, 158, 159, 160, 161,
    162, 163, 164, 165, 166, 167, 168, 272, 273, 274, 275, 173, 174, 175, 661, 496, 662, 663, 668,
    664, 665, 691, 666, 671, 672, 673, 674, 675, 676, 677, 692, 678, 679, 680, 686, 687, 688, 689,
    693, 519, 694, 695, 244, 696, 617, 697, 700, 701, 704, 705, 706, 519, 707, 232, 708, 502, 503,
    504, 709, 65,  66,  67,  21,  22,  23,  24,  25,  26,  27,  28,  29,  30,  31,  32,  33,  710,
    34,  35,  36,  711, 37,  38,  68,  712, 361, 69,  70,  71,  72,  73,  74,  75,  684, 624, 657,
    76,  614, 628, 433, 365, 77,  78,  79,  80,  81,  82,  40,  83,  84,  0,   0,   519, 85,  86,
    87,  88,  0,   0,   0,   89,  90,  91,  92,  93,  94,  95,  0,   96,  97,  98,  0,   99,  100,
    101, 102, 103, 104, 105, 106, 107, 108, 109, 110, 111, 112, 113, 0,   0,   114, 115, 116, 117,
    118, 119, 120, 0,   121, 122, 123, 124, 125, 126, 127, 128, 129, 130, 131, 132, 133, 43,  0,
    0,   0,   0,   0,   0,   519, 65,  66,  67,  21,  22,  23,  24,  25,  26,  27,  28,  29,  30,
    31,  32,  33,  0,   34,  35,  36,  0,   37,  38,  68,  0,   0,   69,  70,  71,  72,  73,  74,
    75,  0,   0,   519, 76,  0,   0,   0,   0,   239, 78,  79,  80,  81,  82,  240, 83,  84,  0,
    0,   0,   85,  86,  87,  88,  0,   0,   0,   89,  90,  91,  92,  93,  94,  95,  0,   96,  97,
    98,  0,   99,  100, 101, 102, 103, 104, 105, 106, 107, 108, 109, 110, 111, 112, 113, 0,   0,
    114, 115, 116, 117, 118, 119, 120, 0,   121, 122, 123, 124, 125, 126, 127, 128, 129, 130, 131,
    132, 133, 43,  65,  66,  67,  21,  22,  23,  24,  25,  26,  27,  28,  29,  30,  31,  32,  33,
    0,   34,  35,  36,  0,   37,  38,  68,  0,   0,   69,  70,  71,  72,  73,  74,  75,  0,   0,
    0,   76,  0,   0,   0,   0,   616, 78,  79,  80,  81,  82,  40,  83,  84,  0,   0,   0,   85,
    86,  87,  88,  0,   0,   0,   89,  90,  91,  92,  93,  94,  95,  0,   96,  97,  98,  0,   99,
    100, 101, 102, 103, 104, 105, 106, 107, 108, 109, 110, 111, 112, 113, 0,   0,   114, 115, 116,
    117, 118, 119, 120, 0,   121, 122, 123, 124, 125, 126, 127, 128, 129, 130, 131, 132, 133, 43,
    370, 371, 372, 0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,
    0,   0,   0,   0,   373, 0,   0,   374, 375, 376, 377, 378, 379, 380, 0,   0,   0,   381, 0,
    0,   0,   0,   0,   382, 383, 384, 385, 386, 0,   387, 388, 0,   0,   0,   389, 390, 391, 392,
    0,   0,   0,   393, 394, 395, 0,   396, 397, 398, 0,   399, 400, 401, 0,   402, 403, 404, 405,
    406, 407, 408, 409, 410, 0,   0,   0,   0,   0,   0,   0,   0,   411, 412, 413, 414, 415, 416,
    417, 0,   418, 419, 420, 421, 422, 423, 424, 425, 426, 427, 428, 429, 430, 20,  21,  22,  23,
    24,  25,  26,  27,  28,  29,  30,  31,  32,  33,  0,   34,  35,  36,  0,   37,  38,  0,   21,
    22,  23,  24,  25,  26,  27,  28,  29,  30,  31,  32,  33,  0,   34,  35,  36,  39,  37,  38,
    0,   0,   0,   40,  0,   0,   141, 142, 0,   0,   0,   0,   0,   0,   0,   143, 144, 145, 499,
    146, 147, 148, 0,   41,  40,  42,  0,   0,   0,   0,   0,   0,   149, 150, 151, 0,   0,   0,
    0,   152, 153, 154, 0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   155, 0,   0,   0,
    0,   108, 109, 110, 111, 112, 113, 0,   0,   43,  0,   0,   270, 271, 0,   0,   0,   0,   0,
    0,   0,   157, 0,   0,   0,   0,   0,   0,   0,   0,   43,  0,   0,   0,   0,   0,   158, 159,
    160, 161, 162, 163, 164, 165, 166, 167, 168, 169, 170, 171, 172, 173, 174, 175, 176, 177, 141,
    142, 0,   0,   0,   0,   0,   0,   0,   143, 144, 145, 0,   146, 147, 148, 0,   0,   0,   0,
    0,   0,   0,   0,   0,   0,   149, 150, 151, 0,   0,   0,   0,   152, 153, 154, 0,   0,   0,
    0,   0,   0,   0,   0,   0,   0,   0,   155, 0,   0,   0,   0,   0,   0,   0,   0,   0,   0,
    0,   0,   0,   0,   0,   516, 517, 0,   0,   0,   0,   0,   0,   0,   157, 0,   0,   0,   0,
    0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   158, 159, 160, 161, 162, 163, 164, 165, 166,
    167, 168, 169, 170, 171, 172, 173, 174, 175, 176, 177, 141, 142, 0,   0,   0,   0,   0,   0,
    0,   143, 144, 145, 0,   146, 147, 148, 0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   149,
    150, 151, 0,   0,   0,   0,   152, 153, 154, 0,   0,   0,   0,   0,   0,   0,   0,   0,   0,
    0,   155, 0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   156,
    0,   0,   0,   0,   0,   0,   0,   157, 0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,
    0,   0,   0,   158, 159, 160, 161, 162, 163, 164, 165, 166, 167, 168, 169, 170, 171, 172, 173,
    174, 175, 176, 177};

const short ParserGen::yycheck_[] = {
    44,  16,  16,  16,  56,  367, 228, 52,  91,  370, 55,  74,  52,  374, 444, 55,  55,  52,  3,
    4,   55,  52,  383, 384, 55,  43,  456, 44,  74,  390, 476, 392, 40,  41,  42,  230, 99,  120,
    371, 372, 240, 241, 91,  44,  91,  240, 241, 657, 90,  410, 496, 412, 413, 99,  71,  240, 241,
    418, 419, 420, 421, 422, 423, 424, 425, 426, 427, 400, 114, 430, 71,  36,  37,  91,  684, 40,
    41,  10,  439, 240, 241, 443, 240, 241, 17,  18,  19,  448, 53,  54,  23,  240, 241, 454, 455,
    60,  61,  458, 240, 241, 240, 241, 463, 464, 235, 466, 0,   468, 91,  240, 241, 240, 241, 43,
    475, 90,  477, 91,  479, 127, 91,  482, 483, 484, 485, 91,  91,  44,  36,  37,  38,  44,  40,
    41,  42,  22,  497, 240, 241, 500, 240, 241, 240, 241, 7,   53,  54,  55,  240, 241, 511, 91,
    60,  61,  62,  84,  85,  86,  87,  88,  89,  66,  523, 524, 525, 526, 240, 241, 403, 404, 44,
    532, 533, 17,  44,  536, 71,  43,  90,  540, 90,  542, 543, 90,  229, 13,  547, 617, 549, 229,
    229, 552, 553, 554, 229, 556, 44,  558, 229, 90,  561, 562, 563, 564, 90,  90,  90,  90,  90,
    431, 90,  90,  90,  228, 228, 576, 201, 125, 126, 127, 128, 90,  90,  90,  44,  375, 90,  44,
    589, 590, 90,  90,  382, 594, 596, 385, 386, 90,  90,  90,  44,  44,  603, 393, 394, 606, 607,
    608, 398, 610, 44,  44,  8,   238, 9,   44,  44,  44,  44,  620, 44,  622, 44,  44,  44,  626,
    44,  6,   44,  44,  27,  28,  44,  13,  44,  636, 44,  638, 639, 36,  37,  38,  11,  40,  41,
    42,  11,  13,  25,  12,  44,  652, 44,  44,  655, 44,  53,  54,  55,  44,  44,  44,  44,  60,
    61,  62,  44,  668, 44,  44,  44,  44,  44,  43,  13,  43,  43,  74,  363, 43,  13,  682, 43,
    14,  44,  43,  378, 379, 380, 44,  691, 692, 43,  90,  91,  44,  43,  43,  16,  44,  43,  13,
    99,  395, 13,  43,  43,  43,  43,  43,  15,  24,  44,  405, 406, 43,  408, 114, 115, 116, 117,
    118, 119, 120, 121, 122, 123, 124, 125, 126, 127, 128, 129, 130, 131, 43,  428, 44,  44,  20,
    44,  43,  21,  44,  44,  44,  44,  44,  43,  43,  43,  21,  44,  44,  44,  44,  44,  44,  44,
    44,  444, 44,  44,  228, 44,  583, 44,  44,  44,  44,  44,  44,  456, 44,  213, 44,  431, 431,
    431, 44,  3,   4,   5,   6,   7,   8,   9,   10,  11,  12,  13,  14,  15,  16,  17,  18,  44,
    20,  21,  22,  44,  24,  25,  26,  44,  241, 29,  30,  31,  32,  33,  34,  35,  656, 587, 624,
    39,  581, 590, 365, 252, 44,  45,  46,  47,  48,  49,  50,  51,  52,  -1,  -1,  516, 56,  57,
    58,  59,  -1,  -1,  -1,  63,  64,  65,  66,  67,  68,  69,  -1,  71,  72,  73,  -1,  75,  76,
    77,  78,  79,  80,  81,  82,  83,  84,  85,  86,  87,  88,  89,  -1,  -1,  92,  93,  94,  95,
    96,  97,  98,  -1,  100, 101, 102, 103, 104, 105, 106, 107, 108, 109, 110, 111, 112, 113, -1,
    -1,  -1,  -1,  -1,  -1,  581, 3,   4,   5,   6,   7,   8,   9,   10,  11,  12,  13,  14,  15,
    16,  17,  18,  -1,  20,  21,  22,  -1,  24,  25,  26,  -1,  -1,  29,  30,  31,  32,  33,  34,
    35,  -1,  -1,  617, 39,  -1,  -1,  -1,  -1,  44,  45,  46,  47,  48,  49,  50,  51,  52,  -1,
    -1,  -1,  56,  57,  58,  59,  -1,  -1,  -1,  63,  64,  65,  66,  67,  68,  69,  -1,  71,  72,
    73,  -1,  75,  76,  77,  78,  79,  80,  81,  82,  83,  84,  85,  86,  87,  88,  89,  -1,  -1,
    92,  93,  94,  95,  96,  97,  98,  -1,  100, 101, 102, 103, 104, 105, 106, 107, 108, 109, 110,
    111, 112, 113, 3,   4,   5,   6,   7,   8,   9,   10,  11,  12,  13,  14,  15,  16,  17,  18,
    -1,  20,  21,  22,  -1,  24,  25,  26,  -1,  -1,  29,  30,  31,  32,  33,  34,  35,  -1,  -1,
    -1,  39,  -1,  -1,  -1,  -1,  44,  45,  46,  47,  48,  49,  50,  51,  52,  -1,  -1,  -1,  56,
    57,  58,  59,  -1,  -1,  -1,  63,  64,  65,  66,  67,  68,  69,  -1,  71,  72,  73,  -1,  75,
    76,  77,  78,  79,  80,  81,  82,  83,  84,  85,  86,  87,  88,  89,  -1,  -1,  92,  93,  94,
    95,  96,  97,  98,  -1,  100, 101, 102, 103, 104, 105, 106, 107, 108, 109, 110, 111, 112, 113,
    3,   4,   5,   -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,
    -1,  -1,  -1,  -1,  26,  -1,  -1,  29,  30,  31,  32,  33,  34,  35,  -1,  -1,  -1,  39,  -1,
    -1,  -1,  -1,  -1,  45,  46,  47,  48,  49,  -1,  51,  52,  -1,  -1,  -1,  56,  57,  58,  59,
    -1,  -1,  -1,  63,  64,  65,  -1,  67,  68,  69,  -1,  71,  72,  73,  -1,  75,  76,  77,  78,
    79,  80,  81,  82,  83,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  92,  93,  94,  95,  96,  97,
    98,  -1,  100, 101, 102, 103, 104, 105, 106, 107, 108, 109, 110, 111, 112, 5,   6,   7,   8,
    9,   10,  11,  12,  13,  14,  15,  16,  17,  18,  -1,  20,  21,  22,  -1,  24,  25,  -1,  6,
    7,   8,   9,   10,  11,  12,  13,  14,  15,  16,  17,  18,  -1,  20,  21,  22,  44,  24,  25,
    -1,  -1,  -1,  50,  -1,  -1,  27,  28,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  36,  37,  38,  44,
    40,  41,  42,  -1,  70,  50,  72,  -1,  -1,  -1,  -1,  -1,  -1,  53,  54,  55,  -1,  -1,  -1,
    -1,  60,  61,  62,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  74,  -1,  -1,  -1,
    -1,  84,  85,  86,  87,  88,  89,  -1,  -1,  113, -1,  -1,  90,  91,  -1,  -1,  -1,  -1,  -1,
    -1,  -1,  99,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  113, -1,  -1,  -1,  -1,  -1,  114, 115,
    116, 117, 118, 119, 120, 121, 122, 123, 124, 125, 126, 127, 128, 129, 130, 131, 132, 133, 27,
    28,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  36,  37,  38,  -1,  40,  41,  42,  -1,  -1,  -1,  -1,
    -1,  -1,  -1,  -1,  -1,  -1,  53,  54,  55,  -1,  -1,  -1,  -1,  60,  61,  62,  -1,  -1,  -1,
    -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  74,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,
    -1,  -1,  -1,  -1,  -1,  90,  91,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  99,  -1,  -1,  -1,  -1,
    -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  114, 115, 116, 117, 118, 119, 120, 121, 122,
    123, 124, 125, 126, 127, 128, 129, 130, 131, 132, 133, 27,  28,  -1,  -1,  -1,  -1,  -1,  -1,
    -1,  36,  37,  38,  -1,  40,  41,  42,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  53,
    54,  55,  -1,  -1,  -1,  -1,  60,  61,  62,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,
    -1,  74,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  91,
    -1,  -1,  -1,  -1,  -1,  -1,  -1,  99,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,
    -1,  -1,  -1,  114, 115, 116, 117, 118, 119, 120, 121, 122, 123, 124, 125, 126, 127, 128, 129,
    130, 131, 132, 133};

const short ParserGen::yystos_[] = {
    0,   10,  17,  18,  19,  23,  286, 91,  270, 90,  175, 270, 270, 91,  280, 0,   271, 91,  176,
    281, 5,   6,   7,   8,   9,   10,  11,  12,  13,  14,  15,  16,  17,  18,  20,  21,  22,  24,
    25,  44,  50,  70,  72,  113, 139, 140, 142, 143, 275, 276, 279, 84,  85,  86,  87,  88,  89,
    177, 178, 179, 180, 181, 182, 183, 43,  3,   4,   5,   26,  29,  30,  31,  32,  33,  34,  35,
    39,  44,  45,  46,  47,  48,  49,  51,  52,  56,  57,  58,  59,  63,  64,  65,  66,  67,  68,
    69,  71,  72,  73,  75,  76,  77,  78,  79,  80,  81,  82,  83,  84,  85,  86,  87,  88,  89,
    92,  93,  94,  95,  96,  97,  98,  100, 101, 102, 103, 104, 105, 106, 107, 108, 109, 110, 111,
    112, 138, 140, 141, 142, 143, 144, 285, 27,  28,  36,  37,  38,  40,  41,  42,  53,  54,  55,
    60,  61,  62,  74,  91,  99,  114, 115, 116, 117, 118, 119, 120, 121, 122, 123, 124, 125, 126,
    127, 128, 129, 130, 131, 132, 133, 148, 149, 150, 151, 152, 153, 154, 155, 156, 157, 158, 160,
    161, 162, 163, 164, 165, 166, 167, 168, 169, 174, 273, 90,  91,  152, 154, 155, 156, 186, 91,
    91,  186, 287, 288, 44,  36,  37,  40,  41,  53,  54,  60,  61,  91,  282, 283, 272, 270, 44,
    184, 22,  7,   91,  176, 66,  44,  71,  277, 278, 274, 44,  50,  136, 138, 140, 141, 142, 145,
    186, 160, 74,  99,  284, 91,  168, 43,  270, 27,  28,  36,  37,  38,  40,  41,  42,  53,  54,
    55,  60,  61,  62,  90,  91,  125, 126, 127, 128, 148, 149, 150, 151, 153, 157, 158, 160, 162,
    163, 164, 166, 167, 168, 185, 188, 190, 191, 193, 194, 195, 196, 197, 198, 199, 200, 201, 202,
    203, 204, 205, 206, 207, 208, 209, 210, 211, 212, 214, 215, 216, 217, 218, 219, 220, 221, 222,
    223, 224, 225, 226, 227, 228, 229, 230, 231, 232, 233, 234, 235, 236, 237, 238, 239, 240, 241,
    242, 243, 244, 245, 246, 247, 248, 249, 250, 251, 252, 253, 254, 255, 256, 257, 258, 259, 260,
    185, 44,  17,  44,  277, 169, 187, 188, 267, 3,   4,   5,   26,  29,  30,  31,  32,  33,  34,
    35,  39,  45,  46,  47,  48,  49,  51,  52,  56,  57,  58,  59,  63,  64,  65,  67,  68,  69,
    71,  72,  73,  75,  76,  77,  78,  79,  80,  81,  82,  83,  92,  93,  94,  95,  96,  97,  98,
    100, 101, 102, 103, 104, 105, 106, 107, 108, 109, 110, 111, 112, 192, 155, 272, 267, 43,  187,
    190, 190, 90,  189, 187, 189, 90,  90,  287, 287, 287, 90,  189, 187, 187, 189, 189, 90,  90,
    90,  187, 90,  187, 189, 189, 287, 90,  90,  189, 90,  190, 90,  213, 287, 213, 213, 287, 287,
    90,  287, 90,  187, 90,  187, 187, 90,  90,  90,  90,  187, 187, 187, 187, 187, 187, 187, 187,
    187, 187, 287, 90,  187, 44,  137, 138, 140, 142, 143, 146, 44,  44,  44,  44,  44,  187, 44,
    44,  44,  267, 90,  91,  159, 169, 170, 171, 172, 13,  9,   8,   187, 44,  44,  44,  44,  44,
    187, 187, 159, 44,  187, 44,  44,  44,  6,   265, 187, 187, 44,  187, 44,  187, 44,  13,  44,
    44,  11,  11,  187, 265, 187, 44,  187, 44,  44,  187, 187, 187, 187, 44,  44,  44,  44,  44,
    44,  44,  44,  44,  44,  265, 187, 44,  187, 187, 43,  159, 268, 173, 43,  187, 187, 187, 187,
    187, 187, 43,  187, 187, 13,  187, 187, 43,  187, 187, 187, 187, 187, 13,  187, 187, 187, 187,
    187, 187, 13,  187, 43,  44,  268, 43,  44,  144, 147, 44,  14,  261, 12,  263, 263, 43,  187,
    269, 269, 44,  43,  187, 43,  267, 44,  43,  16,  266, 13,  13,  43,  187, 43,  43,  187, 187,
    187, 43,  187, 43,  159, 187, 15,  262, 187, 24,  264, 264, 44,  187, 43,  43,  44,  44,  44,
    43,  44,  187, 20,  187, 187, 44,  44,  44,  44,  43,  43,  43,  44,  44,  44,  187, 25,  187,
    261, 262, 44,  44,  44,  44,  187, 21,  21,  44,  44,  44,  44,  44,  187, 262, 44,  44,  187,
    187, 44,  44,  44,  44,  44,  44,  44,  44,  44};

const short ParserGen::yyr1_[] = {
    0,   135, 286, 286, 286, 286, 286, 175, 176, 176, 288, 287, 177, 177, 177, 177, 177, 177, 183,
    178, 179, 186, 186, 186, 186, 180, 181, 182, 184, 184, 145, 145, 185, 185, 185, 185, 185, 185,
    185, 185, 185, 185, 185, 185, 185, 185, 185, 185, 185, 185, 185, 185, 185, 185, 185, 185, 185,
    185, 185, 185, 185, 185, 185, 185, 185, 136, 136, 136, 136, 270, 271, 271, 275, 275, 273, 273,
    272, 272, 277, 278, 278, 276, 279, 279, 279, 274, 274, 139, 139, 139, 142, 138, 138, 138, 138,
    138, 138, 140, 140, 140, 140, 140, 140, 140, 140, 140, 140, 140, 140, 140, 140, 140, 140, 140,
    140, 141, 141, 141, 141, 141, 141, 141, 141, 141, 141, 141, 141, 141, 141, 141, 141, 141, 141,
    141, 141, 141, 141, 141, 141, 141, 141, 141, 141, 141, 141, 141, 141, 141, 141, 141, 141, 141,
    141, 141, 141, 141, 141, 141, 141, 141, 141, 141, 141, 141, 141, 141, 141, 141, 141, 141, 141,
    141, 141, 141, 141, 141, 141, 160, 160, 160, 161, 174, 162, 163, 164, 166, 167, 168, 148, 149,
    150, 151, 153, 157, 158, 152, 152, 152, 152, 154, 154, 154, 154, 155, 155, 155, 155, 156, 156,
    156, 156, 165, 165, 169, 169, 169, 169, 169, 169, 169, 169, 169, 169, 169, 169, 169, 169, 169,
    169, 169, 169, 169, 169, 169, 267, 267, 187, 187, 189, 188, 188, 188, 188, 188, 188, 188, 188,
    190, 191, 192, 192, 146, 137, 137, 137, 137, 143, 193, 193, 193, 193, 193, 193, 193, 193, 193,
    193, 193, 193, 193, 193, 193, 193, 193, 194, 195, 246, 247, 248, 249, 250, 251, 252, 253, 254,
    255, 256, 257, 258, 259, 260, 196, 196, 196, 197, 198, 199, 203, 203, 203, 203, 203, 203, 203,
    203, 203, 203, 203, 203, 203, 203, 203, 203, 203, 203, 203, 203, 203, 203, 204, 263, 263, 264,
    264, 205, 206, 269, 269, 269, 207, 208, 265, 265, 209, 216, 226, 266, 266, 213, 210, 211, 212,
    214, 215, 217, 218, 219, 220, 221, 222, 223, 224, 225, 284, 284, 282, 280, 281, 281, 283, 283,
    283, 283, 283, 283, 283, 283, 285, 285, 200, 200, 201, 202, 159, 159, 170, 170, 171, 268, 268,
    172, 173, 173, 147, 144, 144, 144, 144, 144, 227, 227, 227, 227, 227, 227, 227, 228, 229, 230,
    231, 232, 233, 234, 235, 235, 235, 235, 235, 235, 235, 235, 235, 235, 261, 261, 262, 262, 236,
    237, 238, 239, 240, 241, 242, 243, 244, 245};

const signed char ParserGen::yyr2_[] = {
    0, 2,  2,  2, 2, 2, 2, 3, 0, 4, 0, 2, 1, 1, 1,  1,  1, 1, 5, 3, 7, 1, 1, 1, 1, 2, 2, 4, 0, 2, 2,
    2, 1,  1,  1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,  1,  1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,
    1, 1,  1,  1, 1, 1, 1, 3, 0, 2, 2, 1, 1, 3, 0,  2,  1, 2, 5, 5, 1, 1, 1, 0, 2, 1, 1, 1, 1, 1, 1,
    1, 1,  1,  1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,  1,  1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,
    1, 1,  1,  1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,  1,  1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,
    1, 1,  1,  1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,  1,  1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,
    1, 1,  1,  1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,  1,  1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,
    1, 1,  1,  1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,  1,  1, 0, 2, 1, 1, 4, 1, 1, 1, 1, 1, 1, 1, 1, 3,
    3, 0,  2,  2, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,  1,  1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 4, 4, 4, 4, 7,
    4, 4,  4,  7, 4, 7, 8, 7, 7, 4, 7, 7, 1, 1, 1,  4,  4, 6, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,
    1, 1,  1,  1, 1, 1, 1, 1, 1, 6, 0, 2, 0, 2, 11, 10, 0, 1, 2, 8, 8, 0, 2, 8, 8, 8, 0, 2, 7, 4, 4,
    4, 11, 11, 7, 4, 4, 7, 8, 8, 8, 4, 4, 1, 1, 4,  3,  0, 2, 1, 1, 1, 1, 1, 1, 1, 1, 2, 2, 1, 1, 6,
    6, 1,  1,  1, 1, 3, 0, 2, 3, 0, 2, 2, 1, 1, 1,  1,  1, 1, 1, 1, 1, 1, 1, 1, 4, 4, 4, 4, 4, 4, 4,
    1, 1,  1,  1, 1, 1, 1, 1, 1, 1, 0, 2, 0, 2, 11, 4,  4, 4, 4, 4, 4, 4, 4, 4};


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
                                           "NOR",
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
                                           "predFieldname",
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
                                           "match",
                                           "predicates",
                                           "compoundMatchExprs",
                                           "predValue",
                                           "additionalExprs",
                                           "predicate",
                                           "logicalExpr",
                                           "operatorExpression",
                                           "notExpr",
                                           "logicalExprField",
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
    0,    304,  304,  307,  310,  313,  316,  323,  329,  330,  338,  338,  341,  341,  341,  341,
    341,  341,  344,  354,  360,  370,  370,  370,  370,  374,  379,  384,  400,  403,  410,  413,
    419,  420,  421,  422,  423,  424,  425,  426,  427,  428,  429,  430,  433,  436,  439,  442,
    445,  448,  451,  454,  457,  460,  463,  466,  469,  472,  475,  478,  481,  484,  485,  486,
    487,  496,  496,  496,  496,  500,  506,  509,  515,  518,  527,  528,  534,  537,  544,  547,
    551,  560,  568,  569,  570,  573,  576,  583,  583,  583,  586,  594,  597,  600,  603,  606,
    609,  618,  621,  624,  627,  630,  633,  636,  639,  642,  645,  648,  651,  654,  657,  660,
    663,  666,  669,  677,  680,  683,  686,  689,  692,  695,  698,  701,  704,  707,  710,  713,
    716,  719,  722,  725,  728,  731,  734,  737,  740,  743,  746,  749,  752,  755,  758,  761,
    764,  767,  770,  773,  776,  779,  782,  785,  788,  791,  794,  797,  800,  803,  806,  809,
    812,  815,  818,  821,  824,  827,  830,  833,  836,  839,  842,  845,  848,  851,  854,  857,
    860,  867,  872,  875,  881,  889,  898,  904,  910,  916,  922,  928,  934,  940,  946,  952,
    958,  964,  970,  976,  979,  982,  985,  991,  994,  997,  1000, 1006, 1009, 1012, 1015, 1021,
    1024, 1027, 1030, 1036, 1039, 1045, 1046, 1047, 1048, 1049, 1050, 1051, 1052, 1053, 1054, 1055,
    1056, 1057, 1058, 1059, 1060, 1061, 1062, 1063, 1064, 1065, 1072, 1073, 1080, 1080, 1084, 1089,
    1089, 1089, 1089, 1089, 1089, 1090, 1090, 1096, 1104, 1110, 1113, 1120, 1127, 1127, 1127, 1127,
    1131, 1137, 1137, 1137, 1137, 1137, 1137, 1137, 1137, 1137, 1137, 1137, 1137, 1137, 1138, 1138,
    1138, 1138, 1142, 1149, 1155, 1160, 1165, 1171, 1176, 1181, 1186, 1192, 1197, 1203, 1212, 1218,
    1224, 1229, 1235, 1241, 1241, 1241, 1245, 1252, 1259, 1266, 1266, 1266, 1266, 1266, 1266, 1266,
    1267, 1267, 1267, 1267, 1267, 1267, 1267, 1267, 1268, 1268, 1268, 1268, 1268, 1268, 1268, 1272,
    1282, 1285, 1291, 1294, 1300, 1309, 1318, 1321, 1324, 1330, 1341, 1352, 1355, 1361, 1369, 1377,
    1385, 1388, 1393, 1402, 1408, 1414, 1420, 1430, 1440, 1447, 1454, 1461, 1469, 1477, 1485, 1493,
    1499, 1505, 1508, 1514, 1520, 1525, 1528, 1535, 1538, 1541, 1544, 1547, 1550, 1553, 1556, 1561,
    1563, 1569, 1569, 1573, 1580, 1587, 1587, 1591, 1591, 1595, 1601, 1602, 1609, 1615, 1618, 1625,
    1632, 1633, 1634, 1635, 1636, 1639, 1639, 1639, 1639, 1639, 1639, 1639, 1641, 1646, 1651, 1656,
    1661, 1666, 1671, 1677, 1678, 1679, 1680, 1681, 1682, 1683, 1684, 1685, 1686, 1691, 1694, 1701,
    1704, 1710, 1720, 1725, 1730, 1735, 1740, 1745, 1750, 1755, 1760};

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
#line 6040 "src/mongo/db/cst/parser_gen.cpp"

#line 1764 "src/mongo/db/cst/grammar.yy"
