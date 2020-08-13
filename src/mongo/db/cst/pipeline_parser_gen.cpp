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
#include "mongo/db/cst/c_node_disambiguation.h"
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

#line 67 "src/mongo/db/cst/pipeline_parser_gen.cpp"


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
#line 159 "src/mongo/db/cst/pipeline_parser_gen.cpp"


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

        case 135:  // dbPointer
        case 136:  // javascript
        case 137:  // symbol
        case 138:  // javascriptWScope
        case 139:  // int
        case 140:  // timestamp
        case 141:  // long
        case 142:  // double
        case 143:  // decimal
        case 144:  // minKey
        case 145:  // maxKey
        case 146:  // value
        case 147:  // string
        case 148:  // fieldPath
        case 149:  // binary
        case 150:  // undefined
        case 151:  // objectId
        case 152:  // bool
        case 153:  // date
        case 154:  // null
        case 155:  // regex
        case 156:  // simpleValue
        case 157:  // compoundValue
        case 158:  // valueArray
        case 159:  // valueObject
        case 160:  // valueFields
        case 161:  // variable
        case 162:  // pipeline
        case 163:  // stageList
        case 164:  // stage
        case 165:  // inhibitOptimization
        case 166:  // unionWith
        case 167:  // skip
        case 168:  // limit
        case 169:  // project
        case 170:  // sample
        case 171:  // projectFields
        case 172:  // projection
        case 173:  // num
        case 174:  // expression
        case 175:  // compoundExpression
        case 176:  // exprFixedTwoArg
        case 177:  // expressionArray
        case 178:  // expressionObject
        case 179:  // expressionFields
        case 180:  // maths
        case 181:  // add
        case 182:  // atan2
        case 183:  // boolExps
        case 184:  // and
        case 185:  // or
        case 186:  // not
        case 187:  // literalEscapes
        case 188:  // const
        case 189:  // literal
        case 190:  // stringExps
        case 191:  // concat
        case 192:  // dateFromString
        case 193:  // dateToString
        case 194:  // indexOfBytes
        case 195:  // indexOfCP
        case 196:  // ltrim
        case 197:  // regexFind
        case 198:  // regexFindAll
        case 199:  // regexMatch
        case 200:  // regexArgs
        case 201:  // replaceOne
        case 202:  // replaceAll
        case 203:  // rtrim
        case 204:  // split
        case 205:  // strLenBytes
        case 206:  // strLenCP
        case 207:  // strcasecmp
        case 208:  // substr
        case 209:  // substrBytes
        case 210:  // substrCP
        case 211:  // toLower
        case 212:  // toUpper
        case 213:  // trim
        case 214:  // compExprs
        case 215:  // cmp
        case 216:  // eq
        case 217:  // gt
        case 218:  // gte
        case 219:  // lt
        case 220:  // lte
        case 221:  // ne
        case 222:  // typeExpression
        case 223:  // convert
        case 224:  // toBool
        case 225:  // toDate
        case 226:  // toDecimal
        case 227:  // toDouble
        case 228:  // toInt
        case 229:  // toLong
        case 230:  // toObjectId
        case 231:  // toString
        case 232:  // type
        case 233:  // abs
        case 234:  // ceil
        case 235:  // divide
        case 236:  // exponent
        case 237:  // floor
        case 238:  // ln
        case 239:  // log
        case 240:  // logten
        case 241:  // mod
        case 242:  // multiply
        case 243:  // pow
        case 244:  // round
        case 245:  // sqrt
        case 246:  // subtract
        case 247:  // trunc
        case 257:  // matchExpression
        case 258:  // filterFields
        case 259:  // filterVal
            value.YY_MOVE_OR_COPY<CNode>(YY_MOVE(that.value));
            break;

        case 122:  // projectionFieldname
        case 123:  // expressionFieldname
        case 124:  // stageAsUserFieldname
        case 125:  // filterFieldname
        case 126:  // argAsUserFieldname
        case 127:  // aggExprAsUserFieldname
        case 128:  // invariableUserFieldname
        case 129:  // idAsUserFieldname
        case 130:  // valueFieldname
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

        case 131:  // projectField
        case 132:  // expressionField
        case 133:  // valueField
        case 134:  // filterField
        case 248:  // onErrorArg
        case 249:  // onNullArg
        case 250:  // formatArg
        case 251:  // timezoneArg
        case 252:  // charsArg
        case 253:  // optionsArg
            value.YY_MOVE_OR_COPY<std::pair<CNode::Fieldname, CNode>>(YY_MOVE(that.value));
            break;

        case 97:   // "fieldname"
        case 98:   // "string"
        case 116:  // "$-prefixed string"
        case 117:  // "$$-prefixed string"
        case 118:  // "$-prefixed fieldname"
            value.YY_MOVE_OR_COPY<std::string>(YY_MOVE(that.value));
            break;

        case 254:  // expressions
        case 255:  // values
        case 256:  // exprZeroToTwo
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

        case 135:  // dbPointer
        case 136:  // javascript
        case 137:  // symbol
        case 138:  // javascriptWScope
        case 139:  // int
        case 140:  // timestamp
        case 141:  // long
        case 142:  // double
        case 143:  // decimal
        case 144:  // minKey
        case 145:  // maxKey
        case 146:  // value
        case 147:  // string
        case 148:  // fieldPath
        case 149:  // binary
        case 150:  // undefined
        case 151:  // objectId
        case 152:  // bool
        case 153:  // date
        case 154:  // null
        case 155:  // regex
        case 156:  // simpleValue
        case 157:  // compoundValue
        case 158:  // valueArray
        case 159:  // valueObject
        case 160:  // valueFields
        case 161:  // variable
        case 162:  // pipeline
        case 163:  // stageList
        case 164:  // stage
        case 165:  // inhibitOptimization
        case 166:  // unionWith
        case 167:  // skip
        case 168:  // limit
        case 169:  // project
        case 170:  // sample
        case 171:  // projectFields
        case 172:  // projection
        case 173:  // num
        case 174:  // expression
        case 175:  // compoundExpression
        case 176:  // exprFixedTwoArg
        case 177:  // expressionArray
        case 178:  // expressionObject
        case 179:  // expressionFields
        case 180:  // maths
        case 181:  // add
        case 182:  // atan2
        case 183:  // boolExps
        case 184:  // and
        case 185:  // or
        case 186:  // not
        case 187:  // literalEscapes
        case 188:  // const
        case 189:  // literal
        case 190:  // stringExps
        case 191:  // concat
        case 192:  // dateFromString
        case 193:  // dateToString
        case 194:  // indexOfBytes
        case 195:  // indexOfCP
        case 196:  // ltrim
        case 197:  // regexFind
        case 198:  // regexFindAll
        case 199:  // regexMatch
        case 200:  // regexArgs
        case 201:  // replaceOne
        case 202:  // replaceAll
        case 203:  // rtrim
        case 204:  // split
        case 205:  // strLenBytes
        case 206:  // strLenCP
        case 207:  // strcasecmp
        case 208:  // substr
        case 209:  // substrBytes
        case 210:  // substrCP
        case 211:  // toLower
        case 212:  // toUpper
        case 213:  // trim
        case 214:  // compExprs
        case 215:  // cmp
        case 216:  // eq
        case 217:  // gt
        case 218:  // gte
        case 219:  // lt
        case 220:  // lte
        case 221:  // ne
        case 222:  // typeExpression
        case 223:  // convert
        case 224:  // toBool
        case 225:  // toDate
        case 226:  // toDecimal
        case 227:  // toDouble
        case 228:  // toInt
        case 229:  // toLong
        case 230:  // toObjectId
        case 231:  // toString
        case 232:  // type
        case 233:  // abs
        case 234:  // ceil
        case 235:  // divide
        case 236:  // exponent
        case 237:  // floor
        case 238:  // ln
        case 239:  // log
        case 240:  // logten
        case 241:  // mod
        case 242:  // multiply
        case 243:  // pow
        case 244:  // round
        case 245:  // sqrt
        case 246:  // subtract
        case 247:  // trunc
        case 257:  // matchExpression
        case 258:  // filterFields
        case 259:  // filterVal
            value.move<CNode>(YY_MOVE(that.value));
            break;

        case 122:  // projectionFieldname
        case 123:  // expressionFieldname
        case 124:  // stageAsUserFieldname
        case 125:  // filterFieldname
        case 126:  // argAsUserFieldname
        case 127:  // aggExprAsUserFieldname
        case 128:  // invariableUserFieldname
        case 129:  // idAsUserFieldname
        case 130:  // valueFieldname
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

        case 131:  // projectField
        case 132:  // expressionField
        case 133:  // valueField
        case 134:  // filterField
        case 248:  // onErrorArg
        case 249:  // onNullArg
        case 250:  // formatArg
        case 251:  // timezoneArg
        case 252:  // charsArg
        case 253:  // optionsArg
            value.move<std::pair<CNode::Fieldname, CNode>>(YY_MOVE(that.value));
            break;

        case 97:   // "fieldname"
        case 98:   // "string"
        case 116:  // "$-prefixed string"
        case 117:  // "$$-prefixed string"
        case 118:  // "$-prefixed fieldname"
            value.move<std::string>(YY_MOVE(that.value));
            break;

        case 254:  // expressions
        case 255:  // values
        case 256:  // exprZeroToTwo
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

        case 135:  // dbPointer
        case 136:  // javascript
        case 137:  // symbol
        case 138:  // javascriptWScope
        case 139:  // int
        case 140:  // timestamp
        case 141:  // long
        case 142:  // double
        case 143:  // decimal
        case 144:  // minKey
        case 145:  // maxKey
        case 146:  // value
        case 147:  // string
        case 148:  // fieldPath
        case 149:  // binary
        case 150:  // undefined
        case 151:  // objectId
        case 152:  // bool
        case 153:  // date
        case 154:  // null
        case 155:  // regex
        case 156:  // simpleValue
        case 157:  // compoundValue
        case 158:  // valueArray
        case 159:  // valueObject
        case 160:  // valueFields
        case 161:  // variable
        case 162:  // pipeline
        case 163:  // stageList
        case 164:  // stage
        case 165:  // inhibitOptimization
        case 166:  // unionWith
        case 167:  // skip
        case 168:  // limit
        case 169:  // project
        case 170:  // sample
        case 171:  // projectFields
        case 172:  // projection
        case 173:  // num
        case 174:  // expression
        case 175:  // compoundExpression
        case 176:  // exprFixedTwoArg
        case 177:  // expressionArray
        case 178:  // expressionObject
        case 179:  // expressionFields
        case 180:  // maths
        case 181:  // add
        case 182:  // atan2
        case 183:  // boolExps
        case 184:  // and
        case 185:  // or
        case 186:  // not
        case 187:  // literalEscapes
        case 188:  // const
        case 189:  // literal
        case 190:  // stringExps
        case 191:  // concat
        case 192:  // dateFromString
        case 193:  // dateToString
        case 194:  // indexOfBytes
        case 195:  // indexOfCP
        case 196:  // ltrim
        case 197:  // regexFind
        case 198:  // regexFindAll
        case 199:  // regexMatch
        case 200:  // regexArgs
        case 201:  // replaceOne
        case 202:  // replaceAll
        case 203:  // rtrim
        case 204:  // split
        case 205:  // strLenBytes
        case 206:  // strLenCP
        case 207:  // strcasecmp
        case 208:  // substr
        case 209:  // substrBytes
        case 210:  // substrCP
        case 211:  // toLower
        case 212:  // toUpper
        case 213:  // trim
        case 214:  // compExprs
        case 215:  // cmp
        case 216:  // eq
        case 217:  // gt
        case 218:  // gte
        case 219:  // lt
        case 220:  // lte
        case 221:  // ne
        case 222:  // typeExpression
        case 223:  // convert
        case 224:  // toBool
        case 225:  // toDate
        case 226:  // toDecimal
        case 227:  // toDouble
        case 228:  // toInt
        case 229:  // toLong
        case 230:  // toObjectId
        case 231:  // toString
        case 232:  // type
        case 233:  // abs
        case 234:  // ceil
        case 235:  // divide
        case 236:  // exponent
        case 237:  // floor
        case 238:  // ln
        case 239:  // log
        case 240:  // logten
        case 241:  // mod
        case 242:  // multiply
        case 243:  // pow
        case 244:  // round
        case 245:  // sqrt
        case 246:  // subtract
        case 247:  // trunc
        case 257:  // matchExpression
        case 258:  // filterFields
        case 259:  // filterVal
            value.copy<CNode>(that.value);
            break;

        case 122:  // projectionFieldname
        case 123:  // expressionFieldname
        case 124:  // stageAsUserFieldname
        case 125:  // filterFieldname
        case 126:  // argAsUserFieldname
        case 127:  // aggExprAsUserFieldname
        case 128:  // invariableUserFieldname
        case 129:  // idAsUserFieldname
        case 130:  // valueFieldname
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

        case 131:  // projectField
        case 132:  // expressionField
        case 133:  // valueField
        case 134:  // filterField
        case 248:  // onErrorArg
        case 249:  // onNullArg
        case 250:  // formatArg
        case 251:  // timezoneArg
        case 252:  // charsArg
        case 253:  // optionsArg
            value.copy<std::pair<CNode::Fieldname, CNode>>(that.value);
            break;

        case 97:   // "fieldname"
        case 98:   // "string"
        case 116:  // "$-prefixed string"
        case 117:  // "$$-prefixed string"
        case 118:  // "$-prefixed fieldname"
            value.copy<std::string>(that.value);
            break;

        case 254:  // expressions
        case 255:  // values
        case 256:  // exprZeroToTwo
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

        case 135:  // dbPointer
        case 136:  // javascript
        case 137:  // symbol
        case 138:  // javascriptWScope
        case 139:  // int
        case 140:  // timestamp
        case 141:  // long
        case 142:  // double
        case 143:  // decimal
        case 144:  // minKey
        case 145:  // maxKey
        case 146:  // value
        case 147:  // string
        case 148:  // fieldPath
        case 149:  // binary
        case 150:  // undefined
        case 151:  // objectId
        case 152:  // bool
        case 153:  // date
        case 154:  // null
        case 155:  // regex
        case 156:  // simpleValue
        case 157:  // compoundValue
        case 158:  // valueArray
        case 159:  // valueObject
        case 160:  // valueFields
        case 161:  // variable
        case 162:  // pipeline
        case 163:  // stageList
        case 164:  // stage
        case 165:  // inhibitOptimization
        case 166:  // unionWith
        case 167:  // skip
        case 168:  // limit
        case 169:  // project
        case 170:  // sample
        case 171:  // projectFields
        case 172:  // projection
        case 173:  // num
        case 174:  // expression
        case 175:  // compoundExpression
        case 176:  // exprFixedTwoArg
        case 177:  // expressionArray
        case 178:  // expressionObject
        case 179:  // expressionFields
        case 180:  // maths
        case 181:  // add
        case 182:  // atan2
        case 183:  // boolExps
        case 184:  // and
        case 185:  // or
        case 186:  // not
        case 187:  // literalEscapes
        case 188:  // const
        case 189:  // literal
        case 190:  // stringExps
        case 191:  // concat
        case 192:  // dateFromString
        case 193:  // dateToString
        case 194:  // indexOfBytes
        case 195:  // indexOfCP
        case 196:  // ltrim
        case 197:  // regexFind
        case 198:  // regexFindAll
        case 199:  // regexMatch
        case 200:  // regexArgs
        case 201:  // replaceOne
        case 202:  // replaceAll
        case 203:  // rtrim
        case 204:  // split
        case 205:  // strLenBytes
        case 206:  // strLenCP
        case 207:  // strcasecmp
        case 208:  // substr
        case 209:  // substrBytes
        case 210:  // substrCP
        case 211:  // toLower
        case 212:  // toUpper
        case 213:  // trim
        case 214:  // compExprs
        case 215:  // cmp
        case 216:  // eq
        case 217:  // gt
        case 218:  // gte
        case 219:  // lt
        case 220:  // lte
        case 221:  // ne
        case 222:  // typeExpression
        case 223:  // convert
        case 224:  // toBool
        case 225:  // toDate
        case 226:  // toDecimal
        case 227:  // toDouble
        case 228:  // toInt
        case 229:  // toLong
        case 230:  // toObjectId
        case 231:  // toString
        case 232:  // type
        case 233:  // abs
        case 234:  // ceil
        case 235:  // divide
        case 236:  // exponent
        case 237:  // floor
        case 238:  // ln
        case 239:  // log
        case 240:  // logten
        case 241:  // mod
        case 242:  // multiply
        case 243:  // pow
        case 244:  // round
        case 245:  // sqrt
        case 246:  // subtract
        case 247:  // trunc
        case 257:  // matchExpression
        case 258:  // filterFields
        case 259:  // filterVal
            value.move<CNode>(that.value);
            break;

        case 122:  // projectionFieldname
        case 123:  // expressionFieldname
        case 124:  // stageAsUserFieldname
        case 125:  // filterFieldname
        case 126:  // argAsUserFieldname
        case 127:  // aggExprAsUserFieldname
        case 128:  // invariableUserFieldname
        case 129:  // idAsUserFieldname
        case 130:  // valueFieldname
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

        case 131:  // projectField
        case 132:  // expressionField
        case 133:  // valueField
        case 134:  // filterField
        case 248:  // onErrorArg
        case 249:  // onNullArg
        case 250:  // formatArg
        case 251:  // timezoneArg
        case 252:  // charsArg
        case 253:  // optionsArg
            value.move<std::pair<CNode::Fieldname, CNode>>(that.value);
            break;

        case 97:   // "fieldname"
        case 98:   // "string"
        case 116:  // "$-prefixed string"
        case 117:  // "$$-prefixed string"
        case 118:  // "$-prefixed fieldname"
            value.move<std::string>(that.value);
            break;

        case 254:  // expressions
        case 255:  // values
        case 256:  // exprZeroToTwo
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

                case 135:  // dbPointer
                case 136:  // javascript
                case 137:  // symbol
                case 138:  // javascriptWScope
                case 139:  // int
                case 140:  // timestamp
                case 141:  // long
                case 142:  // double
                case 143:  // decimal
                case 144:  // minKey
                case 145:  // maxKey
                case 146:  // value
                case 147:  // string
                case 148:  // fieldPath
                case 149:  // binary
                case 150:  // undefined
                case 151:  // objectId
                case 152:  // bool
                case 153:  // date
                case 154:  // null
                case 155:  // regex
                case 156:  // simpleValue
                case 157:  // compoundValue
                case 158:  // valueArray
                case 159:  // valueObject
                case 160:  // valueFields
                case 161:  // variable
                case 162:  // pipeline
                case 163:  // stageList
                case 164:  // stage
                case 165:  // inhibitOptimization
                case 166:  // unionWith
                case 167:  // skip
                case 168:  // limit
                case 169:  // project
                case 170:  // sample
                case 171:  // projectFields
                case 172:  // projection
                case 173:  // num
                case 174:  // expression
                case 175:  // compoundExpression
                case 176:  // exprFixedTwoArg
                case 177:  // expressionArray
                case 178:  // expressionObject
                case 179:  // expressionFields
                case 180:  // maths
                case 181:  // add
                case 182:  // atan2
                case 183:  // boolExps
                case 184:  // and
                case 185:  // or
                case 186:  // not
                case 187:  // literalEscapes
                case 188:  // const
                case 189:  // literal
                case 190:  // stringExps
                case 191:  // concat
                case 192:  // dateFromString
                case 193:  // dateToString
                case 194:  // indexOfBytes
                case 195:  // indexOfCP
                case 196:  // ltrim
                case 197:  // regexFind
                case 198:  // regexFindAll
                case 199:  // regexMatch
                case 200:  // regexArgs
                case 201:  // replaceOne
                case 202:  // replaceAll
                case 203:  // rtrim
                case 204:  // split
                case 205:  // strLenBytes
                case 206:  // strLenCP
                case 207:  // strcasecmp
                case 208:  // substr
                case 209:  // substrBytes
                case 210:  // substrCP
                case 211:  // toLower
                case 212:  // toUpper
                case 213:  // trim
                case 214:  // compExprs
                case 215:  // cmp
                case 216:  // eq
                case 217:  // gt
                case 218:  // gte
                case 219:  // lt
                case 220:  // lte
                case 221:  // ne
                case 222:  // typeExpression
                case 223:  // convert
                case 224:  // toBool
                case 225:  // toDate
                case 226:  // toDecimal
                case 227:  // toDouble
                case 228:  // toInt
                case 229:  // toLong
                case 230:  // toObjectId
                case 231:  // toString
                case 232:  // type
                case 233:  // abs
                case 234:  // ceil
                case 235:  // divide
                case 236:  // exponent
                case 237:  // floor
                case 238:  // ln
                case 239:  // log
                case 240:  // logten
                case 241:  // mod
                case 242:  // multiply
                case 243:  // pow
                case 244:  // round
                case 245:  // sqrt
                case 246:  // subtract
                case 247:  // trunc
                case 257:  // matchExpression
                case 258:  // filterFields
                case 259:  // filterVal
                    yylhs.value.emplace<CNode>();
                    break;

                case 122:  // projectionFieldname
                case 123:  // expressionFieldname
                case 124:  // stageAsUserFieldname
                case 125:  // filterFieldname
                case 126:  // argAsUserFieldname
                case 127:  // aggExprAsUserFieldname
                case 128:  // invariableUserFieldname
                case 129:  // idAsUserFieldname
                case 130:  // valueFieldname
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

                case 131:  // projectField
                case 132:  // expressionField
                case 133:  // valueField
                case 134:  // filterField
                case 248:  // onErrorArg
                case 249:  // onNullArg
                case 250:  // formatArg
                case 251:  // timezoneArg
                case 252:  // charsArg
                case 253:  // optionsArg
                    yylhs.value.emplace<std::pair<CNode::Fieldname, CNode>>();
                    break;

                case 97:   // "fieldname"
                case 98:   // "string"
                case 116:  // "$-prefixed string"
                case 117:  // "$$-prefixed string"
                case 118:  // "$-prefixed fieldname"
                    yylhs.value.emplace<std::string>();
                    break;

                case 254:  // expressions
                case 255:  // values
                case 256:  // exprZeroToTwo
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
#line 282 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        invariant(cst);
                        *cst = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 1720 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 3:
#line 286 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        invariant(cst);
                        *cst = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 1729 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 4:
#line 294 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[1].value.as<CNode>());
                    }
#line 1737 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 5:
#line 300 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                    }
#line 1743 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 6:
#line 301 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() =
                            CNode{CNode::ArrayChildren{YY_MOVE(yystack_[2].value.as<CNode>())}};
                    }
#line 1751 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 7:
#line 309 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        lexer.sortObjTokens();
                    }
#line 1757 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 9:
#line 312 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 1763 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 10:
#line 312 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 1769 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 11:
#line 312 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 1775 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 12:
#line 312 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 1781 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 13:
#line 312 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 1787 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 14:
#line 312 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 1793 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 15:
#line 315 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{std::pair{
                            KeyFieldname::sample,
                            CNode{CNode::ObjectChildren{
                                {KeyFieldname::sizeArg, YY_MOVE(yystack_[1].value.as<CNode>())},
                            }}}}};
                    }
#line 1805 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 16:
#line 325 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            std::pair{KeyFieldname::inhibitOptimization, CNode::noopLeaf()}}};
                    }
#line 1813 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 17:
#line 331 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        auto pipeline = YY_MOVE(yystack_[1].value.as<CNode>());
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{std::pair{
                            KeyFieldname::unionWith,
                            CNode{CNode::ObjectChildren{
                                {KeyFieldname::collArg, YY_MOVE(yystack_[3].value.as<CNode>())},
                                {KeyFieldname::pipelineArg, std::move(pipeline)}}}}}};
                    }
#line 1826 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 18:
#line 341 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 1832 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 19:
#line 341 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 1838 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 20:
#line 341 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 1844 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 21:
#line 341 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 1850 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 22:
#line 345 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            std::pair{KeyFieldname::skip, YY_MOVE(yystack_[0].value.as<CNode>())}}};
                    }
#line 1858 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 23:
#line 350 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{std::pair{
                            KeyFieldname::limit, YY_MOVE(yystack_[0].value.as<CNode>())}}};
                    }
#line 1866 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 24:
#line 355 "src/mongo/db/cst/pipeline_grammar.yy"
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
#line 1884 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 25:
#line 371 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode::noopLeaf();
                    }
#line 1892 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 26:
#line 374 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[1].value.as<CNode>());
                        yylhs.value.as<CNode>().objectChildren().emplace_back(
                            YY_MOVE(yystack_[0].value.as<std::pair<CNode::Fieldname, CNode>>()));
                    }
#line 1901 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 27:
#line 381 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<std::pair<CNode::Fieldname, CNode>>() = {
                            KeyFieldname::id, YY_MOVE(yystack_[0].value.as<CNode>())};
                    }
#line 1909 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 28:
#line 384 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<std::pair<CNode::Fieldname, CNode>>() = {
                            YY_MOVE(yystack_[1].value.as<CNode::Fieldname>()),
                            YY_MOVE(yystack_[0].value.as<CNode>())};
                    }
#line 1917 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 29:
#line 390 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 1923 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 30:
#line 391 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 1929 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 31:
#line 392 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 1935 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 32:
#line 393 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 1941 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 33:
#line 394 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 1947 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 34:
#line 395 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 1953 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 35:
#line 396 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 1959 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 36:
#line 397 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 1965 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 37:
#line 398 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 1971 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 38:
#line 399 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 1977 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 39:
#line 400 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 1983 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 40:
#line 401 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() =
                            CNode{NonZeroKey{YY_MOVE(yystack_[0].value.as<int>())}};
                    }
#line 1991 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 41:
#line 404 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{KeyValue::intZeroKey};
                    }
#line 1999 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 42:
#line 407 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() =
                            CNode{NonZeroKey{YY_MOVE(yystack_[0].value.as<long long>())}};
                    }
#line 2007 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 43:
#line 410 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{KeyValue::longZeroKey};
                    }
#line 2015 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 44:
#line 413 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() =
                            CNode{NonZeroKey{YY_MOVE(yystack_[0].value.as<double>())}};
                    }
#line 2023 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 45:
#line 416 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{KeyValue::doubleZeroKey};
                    }
#line 2031 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 46:
#line 419 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() =
                            CNode{NonZeroKey{YY_MOVE(yystack_[0].value.as<Decimal128>())}};
                    }
#line 2039 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 47:
#line 422 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{KeyValue::decimalZeroKey};
                    }
#line 2047 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 48:
#line 425 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{KeyValue::trueKey};
                    }
#line 2055 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 49:
#line 428 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{KeyValue::falseKey};
                    }
#line 2063 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 50:
#line 431 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 2069 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 51:
#line 432 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 2075 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 52:
#line 433 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 2081 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 53:
#line 434 "src/mongo/db/cst/pipeline_grammar.yy"
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
#line 2092 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 54:
#line 443 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() =
                            YY_MOVE(yystack_[0].value.as<CNode::Fieldname>());
                    }
#line 2098 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 55:
#line 443 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() =
                            YY_MOVE(yystack_[0].value.as<CNode::Fieldname>());
                    }
#line 2104 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 56:
#line 443 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() =
                            YY_MOVE(yystack_[0].value.as<CNode::Fieldname>());
                    }
#line 2110 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 57:
#line 443 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() =
                            YY_MOVE(yystack_[0].value.as<CNode::Fieldname>());
                    }
#line 2116 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 58:
#line 447 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[1].value.as<CNode>());
                    }
#line 2124 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 59:
#line 453 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode::noopLeaf();
                    }
#line 2132 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 60:
#line 456 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[1].value.as<CNode>());
                        yylhs.value.as<CNode>().objectChildren().emplace_back(
                            YY_MOVE(yystack_[0].value.as<std::pair<CNode::Fieldname, CNode>>()));
                    }
#line 2141 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 61:
#line 462 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<std::pair<CNode::Fieldname, CNode>>() = {
                            YY_MOVE(yystack_[1].value.as<CNode::Fieldname>()),
                            YY_MOVE(yystack_[0].value.as<CNode>())};
                    }
#line 2149 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 62:
#line 468 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 2155 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 63:
#line 473 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() =
                            YY_MOVE(yystack_[0].value.as<CNode::Fieldname>());
                    }
#line 2161 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 64:
#line 473 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() =
                            YY_MOVE(yystack_[0].value.as<CNode::Fieldname>());
                    }
#line 2167 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 65:
#line 473 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() =
                            YY_MOVE(yystack_[0].value.as<CNode::Fieldname>());
                    }
#line 2173 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 66:
#line 477 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() =
                            UserFieldname{YY_MOVE(yystack_[0].value.as<std::string>())};
                    }
#line 2181 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 67:
#line 485 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() =
                            UserFieldname{"$_internalInhibitOptimization"};
                    }
#line 2189 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 68:
#line 488 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$unionWith"};
                    }
#line 2197 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 69:
#line 491 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$skip"};
                    }
#line 2205 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 70:
#line 494 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$limit"};
                    }
#line 2213 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 71:
#line 497 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$project"};
                    }
#line 2221 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 72:
#line 500 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$sample"};
                    }
#line 2229 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 73:
#line 509 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"coll"};
                    }
#line 2237 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 74:
#line 512 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"pipeline"};
                    }
#line 2245 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 75:
#line 515 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"size"};
                    }
#line 2253 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 76:
#line 518 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"input"};
                    }
#line 2261 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 77:
#line 521 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"to"};
                    }
#line 2269 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 78:
#line 524 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"onError"};
                    }
#line 2277 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 79:
#line 527 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"onNull"};
                    }
#line 2285 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 80:
#line 530 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"dateString"};
                    }
#line 2293 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 81:
#line 533 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"format"};
                    }
#line 2301 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 82:
#line 536 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"timezone"};
                    }
#line 2309 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 83:
#line 539 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"date"};
                    }
#line 2317 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 84:
#line 542 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"chars"};
                    }
#line 2325 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 85:
#line 545 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"regex"};
                    }
#line 2333 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 86:
#line 548 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"options"};
                    }
#line 2341 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 87:
#line 551 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"find"};
                    }
#line 2349 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 88:
#line 554 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"replacement"};
                    }
#line 2357 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 89:
#line 562 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$add"};
                    }
#line 2365 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 90:
#line 565 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$atan2"};
                    }
#line 2373 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 91:
#line 568 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$and"};
                    }
#line 2381 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 92:
#line 571 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$const"};
                    }
#line 2389 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 93:
#line 574 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$literal"};
                    }
#line 2397 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 94:
#line 577 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$or"};
                    }
#line 2405 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 95:
#line 580 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$not"};
                    }
#line 2413 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 96:
#line 583 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$cmp"};
                    }
#line 2421 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 97:
#line 586 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$eq"};
                    }
#line 2429 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 98:
#line 589 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$gt"};
                    }
#line 2437 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 99:
#line 592 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$gte"};
                    }
#line 2445 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 100:
#line 595 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$lt"};
                    }
#line 2453 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 101:
#line 598 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$lte"};
                    }
#line 2461 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 102:
#line 601 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$ne"};
                    }
#line 2469 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 103:
#line 604 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$convert"};
                    }
#line 2477 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 104:
#line 607 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$toBool"};
                    }
#line 2485 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 105:
#line 610 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$toDate"};
                    }
#line 2493 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 106:
#line 613 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$toDecimal"};
                    }
#line 2501 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 107:
#line 616 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$toDouble"};
                    }
#line 2509 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 108:
#line 619 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$toInt"};
                    }
#line 2517 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 109:
#line 622 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$toLong"};
                    }
#line 2525 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 110:
#line 625 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$toObjectId"};
                    }
#line 2533 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 111:
#line 628 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$toString"};
                    }
#line 2541 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 112:
#line 631 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$type"};
                    }
#line 2549 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 113:
#line 634 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$abs"};
                    }
#line 2557 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 114:
#line 637 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$ceil"};
                    }
#line 2565 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 115:
#line 640 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$divide"};
                    }
#line 2573 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 116:
#line 643 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$exp"};
                    }
#line 2581 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 117:
#line 646 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$floor"};
                    }
#line 2589 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 118:
#line 649 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$ln"};
                    }
#line 2597 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 119:
#line 652 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$log"};
                    }
#line 2605 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 120:
#line 655 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$log10"};
                    }
#line 2613 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 121:
#line 658 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$mod"};
                    }
#line 2621 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 122:
#line 661 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$multiply"};
                    }
#line 2629 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 123:
#line 664 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$pow"};
                    }
#line 2637 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 124:
#line 667 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$round"};
                    }
#line 2645 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 125:
#line 670 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$sqrt"};
                    }
#line 2653 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 126:
#line 673 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$subtract"};
                    }
#line 2661 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 127:
#line 676 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$trunc"};
                    }
#line 2669 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 128:
#line 679 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$concat"};
                    }
#line 2677 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 129:
#line 682 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$dateFromString"};
                    }
#line 2685 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 130:
#line 685 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$dateToString"};
                    }
#line 2693 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 131:
#line 688 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$indexOfBytes"};
                    }
#line 2701 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 132:
#line 691 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$indexOfCP"};
                    }
#line 2709 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 133:
#line 694 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$ltrim"};
                    }
#line 2717 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 134:
#line 697 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$regexFind"};
                    }
#line 2725 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 135:
#line 700 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$regexFindAll"};
                    }
#line 2733 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 136:
#line 703 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$regexMatch"};
                    }
#line 2741 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 137:
#line 706 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$replaceOne"};
                    }
#line 2749 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 138:
#line 709 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$replaceAll"};
                    }
#line 2757 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 139:
#line 712 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$rtrim"};
                    }
#line 2765 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 140:
#line 715 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$split"};
                    }
#line 2773 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 141:
#line 718 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$strLenBytes"};
                    }
#line 2781 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 142:
#line 721 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$strLenCP"};
                    }
#line 2789 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 143:
#line 724 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$strcasecmp"};
                    }
#line 2797 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 144:
#line 727 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$substr"};
                    }
#line 2805 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 145:
#line 730 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$substrBytes"};
                    }
#line 2813 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 146:
#line 733 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$substrCP"};
                    }
#line 2821 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 147:
#line 736 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$toLower"};
                    }
#line 2829 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 148:
#line 739 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$trim"};
                    }
#line 2837 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 149:
#line 742 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$toUpper"};
                    }
#line 2845 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 150:
#line 749 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() =
                            CNode{UserString{YY_MOVE(yystack_[0].value.as<std::string>())}};
                    }
#line 2853 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 151:
#line 754 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        std::string str = YY_MOVE(yystack_[0].value.as<std::string>());
                        if (str.size() == 1) {
                            error(yystack_[0].location, "'$' by iteslf is not a valid FieldPath");
                        }
                        yylhs.value.as<CNode>() = CNode{UserFieldPath{str.substr(1), false}};
                    }
#line 2865 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 152:
#line 762 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        std::string str = YY_MOVE(yystack_[0].value.as<std::string>()).substr(2);
                        auto status = c_node_validation::validateVariableName(str);
                        if (!status.isOK()) {
                            error(yystack_[0].location, status.reason());
                        }
                        yylhs.value.as<CNode>() = CNode{UserFieldPath{str, true}};
                    }
#line 2878 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 153:
#line 771 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() =
                            CNode{UserBinary{YY_MOVE(yystack_[0].value.as<BSONBinData>())}};
                    }
#line 2886 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 154:
#line 777 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{UserUndefined{}};
                    }
#line 2894 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 155:
#line 783 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{UserObjectId{}};
                    }
#line 2902 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 156:
#line 789 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() =
                            CNode{UserDate{YY_MOVE(yystack_[0].value.as<Date_t>())}};
                    }
#line 2910 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 157:
#line 795 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{UserNull{}};
                    }
#line 2918 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 158:
#line 801 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() =
                            CNode{UserRegex{YY_MOVE(yystack_[0].value.as<BSONRegEx>())}};
                    }
#line 2926 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 159:
#line 807 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() =
                            CNode{UserDBPointer{YY_MOVE(yystack_[0].value.as<BSONDBRef>())}};
                    }
#line 2934 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 160:
#line 813 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() =
                            CNode{UserJavascript{YY_MOVE(yystack_[0].value.as<BSONCode>())}};
                    }
#line 2942 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 161:
#line 819 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() =
                            CNode{UserSymbol{YY_MOVE(yystack_[0].value.as<BSONSymbol>())}};
                    }
#line 2950 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 162:
#line 825 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{UserJavascriptWithScope{
                            YY_MOVE(yystack_[0].value.as<BSONCodeWScope>())}};
                    }
#line 2958 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 163:
#line 831 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() =
                            CNode{UserTimestamp{YY_MOVE(yystack_[0].value.as<Timestamp>())}};
                    }
#line 2966 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 164:
#line 837 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() =
                            CNode{UserMinKey{YY_MOVE(yystack_[0].value.as<UserMinKey>())}};
                    }
#line 2974 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 165:
#line 843 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() =
                            CNode{UserMaxKey{YY_MOVE(yystack_[0].value.as<UserMaxKey>())}};
                    }
#line 2982 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 166:
#line 849 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() =
                            CNode{UserInt{YY_MOVE(yystack_[0].value.as<int>())}};
                    }
#line 2990 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 167:
#line 852 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{UserInt{0}};
                    }
#line 2998 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 168:
#line 858 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() =
                            CNode{UserLong{YY_MOVE(yystack_[0].value.as<long long>())}};
                    }
#line 3006 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 169:
#line 861 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{UserLong{0ll}};
                    }
#line 3014 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 170:
#line 867 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() =
                            CNode{UserDouble{YY_MOVE(yystack_[0].value.as<double>())}};
                    }
#line 3022 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 171:
#line 870 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{UserDouble{0.0}};
                    }
#line 3030 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 172:
#line 876 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() =
                            CNode{UserDecimal{YY_MOVE(yystack_[0].value.as<Decimal128>())}};
                    }
#line 3038 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 173:
#line 879 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{UserDecimal{0.0}};
                    }
#line 3046 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 174:
#line 885 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{UserBoolean{true}};
                    }
#line 3054 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 175:
#line 888 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{UserBoolean{false}};
                    }
#line 3062 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 176:
#line 894 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 3068 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 177:
#line 895 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 3074 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 178:
#line 896 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 3080 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 179:
#line 897 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 3086 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 180:
#line 898 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 3092 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 181:
#line 899 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 3098 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 182:
#line 900 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 3104 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 183:
#line 901 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 3110 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 184:
#line 902 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 3116 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 185:
#line 903 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 3122 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 186:
#line 904 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 3128 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 187:
#line 905 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 3134 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 188:
#line 906 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 3140 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 189:
#line 907 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 3146 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 190:
#line 908 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 3152 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 191:
#line 909 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 3158 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 192:
#line 910 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 3164 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 193:
#line 911 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 3170 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 194:
#line 912 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 3176 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 195:
#line 913 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 3182 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 196:
#line 914 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 3188 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 197:
#line 921 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                    }
#line 3194 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 198:
#line 922 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<std::vector<CNode>>() =
                            YY_MOVE(yystack_[0].value.as<std::vector<CNode>>());
                        yylhs.value.as<std::vector<CNode>>().emplace_back(
                            YY_MOVE(yystack_[1].value.as<CNode>()));
                    }
#line 3203 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 199:
#line 929 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 3209 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 200:
#line 929 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 3215 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 201:
#line 933 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() =
                            CNode{CNode::ArrayChildren{YY_MOVE(yystack_[2].value.as<CNode>()),
                                                       YY_MOVE(yystack_[1].value.as<CNode>())}};
                    }
#line 3223 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 202:
#line 938 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 3229 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 203:
#line 938 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 3235 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 204:
#line 938 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 3241 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 205:
#line 938 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 3247 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 206:
#line 938 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 3253 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 207:
#line 938 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 3259 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 208:
#line 939 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 3265 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 209:
#line 939 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 3271 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 210:
#line 945 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() =
                            CNode{YY_MOVE(yystack_[1].value.as<std::vector<CNode>>())};
                    }
#line 3279 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 211:
#line 953 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[1].value.as<CNode>());
                    }
#line 3287 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 212:
#line 959 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode::noopLeaf();
                    }
#line 3295 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 213:
#line 962 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[1].value.as<CNode>());
                        yylhs.value.as<CNode>().objectChildren().emplace_back(
                            YY_MOVE(yystack_[0].value.as<std::pair<CNode::Fieldname, CNode>>()));
                    }
#line 3304 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 214:
#line 969 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<std::pair<CNode::Fieldname, CNode>>() = {
                            YY_MOVE(yystack_[1].value.as<CNode::Fieldname>()),
                            YY_MOVE(yystack_[0].value.as<CNode>())};
                    }
#line 3312 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 215:
#line 976 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() =
                            YY_MOVE(yystack_[0].value.as<CNode::Fieldname>());
                    }
#line 3318 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 216:
#line 976 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() =
                            YY_MOVE(yystack_[0].value.as<CNode::Fieldname>());
                    }
#line 3324 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 217:
#line 976 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() =
                            YY_MOVE(yystack_[0].value.as<CNode::Fieldname>());
                    }
#line 3330 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 218:
#line 976 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() =
                            YY_MOVE(yystack_[0].value.as<CNode::Fieldname>());
                    }
#line 3336 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 219:
#line 980 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"_id"};
                    }
#line 3344 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 220:
#line 986 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 3350 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 221:
#line 986 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 3356 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 222:
#line 986 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 3362 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 223:
#line 986 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 3368 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 224:
#line 986 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 3374 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 225:
#line 986 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 3380 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 226:
#line 986 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 3386 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 227:
#line 986 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 3392 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 228:
#line 986 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 3398 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 229:
#line 986 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 3404 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 230:
#line 986 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 3410 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 231:
#line 986 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 3416 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 232:
#line 986 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 3422 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 233:
#line 987 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 3428 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 234:
#line 987 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 3434 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 235:
#line 987 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 3440 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 236:
#line 987 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 3446 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 237:
#line 991 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::add, YY_MOVE(yystack_[1].value.as<CNode>())}}};
                    }
#line 3455 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 238:
#line 998 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::atan2, YY_MOVE(yystack_[1].value.as<CNode>())}}};
                    }
#line 3464 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 239:
#line 1004 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::abs, YY_MOVE(yystack_[1].value.as<CNode>())}}};
                    }
#line 3472 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 240:
#line 1009 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::ceil, YY_MOVE(yystack_[1].value.as<CNode>())}}};
                    }
#line 3480 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 241:
#line 1014 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::divide,
                             CNode{CNode::ArrayChildren{YY_MOVE(yystack_[3].value.as<CNode>()),
                                                        YY_MOVE(yystack_[2].value.as<CNode>())}}}}};
                    }
#line 3489 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 242:
#line 1020 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::exponent, YY_MOVE(yystack_[1].value.as<CNode>())}}};
                    }
#line 3497 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 243:
#line 1025 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::floor, YY_MOVE(yystack_[1].value.as<CNode>())}}};
                    }
#line 3505 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 244:
#line 1030 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::ln, YY_MOVE(yystack_[1].value.as<CNode>())}}};
                    }
#line 3513 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 245:
#line 1035 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::log,
                             CNode{CNode::ArrayChildren{YY_MOVE(yystack_[3].value.as<CNode>()),
                                                        YY_MOVE(yystack_[2].value.as<CNode>())}}}}};
                    }
#line 3522 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 246:
#line 1041 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::logten, YY_MOVE(yystack_[1].value.as<CNode>())}}};
                    }
#line 3530 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 247:
#line 1046 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::mod,
                             CNode{CNode::ArrayChildren{YY_MOVE(yystack_[3].value.as<CNode>()),
                                                        YY_MOVE(yystack_[2].value.as<CNode>())}}}}};
                    }
#line 3539 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 248:
#line 1052 "src/mongo/db/cst/pipeline_grammar.yy"
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
#line 3551 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 249:
#line 1061 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::pow,
                             CNode{CNode::ArrayChildren{YY_MOVE(yystack_[3].value.as<CNode>()),
                                                        YY_MOVE(yystack_[2].value.as<CNode>())}}}}};
                    }
#line 3560 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 250:
#line 1067 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::round,
                             CNode{CNode::ArrayChildren{YY_MOVE(yystack_[3].value.as<CNode>()),
                                                        YY_MOVE(yystack_[2].value.as<CNode>())}}}}};
                    }
#line 3569 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 251:
#line 1073 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::sqrt, YY_MOVE(yystack_[1].value.as<CNode>())}}};
                    }
#line 3577 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 252:
#line 1078 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::subtract,
                             CNode{CNode::ArrayChildren{YY_MOVE(yystack_[3].value.as<CNode>()),
                                                        YY_MOVE(yystack_[2].value.as<CNode>())}}}}};
                    }
#line 3586 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 253:
#line 1084 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::trunc,
                             CNode{CNode::ArrayChildren{YY_MOVE(yystack_[3].value.as<CNode>()),
                                                        YY_MOVE(yystack_[2].value.as<CNode>())}}}}};
                    }
#line 3595 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 254:
#line 1090 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 3601 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 255:
#line 1090 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 3607 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 256:
#line 1090 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 3613 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 257:
#line 1094 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::andExpr, YY_MOVE(yystack_[1].value.as<CNode>())}}};
                    }
#line 3622 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 258:
#line 1101 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::orExpr, YY_MOVE(yystack_[1].value.as<CNode>())}}};
                    }
#line 3631 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 259:
#line 1108 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::notExpr,
                             CNode{CNode::ArrayChildren{YY_MOVE(yystack_[2].value.as<CNode>())}}}}};
                    }
#line 3640 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 260:
#line 1115 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 3646 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 261:
#line 1115 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 3652 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 262:
#line 1115 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 3658 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 263:
#line 1115 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 3664 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 264:
#line 1115 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 3670 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 265:
#line 1115 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 3676 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 266:
#line 1115 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 3682 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 267:
#line 1116 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 3688 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 268:
#line 1116 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 3694 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 269:
#line 1116 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 3700 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 270:
#line 1116 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 3706 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 271:
#line 1116 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 3712 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 272:
#line 1116 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 3718 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 273:
#line 1116 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 3724 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 274:
#line 1116 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 3730 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 275:
#line 1117 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 3736 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 276:
#line 1117 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 3742 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 277:
#line 1117 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 3748 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 278:
#line 1117 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 3754 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 279:
#line 1117 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 3760 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 280:
#line 1117 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 3766 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 281:
#line 1117 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 3772 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 282:
#line 1121 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::concat, CNode{CNode::ArrayChildren{}}}}};
                        auto&& others = YY_MOVE(yystack_[2].value.as<std::vector<CNode>>());
                        auto&& array =
                            yylhs.value.as<CNode>().objectChildren()[0].second.arrayChildren();
                        array.insert(array.end(), others.begin(), others.end());
                    }
#line 3784 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 283:
#line 1131 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<std::pair<CNode::Fieldname, CNode>>() =
                            std::pair{KeyFieldname::formatArg, CNode{KeyValue::absentKey}};
                    }
#line 3792 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 284:
#line 1134 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<std::pair<CNode::Fieldname, CNode>>() = std::pair{
                            KeyFieldname::formatArg, YY_MOVE(yystack_[0].value.as<CNode>())};
                    }
#line 3800 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 285:
#line 1140 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<std::pair<CNode::Fieldname, CNode>>() =
                            std::pair{KeyFieldname::timezoneArg, CNode{KeyValue::absentKey}};
                    }
#line 3808 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 286:
#line 1143 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<std::pair<CNode::Fieldname, CNode>>() = std::pair{
                            KeyFieldname::timezoneArg, YY_MOVE(yystack_[0].value.as<CNode>())};
                    }
#line 3816 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 287:
#line 1150 "src/mongo/db/cst/pipeline_grammar.yy"
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
#line 3826 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 288:
#line 1159 "src/mongo/db/cst/pipeline_grammar.yy"
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
#line 3836 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 289:
#line 1167 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<std::vector<CNode>>() = CNode::ArrayChildren{};
                    }
#line 3844 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 290:
#line 1170 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<std::vector<CNode>>() =
                            CNode::ArrayChildren{YY_MOVE(yystack_[0].value.as<CNode>())};
                    }
#line 3852 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 291:
#line 1173 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<std::vector<CNode>>() =
                            CNode::ArrayChildren{YY_MOVE(yystack_[1].value.as<CNode>()),
                                                 YY_MOVE(yystack_[0].value.as<CNode>())};
                    }
#line 3860 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 292:
#line 1180 "src/mongo/db/cst/pipeline_grammar.yy"
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
#line 3872 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 293:
#line 1191 "src/mongo/db/cst/pipeline_grammar.yy"
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
#line 3884 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 294:
#line 1201 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<std::pair<CNode::Fieldname, CNode>>() =
                            std::pair{KeyFieldname::charsArg, CNode{KeyValue::absentKey}};
                    }
#line 3892 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 295:
#line 1204 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<std::pair<CNode::Fieldname, CNode>>() = std::pair{
                            KeyFieldname::charsArg, YY_MOVE(yystack_[0].value.as<CNode>())};
                    }
#line 3900 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 296:
#line 1210 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::ltrim,
                             CNode{CNode::ObjectChildren{
                                 {KeyFieldname::inputArg, YY_MOVE(yystack_[2].value.as<CNode>())},
                                 YY_MOVE(yystack_[4]
                                             .value.as<std::pair<CNode::Fieldname, CNode>>())}}}}};
                    }
#line 3910 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 297:
#line 1218 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::rtrim,
                             CNode{CNode::ObjectChildren{
                                 {KeyFieldname::inputArg, YY_MOVE(yystack_[2].value.as<CNode>())},
                                 YY_MOVE(yystack_[4]
                                             .value.as<std::pair<CNode::Fieldname, CNode>>())}}}}};
                    }
#line 3920 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 298:
#line 1226 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::trim,
                             CNode{CNode::ObjectChildren{
                                 {KeyFieldname::inputArg, YY_MOVE(yystack_[2].value.as<CNode>())},
                                 YY_MOVE(yystack_[4]
                                             .value.as<std::pair<CNode::Fieldname, CNode>>())}}}}};
                    }
#line 3930 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 299:
#line 1234 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<std::pair<CNode::Fieldname, CNode>>() =
                            std::pair{KeyFieldname::optionsArg, CNode{KeyValue::absentKey}};
                    }
#line 3938 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 300:
#line 1237 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<std::pair<CNode::Fieldname, CNode>>() = std::pair{
                            KeyFieldname::optionsArg, YY_MOVE(yystack_[0].value.as<CNode>())};
                    }
#line 3946 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 301:
#line 1242 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        // Note that the order of these arguments must match the constructor for the
                        // regex expression.
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::inputArg, YY_MOVE(yystack_[4].value.as<CNode>())},
                            {KeyFieldname::regexArg, YY_MOVE(yystack_[1].value.as<CNode>())},
                            YY_MOVE(yystack_[3].value.as<std::pair<CNode::Fieldname, CNode>>())}};
                    }
#line 3958 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 302:
#line 1251 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::regexFind, YY_MOVE(yystack_[1].value.as<CNode>())}}};
                    }
#line 3966 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 303:
#line 1257 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::regexFindAll, YY_MOVE(yystack_[1].value.as<CNode>())}}};
                    }
#line 3974 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 304:
#line 1263 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::regexMatch, YY_MOVE(yystack_[1].value.as<CNode>())}}};
                    }
#line 3982 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 305:
#line 1270 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::replaceOne,
                             CNode{CNode::ObjectChildren{
                                 {KeyFieldname::inputArg, YY_MOVE(yystack_[4].value.as<CNode>())},
                                 {KeyFieldname::findArg, YY_MOVE(yystack_[6].value.as<CNode>())},
                                 {KeyFieldname::replacementArg,
                                  YY_MOVE(yystack_[2].value.as<CNode>())}}}}}};
                    }
#line 3993 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 306:
#line 1280 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::replaceAll,
                             CNode{CNode::ObjectChildren{
                                 {KeyFieldname::inputArg, YY_MOVE(yystack_[4].value.as<CNode>())},
                                 {KeyFieldname::findArg, YY_MOVE(yystack_[6].value.as<CNode>())},
                                 {KeyFieldname::replacementArg,
                                  YY_MOVE(yystack_[2].value.as<CNode>())}}}}}};
                    }
#line 4004 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 307:
#line 1289 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::split,
                             CNode{CNode::ArrayChildren{YY_MOVE(yystack_[3].value.as<CNode>()),
                                                        YY_MOVE(yystack_[2].value.as<CNode>())}}}}};
                    }
#line 4013 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 308:
#line 1296 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::strLenBytes, YY_MOVE(yystack_[1].value.as<CNode>())}}};
                    }
#line 4022 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 309:
#line 1303 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::strLenCP, YY_MOVE(yystack_[1].value.as<CNode>())}}};
                    }
#line 4031 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 310:
#line 1311 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::strcasecmp,
                             CNode{CNode::ArrayChildren{YY_MOVE(yystack_[3].value.as<CNode>()),
                                                        YY_MOVE(yystack_[2].value.as<CNode>())}}}}};
                    }
#line 4040 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 311:
#line 1319 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::substr,
                             CNode{CNode::ArrayChildren{YY_MOVE(yystack_[4].value.as<CNode>()),
                                                        YY_MOVE(yystack_[3].value.as<CNode>()),
                                                        YY_MOVE(yystack_[2].value.as<CNode>())}}}}};
                    }
#line 4049 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 312:
#line 1327 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::substrBytes,
                             CNode{CNode::ArrayChildren{YY_MOVE(yystack_[4].value.as<CNode>()),
                                                        YY_MOVE(yystack_[3].value.as<CNode>()),
                                                        YY_MOVE(yystack_[2].value.as<CNode>())}}}}};
                    }
#line 4058 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 313:
#line 1335 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::substrCP,
                             CNode{CNode::ArrayChildren{YY_MOVE(yystack_[4].value.as<CNode>()),
                                                        YY_MOVE(yystack_[3].value.as<CNode>()),
                                                        YY_MOVE(yystack_[2].value.as<CNode>())}}}}};
                    }
#line 4067 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 314:
#line 1342 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::toLower, YY_MOVE(yystack_[1].value.as<CNode>())}}};
                    }
#line 4075 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 315:
#line 1348 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::toUpper, YY_MOVE(yystack_[1].value.as<CNode>())}}};
                    }
#line 4083 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 316:
#line 1354 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 4089 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 317:
#line 1354 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 4095 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 318:
#line 1358 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::constExpr,
                             CNode{CNode::ArrayChildren{YY_MOVE(yystack_[2].value.as<CNode>())}}}}};
                    }
#line 4104 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 319:
#line 1365 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::literal,
                             CNode{CNode::ArrayChildren{YY_MOVE(yystack_[2].value.as<CNode>())}}}}};
                    }
#line 4113 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 320:
#line 1372 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 4119 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 321:
#line 1372 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 4125 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 322:
#line 1376 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 4131 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 323:
#line 1376 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 4137 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 324:
#line 1380 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() =
                            CNode{YY_MOVE(yystack_[1].value.as<std::vector<CNode>>())};
                    }
#line 4145 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 325:
#line 1386 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                    }
#line 4151 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 326:
#line 1387 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<std::vector<CNode>>() =
                            YY_MOVE(yystack_[0].value.as<std::vector<CNode>>());
                        yylhs.value.as<std::vector<CNode>>().emplace_back(
                            YY_MOVE(yystack_[1].value.as<CNode>()));
                    }
#line 4160 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 327:
#line 1394 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[1].value.as<CNode>());
                    }
#line 4168 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 328:
#line 1400 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode::noopLeaf();
                    }
#line 4176 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 329:
#line 1403 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[1].value.as<CNode>());
                        yylhs.value.as<CNode>().objectChildren().emplace_back(
                            YY_MOVE(yystack_[0].value.as<std::pair<CNode::Fieldname, CNode>>()));
                    }
#line 4185 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 330:
#line 1410 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<std::pair<CNode::Fieldname, CNode>>() = {
                            YY_MOVE(yystack_[1].value.as<CNode::Fieldname>()),
                            YY_MOVE(yystack_[0].value.as<CNode>())};
                    }
#line 4193 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 331:
#line 1417 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() =
                            YY_MOVE(yystack_[0].value.as<CNode::Fieldname>());
                    }
#line 4199 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 332:
#line 1418 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() =
                            YY_MOVE(yystack_[0].value.as<CNode::Fieldname>());
                    }
#line 4205 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 333:
#line 1419 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() =
                            YY_MOVE(yystack_[0].value.as<CNode::Fieldname>());
                    }
#line 4211 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 334:
#line 1420 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() =
                            YY_MOVE(yystack_[0].value.as<CNode::Fieldname>());
                    }
#line 4217 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 335:
#line 1421 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() =
                            YY_MOVE(yystack_[0].value.as<CNode::Fieldname>());
                    }
#line 4223 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 336:
#line 1424 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 4229 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 337:
#line 1424 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 4235 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 338:
#line 1424 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 4241 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 339:
#line 1424 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 4247 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 340:
#line 1424 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 4253 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 341:
#line 1424 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 4259 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 342:
#line 1424 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 4265 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 343:
#line 1426 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::cmp, YY_MOVE(yystack_[1].value.as<CNode>())}}};
                    }
#line 4274 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 344:
#line 1431 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::eq, YY_MOVE(yystack_[1].value.as<CNode>())}}};
                    }
#line 4283 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 345:
#line 1436 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::gt, YY_MOVE(yystack_[1].value.as<CNode>())}}};
                    }
#line 4292 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 346:
#line 1441 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::gte, YY_MOVE(yystack_[1].value.as<CNode>())}}};
                    }
#line 4301 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 347:
#line 1446 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::lt, YY_MOVE(yystack_[1].value.as<CNode>())}}};
                    }
#line 4310 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 348:
#line 1451 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::lte, YY_MOVE(yystack_[1].value.as<CNode>())}}};
                    }
#line 4319 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 349:
#line 1456 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::ne, YY_MOVE(yystack_[1].value.as<CNode>())}}};
                    }
#line 4328 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 350:
#line 1462 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 4334 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 351:
#line 1463 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 4340 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 352:
#line 1464 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 4346 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 353:
#line 1465 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 4352 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 354:
#line 1466 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 4358 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 355:
#line 1467 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 4364 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 356:
#line 1468 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 4370 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 357:
#line 1469 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 4376 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 358:
#line 1470 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 4382 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 359:
#line 1471 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 4388 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 360:
#line 1476 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<std::pair<CNode::Fieldname, CNode>>() =
                            std::pair{KeyFieldname::onErrorArg, CNode{KeyValue::absentKey}};
                    }
#line 4396 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 361:
#line 1479 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<std::pair<CNode::Fieldname, CNode>>() = std::pair{
                            KeyFieldname::onErrorArg, YY_MOVE(yystack_[0].value.as<CNode>())};
                    }
#line 4404 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 362:
#line 1486 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<std::pair<CNode::Fieldname, CNode>>() =
                            std::pair{KeyFieldname::onNullArg, CNode{KeyValue::absentKey}};
                    }
#line 4412 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 363:
#line 1489 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<std::pair<CNode::Fieldname, CNode>>() = std::pair{
                            KeyFieldname::onNullArg, YY_MOVE(yystack_[0].value.as<CNode>())};
                    }
#line 4420 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 364:
#line 1496 "src/mongo/db/cst/pipeline_grammar.yy"
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
#line 4431 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 365:
#line 1505 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::toBool, YY_MOVE(yystack_[1].value.as<CNode>())}}};
                    }
#line 4439 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 366:
#line 1510 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::toDate, YY_MOVE(yystack_[1].value.as<CNode>())}}};
                    }
#line 4447 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 367:
#line 1515 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::toDecimal, YY_MOVE(yystack_[1].value.as<CNode>())}}};
                    }
#line 4455 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 368:
#line 1520 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::toDouble, YY_MOVE(yystack_[1].value.as<CNode>())}}};
                    }
#line 4463 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 369:
#line 1525 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::toInt, YY_MOVE(yystack_[1].value.as<CNode>())}}};
                    }
#line 4471 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 370:
#line 1530 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::toLong, YY_MOVE(yystack_[1].value.as<CNode>())}}};
                    }
#line 4479 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 371:
#line 1535 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::toObjectId, YY_MOVE(yystack_[1].value.as<CNode>())}}};
                    }
#line 4487 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 372:
#line 1540 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::toString, YY_MOVE(yystack_[1].value.as<CNode>())}}};
                    }
#line 4495 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 373:
#line 1545 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::type, YY_MOVE(yystack_[1].value.as<CNode>())}}};
                    }
#line 4503 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;


#line 4507 "src/mongo/db/cst/pipeline_parser_gen.cpp"

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


const short PipelineParserGen::yypact_ninf_ = -557;

const signed char PipelineParserGen::yytable_ninf_ = -1;

const short PipelineParserGen::yypact_[] = {
    -56,  -40,  -38,  45,   -6,   -557, -557, -557, -557, 119,  37,   854,  -2,   -8,   0,    3,
    -8,   -557, 47,   -557, -557, -557, -557, -557, -557, -557, -557, -557, -557, -557, -557, -557,
    -557, -557, -557, -557, -557, -557, -557, -557, -557, -557, -557, -557, -557, 529,  -557, -557,
    -557, -557, 51,   -557, -557, -557, -557, -557, -557, -557, -557, -557, -557, -557, -557, -557,
    -557, 73,   -557, 88,   24,   -6,   -557, -557, 529,  -557, -557, -557, -557, -557, -557, -557,
    -557, -557, -557, -557, -557, -557, -557, -557, -557, -557, -557, -557, -557, -557, -557, -557,
    -557, -557, -557, -557, -557, -557, -557, -557, -557, -557, -557, -557, -557, -557, -557, -557,
    -557, -557, -557, -557, -557, -557, 359,  -8,   8,    -557, -557, 529,  85,   454,  -557, -557,
    -557, -557, -557, -557, -557, -557, -557, -557, -557, -557, -557, -557, -557, -557, -557, -557,
    738,  -557, -557, -557, -557, -557, -557, -557, -557, -557, -557, -557, -557, -557, -557, -557,
    -557, -557, -557, -557, -557, -557, -557, -557, -557, -557, -557, -557, -557, -557, -557, -557,
    -557, -557, -557, -557, -557, -557, -557, -557, -557, -557, -557, -557, -557, -557, -557, -557,
    -557, -557, -557, 738,  -557, -557, -557, -557, -557, 72,   113,  -557, -557, -557, -557, -557,
    -557, -557, -557, 529,  -557, -557, -557, -557, -557, -557, -557, 549,  664,  -557, -557, -557,
    -557, -557, -557, -557, -557, -557, -557, -557, -557, -557, -557, -557, -557, -557, -557, -557,
    -557, -557, -557, -557, -557, -557, -557, -557, -557, -557, -557, -557, -557, -557, -557, -557,
    -557, -557, -557, -557, -557, -557, -557, -557, -557, -557, -557, -557, -557, -557, -557, -557,
    -557, -557, -557, -557, -557, -557, -557, -557, -557, -557, -557, -557, -557, -557, -557, -557,
    -557, -557, -557, -557, -557, -557, -557, -557, -557, -557, -557, -557, -557, -557, -557, -557,
    -557, -557, -557, -557, -557, -557, -557, -557, -22,  -557, -557, 549,  -557, 100,  549,  61,
    61,   62,   549,  62,   63,   64,   -557, -557, -557, 67,   62,   549,  549,  62,   62,   68,
    75,   76,   549,  79,   549,  62,   62,   -557, 83,   86,   62,   87,   61,   89,   -557, -557,
    -557, -557, -557, 92,   -557, 96,   549,  98,   549,  549,  99,   103,  105,  107,  549,  549,
    549,  549,  549,  549,  549,  549,  549,  549,  -557, 108,  549,  785,  124,  -557, -557, 158,
    159,  160,  549,  161,  162,  164,  549,  529,  189,  193,  195,  549,  168,  169,  170,  171,
    172,  549,  549,  529,  175,  549,  176,  177,  179,  210,  549,  549,  181,  549,  182,  549,
    183,  208,  185,  186,  214,  215,  549,  210,  549,  196,  549,  197,  198,  549,  549,  549,
    549,  200,  202,  203,  204,  206,  207,  209,  211,  212,  213,  210,  549,  216,  -557, 549,
    -557, -557, -557, -557, -557, -557, -557, -557, -557, 549,  -557, -557, -557, 220,  221,  549,
    549,  549,  549,  -557, -557, -557, -557, -557, 549,  549,  222,  -557, 549,  -557, -557, -557,
    549,  219,  549,  549,  -557, 223,  -557, 549,  -557, 549,  -557, -557, 549,  549,  549,  232,
    549,  -557, 549,  -557, -557, 549,  549,  549,  549,  -557, -557, -557, -557, -557, -557, -557,
    -557, -557, -557, 234,  549,  -557, -557, 224,  225,  227,  237,  249,  249,  230,  549,  549,
    235,  233,  -557, 549,  238,  549,  236,  239,  260,  264,  265,  243,  549,  244,  245,  549,
    549,  549,  246,  549,  248,  -557, -557, -557, 549,  270,  549,  266,  266,  251,  549,  250,
    253,  -557, 254,  255,  256,  258,  -557, 259,  549,  272,  549,  549,  261,  262,  263,  268,
    267,  273,  277,  271,  278,  279,  -557, 549,  280,  -557, 549,  237,  270,  -557, -557, 281,
    282,  -557, 283,  -557, 284,  -557, -557, 549,  276,  295,  -557, 285,  -557, -557, 286,  287,
    288,  -557, 289,  -557, -557, 549,  -557, 270,  291,  -557, -557, -557, -557, 292,  549,  549,
    -557, -557, -557, -557, -557, 293,  294,  296,  -557, 297,  298,  299,  302,  -557, 304,  305,
    -557, -557, -557, -557};

const short PipelineParserGen::yydefact_[] = {
    0,   0,   0,   0,   5,   2,   59,  3,   1,   0,   0,   0,   0,   0,   0,   0,   0,   7,   0,
    9,   10,  11,  12,  13,  14,  4,   84,  73,  83,  80,  87,  81,  76,  78,  79,  86,  74,  85,
    88,  75,  82,  77,  58,  219, 66,  0,   65,  64,  63,  60,  0,   173, 171, 167, 169, 166, 168,
    170, 172, 18,  19,  20,  21,  23,  25,  0,   22,  0,   0,   5,   175, 174, 325, 328, 150, 153,
    154, 155, 156, 157, 158, 159, 160, 161, 162, 163, 164, 165, 151, 152, 185, 186, 187, 188, 189,
    194, 190, 191, 192, 195, 196, 62,  176, 177, 179, 180, 181, 193, 182, 183, 184, 320, 321, 322,
    323, 178, 61,  16,  0,   0,   0,   8,   6,   325, 0,   0,   113, 89,  91,  90,  114, 96,  128,
    92,  103, 129, 130, 115, 24,  97,  116, 117, 98,  99,  0,   131, 132, 93,  118, 119, 120, 100,
    101, 133, 121, 122, 102, 95,  94,  123, 134, 135, 136, 138, 137, 124, 139, 140, 125, 67,  70,
    71,  72,  69,  68,  143, 141, 142, 144, 145, 146, 126, 104, 105, 106, 107, 108, 109, 147, 110,
    111, 149, 148, 127, 112, 0,   55,  56,  57,  54,  26,  0,   0,   326, 324, 327, 332, 333, 334,
    331, 335, 0,   329, 49,  48,  47,  45,  41,  43,  197, 212, 40,  42,  44,  46,  36,  37,  38,
    39,  50,  51,  52,  29,  30,  31,  32,  33,  34,  35,  27,  53,  202, 203, 204, 220, 221, 205,
    254, 255, 256, 206, 316, 317, 209, 260, 261, 262, 263, 264, 265, 266, 267, 268, 269, 270, 271,
    272, 273, 274, 275, 276, 277, 278, 279, 281, 280, 207, 336, 337, 338, 339, 340, 341, 342, 208,
    350, 351, 352, 353, 354, 355, 356, 357, 358, 359, 222, 223, 224, 225, 226, 227, 228, 229, 230,
    231, 232, 233, 234, 235, 236, 28,  15,  0,   330, 199, 197, 200, 0,   0,   0,   0,   0,   0,
    0,   0,   0,   7,   7,   7,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,
    0,   7,   0,   0,   0,   0,   0,   0,   7,   7,   7,   7,   7,   0,   7,   0,   0,   0,   0,
    0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   7,   0,   0,   0,
    0,   198, 210, 0,   0,   0,   0,   0,   0,   0,   197, 0,   0,   0,   0,   0,   0,   0,   0,
    0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   294, 0,   0,   0,   0,   0,   0,   0,   0,
    0,   0,   0,   0,   0,   294, 0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,
    0,   0,   0,   0,   0,   0,   294, 0,   0,   211, 0,   216, 217, 215, 218, 213, 17,  239, 237,
    257, 0,   238, 240, 343, 0,   0,   0,   0,   0,   0,   344, 242, 243, 345, 346, 0,   0,   0,
    244, 0,   246, 347, 348, 0,   0,   0,   0,   349, 0,   258, 0,   302, 0,   303, 304, 0,   0,
    0,   0,   0,   251, 0,   308, 309, 0,   0,   0,   0,   365, 366, 367, 368, 369, 370, 314, 371,
    372, 315, 0,   0,   373, 214, 0,   0,   0,   360, 283, 283, 0,   289, 289, 0,   0,   295, 0,
    0,   197, 0,   0,   299, 0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   201, 282,
    318, 0,   362, 0,   285, 285, 0,   290, 0,   0,   319, 0,   0,   0,   0,   259, 0,   0,   0,
    0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   361, 0,   0,   284, 0,   360, 362,
    241, 291, 0,   0,   245, 0,   247, 0,   249, 300, 0,   0,   0,   250, 0,   307, 310, 0,   0,
    0,   252, 0,   253, 363, 0,   286, 362, 0,   292, 293, 296, 248, 0,   0,   0,   297, 311, 312,
    313, 298, 0,   0,   0,   301, 0,   0,   0,   0,   288, 0,   0,   364, 287, 306, 305};

const short PipelineParserGen::yypgoto_[] = {
    -557, -557, -557, -117, -557, -112, 191,  -109, -115, -557, -557, -557, -557, -557, -114, -113,
    -105, -104, 4,    -98,  6,    -9,   12,   -96,  -79,  -42,  -84,  -557, -78,  -77,  -76,  -557,
    -73,  -71,  -69,  -43,  -557, -557, -557, -557, -557, -557, 231,  -557, -557, -557, -557, -557,
    -557, -557, -557, 136,  2,    -317, -67,  -189, -286, -557, -557, -557, -557, -557, -557, -557,
    -557, -557, -557, -557, -557, -557, -557, -557, -557, -557, -557, -557, -557, -557, -557, -219,
    -557, -557, -557, -557, -557, -557, -557, -557, -557, -557, -557, -557, -557, -557, -557, -557,
    -557, -557, -557, -557, -557, -557, -557, -557, -557, -557, -557, -557, -557, -557, -557, -557,
    -557, -557, -557, -557, -557, -557, -557, -557, -557, -557, -557, -557, -557, -557, -557, -245,
    -556, -181, -212, -396, -557, -304, 228,  -182, -557, -557, -557, -557, -17,  -557};

const short PipelineParserGen::yydefgoto_[] = {
    -1,  195, 447, 196, 45,  46,  198, 47,  48,  211, 200, 452, 212, 49,  90,  91,  92,  93,
    94,  95,  96,  97,  98,  99,  100, 123, 102, 103, 104, 105, 106, 107, 108, 109, 110, 314,
    112, 113, 114, 125, 115, 5,   10,  18,  19,  20,  21,  22,  23,  24,  118, 239, 63,  315,
    316, 387, 241, 242, 379, 243, 244, 245, 246, 247, 248, 249, 250, 251, 252, 253, 254, 255,
    256, 257, 258, 259, 260, 261, 262, 416, 263, 264, 265, 266, 267, 268, 269, 270, 271, 272,
    273, 274, 275, 276, 277, 278, 279, 280, 281, 282, 283, 284, 285, 286, 287, 288, 289, 290,
    291, 292, 293, 294, 295, 296, 297, 298, 299, 300, 301, 302, 303, 304, 305, 306, 307, 308,
    309, 553, 584, 555, 587, 481, 569, 317, 124, 559, 7,   11,  116, 3,   417, 68};

const short PipelineParserGen::yytable_[] = {
    67,  383, 111, 101, 61,  388, 197, 61,  206, 199, 210, 381, 52,  207, 397, 398, 209, 59,  66,
    60,  59,  404, 60,  406, 51,  62,  52,  495, 62,  111, 225, 226, 616, 384, 385, 4,   202, 53,
    6,   227, 228, 425, 54,  427, 428, 8,   229, 515, 230, 433, 434, 435, 436, 437, 438, 439, 440,
    441, 442, 630, 232, 445, 414, 1,   2,   231, 233, 234, 235, 457, 9,   236, 25,  237, 50,  238,
    64,  240, 466, 65,  111, 225, 226, 69,  472, 473, 461, 117, 476, 57,  227, 228, 119, 482, 483,
    120, 485, 229, 487, 230, 121, 55,  56,  57,  58,  494, 74,  496, 311, 498, 61,  232, 501, 502,
    503, 504, 231, 233, 234, 235, 204, 201, 236, 59,  237, 60,  238, 516, 240, 312, 518, 62,  418,
    419, 389, 382, 219, 386, 390, 391, 519, 396, 395, 401, 399, 400, 522, 523, 524, 525, 402, 403,
    407, 408, 405, 526, 527, 412, 410, 529, 453, 411, 413, 530, 415, 532, 533, 422, 111, 313, 535,
    424, 536, 426, 429, 537, 538, 539, 430, 541, 431, 542, 432, 444, 543, 544, 545, 546, 12,  13,
    14,  15,  16,  17,  454, 455, 456, 458, 459, 548, 460, 463, 464, 465, 467, 468, 469, 470, 471,
    558, 558, 475, 477, 478, 563, 479, 480, 484, 486, 488, 489, 490, 491, 573, 492, 493, 576, 577,
    578, 565, 580, 531, 497, 499, 500, 582, 505, 585, 506, 507, 508, 590, 509, 510, 540, 511, 547,
    512, 513, 514, 552, 598, 517, 600, 601, 520, 521, 528, 534, 549, 554, 550, 448, 551, 451, 557,
    612, 449, 562, 614, 450, 561, 566, 564, 567, 568, 570, 571, 572, 574, 575, 579, 621, 581, 583,
    591, 586, 589, 592, 599, 593, 594, 595, 596, 622, 597, 629, 602, 603, 604, 122, 613, 606, 380,
    605, 633, 634, 609, 607, 392, 393, 394, 608, 623, 610, 611, 208, 617, 618, 619, 620, 624, 625,
    626, 627, 628, 409, 631, 632, 635, 636, 310, 637, 638, 639, 640, 420, 421, 641, 423, 642, 643,
    615, 556, 588, 560, 0,   0,   111, 462, 0,   203, 0,   0,   0,   0,   0,   0,   0,   443, 111,
    474, 126, 127, 128, 26,  27,  28,  29,  30,  31,  32,  33,  34,  35,  36,  37,  38,  39,  40,
    41,  129, 0,   0,   130, 131, 132, 133, 134, 135, 136, 0,   137, 0,   0,   138, 139, 140, 141,
    142, 143, 144, 145, 146, 0,   147, 148, 149, 150, 0,   151, 152, 153, 154, 155, 156, 157, 158,
    159, 160, 161, 162, 163, 164, 165, 166, 167, 168, 169, 170, 171, 172, 173, 174, 0,   0,   175,
    176, 177, 178, 179, 180, 181, 182, 183, 184, 185, 186, 187, 188, 189, 190, 191, 192, 193, 194,
    44,  126, 127, 128, 26,  27,  28,  29,  30,  31,  32,  33,  34,  35,  36,  37,  38,  39,  40,
    41,  129, 0,   0,   130, 131, 132, 133, 134, 135, 136, 0,   137, 0,   0,   205, 139, 140, 141,
    142, 143, 43,  145, 146, 0,   147, 148, 149, 150, 0,   151, 152, 153, 154, 155, 156, 157, 158,
    159, 160, 161, 162, 163, 164, 165, 166, 167, 168, 169, 170, 171, 172, 173, 174, 0,   0,   175,
    176, 177, 178, 179, 180, 181, 182, 183, 184, 185, 186, 187, 188, 189, 190, 191, 192, 193, 194,
    44,  70,  71,  0,   0,   0,   0,   0,   0,   0,   51,  0,   52,  0,   0,   0,   0,   0,   0,
    0,   0,   70,  71,  53,  0,   0,   0,   0,   54,  0,   51,  0,   52,  0,   0,   0,   0,   0,
    0,   0,   0,   0,   0,   53,  0,   0,   0,   0,   54,  0,   0,   0,   0,   72,  73,  0,   0,
    0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   219, 220, 0,
    74,  75,  76,  77,  78,  79,  80,  81,  82,  83,  84,  55,  56,  57,  58,  85,  86,  87,  88,
    89,  74,  75,  76,  77,  78,  79,  80,  81,  82,  83,  84,  55,  56,  57,  58,  85,  86,  87,
    88,  89,  318, 319, 320, 0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,
    0,   0,   321, 0,   0,   322, 323, 324, 325, 326, 327, 328, 0,   329, 0,   0,   0,   330, 331,
    332, 333, 334, 0,   335, 336, 0,   337, 338, 339, 340, 0,   341, 342, 343, 344, 345, 346, 347,
    348, 349, 350, 351, 352, 353, 354, 355, 356, 357, 358, 0,   0,   0,   0,   0,   0,   0,   0,
    359, 360, 361, 362, 363, 364, 365, 366, 367, 368, 369, 370, 371, 372, 373, 374, 375, 376, 377,
    378, 213, 214, 0,   0,   0,   0,   0,   0,   0,   215, 0,   216, 0,   0,   0,   0,   0,   0,
    0,   0,   0,   0,   217, 0,   0,   0,   0,   218, 0,   0,   26,  27,  28,  29,  30,  31,  32,
    33,  34,  35,  36,  37,  38,  39,  40,  41,  0,   0,   0,   0,   0,   0,   219, 220, 0,   0,
    0,   0,   0,   0,   446, 0,   0,   0,   0,   0,   43,  0,   0,   0,   0,   0,   0,   0,   0,
    74,  75,  76,  77,  78,  79,  80,  81,  82,  83,  84,  221, 222, 223, 224, 85,  86,  87,  169,
    170, 171, 172, 173, 174, 26,  27,  28,  29,  30,  31,  32,  33,  34,  35,  36,  37,  38,  39,
    40,  41,  0,   0,   0,   0,   0,   0,   44,  0,   0,   0,   0,   0,   0,   0,   42,  0,   0,
    0,   0,   0,   43,  0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,
    0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,
    0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,
    0,   44};

const short PipelineParserGen::yycheck_[] = {
    17,  318, 45,  45,  13,  322, 118, 16,  125, 118, 125, 315, 34,  125, 331, 332, 125, 13,  16,
    13,  16,  338, 16,  340, 32,  13,  34,  423, 16,  72,  144, 144, 588, 319, 320, 75,  120, 45,
    76,  144, 144, 358, 50,  360, 361, 0,   144, 443, 144, 366, 367, 368, 369, 370, 371, 372, 373,
    374, 375, 615, 144, 378, 348, 119, 120, 144, 144, 144, 144, 386, 76,  144, 35,  144, 76,  144,
    76,  144, 395, 76,  123, 195, 195, 36,  401, 402, 390, 36,  405, 111, 195, 195, 19,  410, 411,
    7,   413, 195, 415, 195, 76,  109, 110, 111, 112, 422, 98,  424, 36,  426, 119, 195, 429, 430,
    431, 432, 195, 195, 195, 195, 35,  119, 195, 119, 195, 119, 195, 444, 195, 16,  447, 119, 351,
    352, 323, 35,  75,  75,  75,  75,  457, 330, 75,  75,  333, 334, 463, 464, 465, 466, 75,  75,
    341, 342, 75,  472, 473, 346, 75,  476, 36,  75,  75,  480, 75,  482, 483, 75,  211, 211, 487,
    75,  489, 75,  75,  492, 493, 494, 75,  496, 75,  498, 75,  75,  501, 502, 503, 504, 69,  70,
    71,  72,  73,  74,  36,  36,  36,  36,  36,  516, 36,  12,  9,   8,   36,  36,  36,  36,  36,
    526, 527, 36,  36,  36,  531, 36,  6,   36,  36,  36,  12,  36,  36,  540, 10,  10,  543, 544,
    545, 533, 547, 12,  36,  36,  36,  552, 36,  554, 36,  36,  36,  558, 36,  36,  12,  36,  12,
    36,  36,  36,  13,  568, 36,  570, 571, 35,  35,  35,  35,  35,  11,  36,  379, 36,  379, 35,
    583, 379, 35,  586, 379, 36,  36,  35,  35,  15,  12,  12,  35,  35,  35,  35,  599, 35,  14,
    35,  20,  36,  35,  17,  36,  36,  36,  35,  18,  36,  613, 36,  36,  36,  69,  21,  35,  312,
    36,  622, 623, 36,  35,  326, 327, 328, 35,  18,  36,  36,  125, 36,  36,  36,  36,  36,  36,
    36,  36,  36,  343, 36,  36,  36,  36,  195, 36,  36,  36,  36,  353, 354, 36,  356, 36,  36,
    587, 524, 556, 527, -1,  -1,  391, 391, -1,  123, -1,  -1,  -1,  -1,  -1,  -1,  -1,  376, 403,
    403, 3,   4,   5,   6,   7,   8,   9,   10,  11,  12,  13,  14,  15,  16,  17,  18,  19,  20,
    21,  22,  -1,  -1,  25,  26,  27,  28,  29,  30,  31,  -1,  33,  -1,  -1,  36,  37,  38,  39,
    40,  41,  42,  43,  44,  -1,  46,  47,  48,  49,  -1,  51,  52,  53,  54,  55,  56,  57,  58,
    59,  60,  61,  62,  63,  64,  65,  66,  67,  68,  69,  70,  71,  72,  73,  74,  -1,  -1,  77,
    78,  79,  80,  81,  82,  83,  84,  85,  86,  87,  88,  89,  90,  91,  92,  93,  94,  95,  96,
    97,  3,   4,   5,   6,   7,   8,   9,   10,  11,  12,  13,  14,  15,  16,  17,  18,  19,  20,
    21,  22,  -1,  -1,  25,  26,  27,  28,  29,  30,  31,  -1,  33,  -1,  -1,  36,  37,  38,  39,
    40,  41,  42,  43,  44,  -1,  46,  47,  48,  49,  -1,  51,  52,  53,  54,  55,  56,  57,  58,
    59,  60,  61,  62,  63,  64,  65,  66,  67,  68,  69,  70,  71,  72,  73,  74,  -1,  -1,  77,
    78,  79,  80,  81,  82,  83,  84,  85,  86,  87,  88,  89,  90,  91,  92,  93,  94,  95,  96,
    97,  23,  24,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  32,  -1,  34,  -1,  -1,  -1,  -1,  -1,  -1,
    -1,  -1,  23,  24,  45,  -1,  -1,  -1,  -1,  50,  -1,  32,  -1,  34,  -1,  -1,  -1,  -1,  -1,
    -1,  -1,  -1,  -1,  -1,  45,  -1,  -1,  -1,  -1,  50,  -1,  -1,  -1,  -1,  75,  76,  -1,  -1,
    -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  75,  76,  -1,
    98,  99,  100, 101, 102, 103, 104, 105, 106, 107, 108, 109, 110, 111, 112, 113, 114, 115, 116,
    117, 98,  99,  100, 101, 102, 103, 104, 105, 106, 107, 108, 109, 110, 111, 112, 113, 114, 115,
    116, 117, 3,   4,   5,   -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,
    -1,  -1,  22,  -1,  -1,  25,  26,  27,  28,  29,  30,  31,  -1,  33,  -1,  -1,  -1,  37,  38,
    39,  40,  41,  -1,  43,  44,  -1,  46,  47,  48,  49,  -1,  51,  52,  53,  54,  55,  56,  57,
    58,  59,  60,  61,  62,  63,  64,  65,  66,  67,  68,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,
    77,  78,  79,  80,  81,  82,  83,  84,  85,  86,  87,  88,  89,  90,  91,  92,  93,  94,  95,
    96,  23,  24,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  32,  -1,  34,  -1,  -1,  -1,  -1,  -1,  -1,
    -1,  -1,  -1,  -1,  45,  -1,  -1,  -1,  -1,  50,  -1,  -1,  6,   7,   8,   9,   10,  11,  12,
    13,  14,  15,  16,  17,  18,  19,  20,  21,  -1,  -1,  -1,  -1,  -1,  -1,  75,  76,  -1,  -1,
    -1,  -1,  -1,  -1,  36,  -1,  -1,  -1,  -1,  -1,  42,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,
    98,  99,  100, 101, 102, 103, 104, 105, 106, 107, 108, 109, 110, 111, 112, 113, 114, 115, 69,
    70,  71,  72,  73,  74,  6,   7,   8,   9,   10,  11,  12,  13,  14,  15,  16,  17,  18,  19,
    20,  21,  -1,  -1,  -1,  -1,  -1,  -1,  97,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  36,  -1,  -1,
    -1,  -1,  -1,  42,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,
    -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,
    -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,
    -1,  97};

const short PipelineParserGen::yystos_[] = {
    0,   119, 120, 260, 75,  162, 76,  257, 0,   76,  163, 258, 69,  70,  71,  72,  73,  74,  164,
    165, 166, 167, 168, 169, 170, 35,  6,   7,   8,   9,   10,  11,  12,  13,  14,  15,  16,  17,
    18,  19,  20,  21,  36,  42,  97,  125, 126, 128, 129, 134, 76,  32,  34,  45,  50,  109, 110,
    111, 112, 139, 141, 142, 143, 173, 76,  76,  173, 261, 262, 36,  23,  24,  75,  76,  98,  99,
    100, 101, 102, 103, 104, 105, 106, 107, 108, 113, 114, 115, 116, 117, 135, 136, 137, 138, 139,
    140, 141, 142, 143, 144, 145, 146, 147, 148, 149, 150, 151, 152, 153, 154, 155, 156, 157, 158,
    159, 161, 259, 36,  171, 19,  7,   76,  163, 146, 255, 160, 3,   4,   5,   22,  25,  26,  27,
    28,  29,  30,  31,  33,  36,  37,  38,  39,  40,  41,  42,  43,  44,  46,  47,  48,  49,  51,
    52,  53,  54,  55,  56,  57,  58,  59,  60,  61,  62,  63,  64,  65,  66,  67,  68,  69,  70,
    71,  72,  73,  74,  77,  78,  79,  80,  81,  82,  83,  84,  85,  86,  87,  88,  89,  90,  91,
    92,  93,  94,  95,  96,  122, 124, 126, 127, 128, 131, 173, 147, 255, 35,  36,  124, 126, 127,
    128, 129, 130, 133, 23,  24,  32,  34,  45,  50,  75,  76,  109, 110, 111, 112, 135, 136, 137,
    138, 140, 144, 145, 147, 149, 150, 151, 153, 154, 155, 172, 175, 177, 178, 180, 181, 182, 183,
    184, 185, 186, 187, 188, 189, 190, 191, 192, 193, 194, 195, 196, 197, 198, 199, 201, 202, 203,
    204, 205, 206, 207, 208, 209, 210, 211, 212, 213, 214, 215, 216, 217, 218, 219, 220, 221, 222,
    223, 224, 225, 226, 227, 228, 229, 230, 231, 232, 233, 234, 235, 236, 237, 238, 239, 240, 241,
    242, 243, 244, 245, 246, 247, 172, 36,  16,  146, 156, 174, 175, 254, 3,   4,   5,   22,  25,
    26,  27,  28,  29,  30,  31,  33,  37,  38,  39,  40,  41,  43,  44,  46,  47,  48,  49,  51,
    52,  53,  54,  55,  56,  57,  58,  59,  60,  61,  62,  63,  64,  65,  66,  67,  68,  77,  78,
    79,  80,  81,  82,  83,  84,  85,  86,  87,  88,  89,  90,  91,  92,  93,  94,  95,  96,  179,
    142, 254, 35,  174, 177, 177, 75,  176, 174, 176, 75,  75,  261, 261, 261, 75,  176, 174, 174,
    176, 176, 75,  75,  75,  174, 75,  174, 176, 176, 261, 75,  75,  176, 75,  177, 75,  200, 261,
    200, 200, 261, 261, 75,  261, 75,  174, 75,  174, 174, 75,  75,  75,  75,  174, 174, 174, 174,
    174, 174, 174, 174, 174, 174, 261, 75,  174, 36,  123, 124, 126, 128, 129, 132, 36,  36,  36,
    36,  174, 36,  36,  36,  254, 146, 12,  9,   8,   174, 36,  36,  36,  36,  36,  174, 174, 146,
    36,  174, 36,  36,  36,  6,   252, 174, 174, 36,  174, 36,  174, 36,  12,  36,  36,  10,  10,
    174, 252, 174, 36,  174, 36,  36,  174, 174, 174, 174, 36,  36,  36,  36,  36,  36,  36,  36,
    36,  36,  252, 174, 36,  174, 174, 35,  35,  174, 174, 174, 174, 174, 174, 35,  174, 174, 12,
    174, 174, 35,  174, 174, 174, 174, 174, 12,  174, 174, 174, 174, 174, 174, 12,  174, 35,  36,
    36,  13,  248, 11,  250, 250, 35,  174, 256, 256, 36,  35,  174, 35,  254, 36,  35,  15,  253,
    12,  12,  35,  174, 35,  35,  174, 174, 174, 35,  174, 35,  174, 14,  249, 174, 20,  251, 251,
    36,  174, 35,  35,  36,  36,  36,  35,  36,  174, 17,  174, 174, 36,  36,  36,  36,  35,  35,
    35,  36,  36,  36,  174, 21,  174, 248, 249, 36,  36,  36,  36,  174, 18,  18,  36,  36,  36,
    36,  36,  174, 249, 36,  36,  174, 174, 36,  36,  36,  36,  36,  36,  36,  36,  36};

const short PipelineParserGen::yyr1_[] = {
    0,   121, 260, 260, 162, 163, 163, 262, 261, 164, 164, 164, 164, 164, 164, 170, 165, 166, 173,
    173, 173, 173, 167, 168, 169, 171, 171, 131, 131, 172, 172, 172, 172, 172, 172, 172, 172, 172,
    172, 172, 172, 172, 172, 172, 172, 172, 172, 172, 172, 172, 172, 172, 172, 172, 122, 122, 122,
    122, 257, 258, 258, 134, 259, 125, 125, 125, 128, 124, 124, 124, 124, 124, 124, 126, 126, 126,
    126, 126, 126, 126, 126, 126, 126, 126, 126, 126, 126, 126, 126, 127, 127, 127, 127, 127, 127,
    127, 127, 127, 127, 127, 127, 127, 127, 127, 127, 127, 127, 127, 127, 127, 127, 127, 127, 127,
    127, 127, 127, 127, 127, 127, 127, 127, 127, 127, 127, 127, 127, 127, 127, 127, 127, 127, 127,
    127, 127, 127, 127, 127, 127, 127, 127, 127, 127, 127, 127, 127, 127, 127, 127, 127, 147, 148,
    161, 149, 150, 151, 153, 154, 155, 135, 136, 137, 138, 140, 144, 145, 139, 139, 141, 141, 142,
    142, 143, 143, 152, 152, 156, 156, 156, 156, 156, 156, 156, 156, 156, 156, 156, 156, 156, 156,
    156, 156, 156, 156, 156, 156, 156, 254, 254, 174, 174, 176, 175, 175, 175, 175, 175, 175, 175,
    175, 177, 178, 179, 179, 132, 123, 123, 123, 123, 129, 180, 180, 180, 180, 180, 180, 180, 180,
    180, 180, 180, 180, 180, 180, 180, 180, 180, 181, 182, 233, 234, 235, 236, 237, 238, 239, 240,
    241, 242, 243, 244, 245, 246, 247, 183, 183, 183, 184, 185, 186, 190, 190, 190, 190, 190, 190,
    190, 190, 190, 190, 190, 190, 190, 190, 190, 190, 190, 190, 190, 190, 190, 190, 191, 250, 250,
    251, 251, 192, 193, 256, 256, 256, 194, 195, 252, 252, 196, 203, 213, 253, 253, 200, 197, 198,
    199, 201, 202, 204, 205, 206, 207, 208, 209, 210, 211, 212, 187, 187, 188, 189, 146, 146, 157,
    157, 158, 255, 255, 159, 160, 160, 133, 130, 130, 130, 130, 130, 214, 214, 214, 214, 214, 214,
    214, 215, 216, 217, 218, 219, 220, 221, 222, 222, 222, 222, 222, 222, 222, 222, 222, 222, 248,
    248, 249, 249, 223, 224, 225, 226, 227, 228, 229, 230, 231, 232};

const signed char PipelineParserGen::yyr2_[] = {
    0, 2, 2, 2, 3, 0, 4, 0, 2, 1, 1, 1, 1, 1, 1, 5,  3,  7, 1, 1, 1, 1, 2, 2, 4, 0, 2,  2,  2,
    1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,  1,  1, 1, 1, 1, 1, 1, 1, 1, 1, 1,  1,  1,
    3, 0, 2, 2, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,  1,  1, 1, 1, 1, 1, 1, 1, 1, 1, 1,  1,  1,
    1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,  1,  1, 1, 1, 1, 1, 1, 1, 1, 1, 1,  1,  1,
    1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,  1,  1, 1, 1, 1, 1, 1, 1, 1, 1, 1,  1,  1,
    1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,  1,  1, 1, 1, 1, 1, 1, 1, 1, 1, 1,  1,  1,
    1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,  1,  1, 1, 1, 1, 1, 1, 0, 2, 1, 1,  4,  1,
    1, 1, 1, 1, 1, 1, 1, 3, 3, 0, 2, 2, 1, 1, 1, 1,  1,  1, 1, 1, 1, 1, 1, 1, 1, 1, 1,  1,  1,
    1, 1, 1, 1, 1, 4, 4, 4, 4, 7, 4, 4, 4, 7, 4, 7,  8,  7, 7, 4, 7, 7, 1, 1, 1, 4, 4,  6,  1,
    1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,  1,  1, 1, 1, 1, 6, 0, 2, 0, 2, 11, 10, 0,
    1, 2, 8, 8, 0, 2, 8, 8, 8, 0, 2, 7, 4, 4, 4, 11, 11, 7, 4, 4, 7, 8, 8, 8, 4, 4, 1,  1,  6,
    6, 1, 1, 1, 1, 3, 0, 2, 3, 0, 2, 2, 1, 1, 1, 1,  1,  1, 1, 1, 1, 1, 1, 1, 4, 4, 4,  4,  4,
    4, 4, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 0, 2, 0, 2,  11, 4, 4, 4, 4, 4, 4, 4, 4, 4};


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
                                                   "\"$-prefixed string\"",
                                                   "\"$$-prefixed string\"",
                                                   "\"$-prefixed fieldname\"",
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
                                                   "start",
                                                   "START_ORDERED_OBJECT",
                                                   "$@1",
                                                   YY_NULLPTR};

#if YYDEBUG
const short PipelineParserGen::yyrline_[] = {
    0,    282,  282,  286,  294,  300,  301,  309,  309,  312,  312,  312,  312,  312,  312,  315,
    325,  331,  341,  341,  341,  341,  345,  350,  355,  371,  374,  381,  384,  390,  391,  392,
    393,  394,  395,  396,  397,  398,  399,  400,  401,  404,  407,  410,  413,  416,  419,  422,
    425,  428,  431,  432,  433,  434,  443,  443,  443,  443,  447,  453,  456,  462,  468,  473,
    473,  473,  477,  485,  488,  491,  494,  497,  500,  509,  512,  515,  518,  521,  524,  527,
    530,  533,  536,  539,  542,  545,  548,  551,  554,  562,  565,  568,  571,  574,  577,  580,
    583,  586,  589,  592,  595,  598,  601,  604,  607,  610,  613,  616,  619,  622,  625,  628,
    631,  634,  637,  640,  643,  646,  649,  652,  655,  658,  661,  664,  667,  670,  673,  676,
    679,  682,  685,  688,  691,  694,  697,  700,  703,  706,  709,  712,  715,  718,  721,  724,
    727,  730,  733,  736,  739,  742,  749,  754,  762,  771,  777,  783,  789,  795,  801,  807,
    813,  819,  825,  831,  837,  843,  849,  852,  858,  861,  867,  870,  876,  879,  885,  888,
    894,  895,  896,  897,  898,  899,  900,  901,  902,  903,  904,  905,  906,  907,  908,  909,
    910,  911,  912,  913,  914,  921,  922,  929,  929,  933,  938,  938,  938,  938,  938,  938,
    939,  939,  945,  953,  959,  962,  969,  976,  976,  976,  976,  980,  986,  986,  986,  986,
    986,  986,  986,  986,  986,  986,  986,  986,  986,  987,  987,  987,  987,  991,  998,  1004,
    1009, 1014, 1020, 1025, 1030, 1035, 1041, 1046, 1052, 1061, 1067, 1073, 1078, 1084, 1090, 1090,
    1090, 1094, 1101, 1108, 1115, 1115, 1115, 1115, 1115, 1115, 1115, 1116, 1116, 1116, 1116, 1116,
    1116, 1116, 1116, 1117, 1117, 1117, 1117, 1117, 1117, 1117, 1121, 1131, 1134, 1140, 1143, 1149,
    1158, 1167, 1170, 1173, 1179, 1190, 1201, 1204, 1210, 1218, 1226, 1234, 1237, 1242, 1251, 1257,
    1263, 1269, 1279, 1289, 1296, 1303, 1310, 1318, 1326, 1334, 1342, 1348, 1354, 1354, 1358, 1365,
    1372, 1372, 1376, 1376, 1380, 1386, 1387, 1394, 1400, 1403, 1410, 1417, 1418, 1419, 1420, 1421,
    1424, 1424, 1424, 1424, 1424, 1424, 1424, 1426, 1431, 1436, 1441, 1446, 1451, 1456, 1462, 1463,
    1464, 1465, 1466, 1467, 1468, 1469, 1470, 1471, 1476, 1479, 1486, 1489, 1495, 1505, 1510, 1515,
    1520, 1525, 1530, 1535, 1540, 1545};

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
#line 5457 "src/mongo/db/cst/pipeline_parser_gen.cpp"

#line 1549 "src/mongo/db/cst/pipeline_grammar.yy"
