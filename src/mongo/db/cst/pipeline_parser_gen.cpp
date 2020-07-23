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
        case 33:  // stageList
        case 34:  // stage
        case 35:  // inhibitOptimization
        case 36:  // unionWith
        case 37:  // num
        case 38:  // skip
        case 39:  // limit
        case 40:  // project
        case 41:  // projectFields
        case 42:  // projection
        case 43:  // compoundExpression
        case 44:  // expression
        case 45:  // maths
        case 46:  // add
        case 47:  // atan2
        case 48:  // string
        case 49:  // int
        case 50:  // long
        case 51:  // double
        case 52:  // bool
        case 53:  // value
        case 54:  // boolExps
        case 55:  // and
        case 56:  // or
        case 57:  // not
            value.YY_MOVE_OR_COPY<CNode>(YY_MOVE(that.value));
            break;

        case 58:  // projectionFieldname
            value.YY_MOVE_OR_COPY<CNode::Fieldname>(YY_MOVE(that.value));
            break;

        case 31:  // DECIMAL_NON_ZERO
            value.YY_MOVE_OR_COPY<Decimal128>(YY_MOVE(that.value));
            break;

        case 30:  // DOUBLE_NON_ZERO
            value.YY_MOVE_OR_COPY<double>(YY_MOVE(that.value));
            break;

        case 28:  // INT_NON_ZERO
            value.YY_MOVE_OR_COPY<int>(YY_MOVE(that.value));
            break;

        case 29:  // LONG_NON_ZERO
            value.YY_MOVE_OR_COPY<long long>(YY_MOVE(that.value));
            break;

        case 59:  // projectField
            value.YY_MOVE_OR_COPY<std::pair<CNode::Fieldname, CNode>>(YY_MOVE(that.value));
            break;

        case 26:  // FIELDNAME
        case 27:  // STRING
            value.YY_MOVE_OR_COPY<std::string>(YY_MOVE(that.value));
            break;

        case 60:  // expressions
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
        case 33:  // stageList
        case 34:  // stage
        case 35:  // inhibitOptimization
        case 36:  // unionWith
        case 37:  // num
        case 38:  // skip
        case 39:  // limit
        case 40:  // project
        case 41:  // projectFields
        case 42:  // projection
        case 43:  // compoundExpression
        case 44:  // expression
        case 45:  // maths
        case 46:  // add
        case 47:  // atan2
        case 48:  // string
        case 49:  // int
        case 50:  // long
        case 51:  // double
        case 52:  // bool
        case 53:  // value
        case 54:  // boolExps
        case 55:  // and
        case 56:  // or
        case 57:  // not
            value.move<CNode>(YY_MOVE(that.value));
            break;

        case 58:  // projectionFieldname
            value.move<CNode::Fieldname>(YY_MOVE(that.value));
            break;

        case 31:  // DECIMAL_NON_ZERO
            value.move<Decimal128>(YY_MOVE(that.value));
            break;

        case 30:  // DOUBLE_NON_ZERO
            value.move<double>(YY_MOVE(that.value));
            break;

        case 28:  // INT_NON_ZERO
            value.move<int>(YY_MOVE(that.value));
            break;

        case 29:  // LONG_NON_ZERO
            value.move<long long>(YY_MOVE(that.value));
            break;

        case 59:  // projectField
            value.move<std::pair<CNode::Fieldname, CNode>>(YY_MOVE(that.value));
            break;

        case 26:  // FIELDNAME
        case 27:  // STRING
            value.move<std::string>(YY_MOVE(that.value));
            break;

        case 60:  // expressions
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
        case 33:  // stageList
        case 34:  // stage
        case 35:  // inhibitOptimization
        case 36:  // unionWith
        case 37:  // num
        case 38:  // skip
        case 39:  // limit
        case 40:  // project
        case 41:  // projectFields
        case 42:  // projection
        case 43:  // compoundExpression
        case 44:  // expression
        case 45:  // maths
        case 46:  // add
        case 47:  // atan2
        case 48:  // string
        case 49:  // int
        case 50:  // long
        case 51:  // double
        case 52:  // bool
        case 53:  // value
        case 54:  // boolExps
        case 55:  // and
        case 56:  // or
        case 57:  // not
            value.copy<CNode>(that.value);
            break;

        case 58:  // projectionFieldname
            value.copy<CNode::Fieldname>(that.value);
            break;

        case 31:  // DECIMAL_NON_ZERO
            value.copy<Decimal128>(that.value);
            break;

        case 30:  // DOUBLE_NON_ZERO
            value.copy<double>(that.value);
            break;

        case 28:  // INT_NON_ZERO
            value.copy<int>(that.value);
            break;

        case 29:  // LONG_NON_ZERO
            value.copy<long long>(that.value);
            break;

        case 59:  // projectField
            value.copy<std::pair<CNode::Fieldname, CNode>>(that.value);
            break;

        case 26:  // FIELDNAME
        case 27:  // STRING
            value.copy<std::string>(that.value);
            break;

        case 60:  // expressions
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
        case 33:  // stageList
        case 34:  // stage
        case 35:  // inhibitOptimization
        case 36:  // unionWith
        case 37:  // num
        case 38:  // skip
        case 39:  // limit
        case 40:  // project
        case 41:  // projectFields
        case 42:  // projection
        case 43:  // compoundExpression
        case 44:  // expression
        case 45:  // maths
        case 46:  // add
        case 47:  // atan2
        case 48:  // string
        case 49:  // int
        case 50:  // long
        case 51:  // double
        case 52:  // bool
        case 53:  // value
        case 54:  // boolExps
        case 55:  // and
        case 56:  // or
        case 57:  // not
            value.move<CNode>(that.value);
            break;

        case 58:  // projectionFieldname
            value.move<CNode::Fieldname>(that.value);
            break;

        case 31:  // DECIMAL_NON_ZERO
            value.move<Decimal128>(that.value);
            break;

        case 30:  // DOUBLE_NON_ZERO
            value.move<double>(that.value);
            break;

        case 28:  // INT_NON_ZERO
            value.move<int>(that.value);
            break;

        case 29:  // LONG_NON_ZERO
            value.move<long long>(that.value);
            break;

        case 59:  // projectField
            value.move<std::pair<CNode::Fieldname, CNode>>(that.value);
            break;

        case 26:  // FIELDNAME
        case 27:  // STRING
            value.move<std::string>(that.value);
            break;

        case 60:  // expressions
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
                case 33:  // stageList
                case 34:  // stage
                case 35:  // inhibitOptimization
                case 36:  // unionWith
                case 37:  // num
                case 38:  // skip
                case 39:  // limit
                case 40:  // project
                case 41:  // projectFields
                case 42:  // projection
                case 43:  // compoundExpression
                case 44:  // expression
                case 45:  // maths
                case 46:  // add
                case 47:  // atan2
                case 48:  // string
                case 49:  // int
                case 50:  // long
                case 51:  // double
                case 52:  // bool
                case 53:  // value
                case 54:  // boolExps
                case 55:  // and
                case 56:  // or
                case 57:  // not
                    yylhs.value.emplace<CNode>();
                    break;

                case 58:  // projectionFieldname
                    yylhs.value.emplace<CNode::Fieldname>();
                    break;

                case 31:  // DECIMAL_NON_ZERO
                    yylhs.value.emplace<Decimal128>();
                    break;

                case 30:  // DOUBLE_NON_ZERO
                    yylhs.value.emplace<double>();
                    break;

                case 28:  // INT_NON_ZERO
                    yylhs.value.emplace<int>();
                    break;

                case 29:  // LONG_NON_ZERO
                    yylhs.value.emplace<long long>();
                    break;

                case 59:  // projectField
                    yylhs.value.emplace<std::pair<CNode::Fieldname, CNode>>();
                    break;

                case 26:  // FIELDNAME
                case 27:  // STRING
                    yylhs.value.emplace<std::string>();
                    break;

                case 60:  // expressions
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
#line 171 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        *cst = YY_MOVE(yystack_[1].value.as<CNode>());
                    }
#line 867 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 3:
#line 177 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                    }
#line 873 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 4:
#line 178 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() =
                            CNode{CNode::ArrayChildren{YY_MOVE(yystack_[2].value.as<CNode>())}};
                    }
#line 881 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 5:
#line 186 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        lexer.sortObjTokens();
                    }
#line 887 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 7:
#line 189 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 893 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 8:
#line 189 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 899 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 9:
#line 189 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 905 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 10:
#line 189 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 911 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 11:
#line 189 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 917 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 12:
#line 193 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            std::pair{KeyFieldname::inhibitOptimization, CNode::noopLeaf()}}};
                    }
#line 925 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 13:
#line 199 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        auto pipeline = YY_MOVE(yystack_[1].value.as<CNode>());
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{std::pair{
                            KeyFieldname::unionWith,
                            CNode{CNode::ObjectChildren{
                                {KeyFieldname::collArg, YY_MOVE(yystack_[3].value.as<CNode>())},
                                {KeyFieldname::pipelineArg, std::move(pipeline)}}}}}};
                    }
#line 938 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 14:
#line 209 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 944 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 15:
#line 209 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 950 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 16:
#line 209 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 956 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 17:
#line 213 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            std::pair{KeyFieldname::skip, YY_MOVE(yystack_[0].value.as<CNode>())}}};
                    }
#line 964 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 18:
#line 218 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{std::pair{
                            KeyFieldname::limit, YY_MOVE(yystack_[0].value.as<CNode>())}}};
                    }
#line 972 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 19:
#line 223 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{std::pair{
                            KeyFieldname::project, YY_MOVE(yystack_[1].value.as<CNode>())}}};
                    }
#line 980 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 20:
#line 229 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode::noopLeaf();
                    }
#line 988 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 21:
#line 232 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[1].value.as<CNode>());
                        yylhs.value.as<CNode>().objectChildren().emplace_back(
                            YY_MOVE(yystack_[0].value.as<std::pair<CNode::Fieldname, CNode>>()));
                    }
#line 997 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 22:
#line 239 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<std::pair<CNode::Fieldname, CNode>>() = {
                            KeyFieldname::id, YY_MOVE(yystack_[0].value.as<CNode>())};
                    }
#line 1005 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 23:
#line 242 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<std::pair<CNode::Fieldname, CNode>>() = {
                            YY_MOVE(yystack_[1].value.as<CNode::Fieldname>()),
                            YY_MOVE(yystack_[0].value.as<CNode>())};
                    }
#line 1013 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 24:
#line 248 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 1019 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 25:
#line 249 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() =
                            CNode{NonZeroKey{YY_MOVE(yystack_[0].value.as<int>())}};
                    }
#line 1027 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 26:
#line 252 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{KeyValue::intZeroKey};
                    }
#line 1035 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 27:
#line 255 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() =
                            CNode{NonZeroKey{YY_MOVE(yystack_[0].value.as<long long>())}};
                    }
#line 1043 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 28:
#line 258 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{KeyValue::longZeroKey};
                    }
#line 1051 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 29:
#line 261 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() =
                            CNode{NonZeroKey{YY_MOVE(yystack_[0].value.as<double>())}};
                    }
#line 1059 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 30:
#line 264 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{KeyValue::doubleZeroKey};
                    }
#line 1067 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 31:
#line 267 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() =
                            CNode{NonZeroKey{YY_MOVE(yystack_[0].value.as<Decimal128>())}};
                    }
#line 1075 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 32:
#line 270 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{KeyValue::decimalZeroKey};
                    }
#line 1083 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 33:
#line 273 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{KeyValue::trueKey};
                    }
#line 1091 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 34:
#line 276 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{KeyValue::falseKey};
                    }
#line 1099 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 35:
#line 279 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 1105 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 36:
#line 283 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() =
                            UserFieldname{YY_MOVE(yystack_[0].value.as<std::string>())};
                    }
#line 1113 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 37:
#line 288 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() =
                            UserFieldname{"$_internalInhibitOptimization"};
                    }
#line 1121 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 38:
#line 291 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$unionWith"};
                    }
#line 1129 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 39:
#line 294 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$project"};
                    }
#line 1137 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 40:
#line 297 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"coll"};
                    }
#line 1145 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 41:
#line 300 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"pipeline"};
                    }
#line 1153 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 42:
#line 303 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$add"};
                    }
#line 1161 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 43:
#line 306 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$atan2"};
                    }
#line 1169 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 44:
#line 309 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$and"};
                    }
#line 1177 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 45:
#line 312 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$or"};
                    }
#line 1185 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 46:
#line 315 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$not"};
                    }
#line 1193 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 47:
#line 321 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() =
                            CNode{UserString{YY_MOVE(yystack_[0].value.as<std::string>())}};
                    }
#line 1201 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 48:
#line 327 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() =
                            CNode{UserInt{YY_MOVE(yystack_[0].value.as<int>())}};
                    }
#line 1209 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 49:
#line 330 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{UserLong{0}};
                    }
#line 1217 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 50:
#line 336 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() =
                            CNode{UserLong{YY_MOVE(yystack_[0].value.as<long long>())}};
                    }
#line 1225 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 51:
#line 339 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{UserLong{0ll}};
                    }
#line 1233 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 52:
#line 345 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() =
                            CNode{UserDouble{YY_MOVE(yystack_[0].value.as<double>())}};
                    }
#line 1241 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 53:
#line 348 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{UserDouble{0.0}};
                    }
#line 1249 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 54:
#line 354 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{UserBoolean{true}};
                    }
#line 1257 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 55:
#line 357 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{UserBoolean{false}};
                    }
#line 1265 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 56:
#line 363 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 1271 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 57:
#line 364 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 1277 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 58:
#line 365 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 1283 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 59:
#line 366 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 1289 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 60:
#line 367 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 1295 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 61:
#line 374 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                    }
#line 1301 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 62:
#line 375 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<std::vector<CNode>>() =
                            YY_MOVE(yystack_[0].value.as<std::vector<CNode>>());
                        yylhs.value.as<std::vector<CNode>>().emplace_back(
                            YY_MOVE(yystack_[1].value.as<CNode>()));
                    }
#line 1310 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 63:
#line 381 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 1316 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 64:
#line 382 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 1322 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 65:
#line 386 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 1328 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 66:
#line 386 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 1334 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 67:
#line 390 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 1340 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 68:
#line 391 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 1346 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 69:
#line 394 "src/mongo/db/cst/pipeline_grammar.yy"
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
#line 1358 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 70:
#line 404 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::atan2,
                             CNode{CNode::ArrayChildren{YY_MOVE(yystack_[3].value.as<CNode>()),
                                                        YY_MOVE(yystack_[2].value.as<CNode>())}}}}};
                    }
#line 1367 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 71:
#line 411 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 1373 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 72:
#line 411 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 1379 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 73:
#line 411 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 1385 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 74:
#line 415 "src/mongo/db/cst/pipeline_grammar.yy"
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
#line 1397 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 75:
#line 425 "src/mongo/db/cst/pipeline_grammar.yy"
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
#line 1409 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 76:
#line 435 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::notExpr,
                             CNode{CNode::ArrayChildren{YY_MOVE(yystack_[2].value.as<CNode>())}}}}};
                    }
#line 1418 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;


#line 1422 "src/mongo/db/cst/pipeline_parser_gen.cpp"

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
        if (!yyla.empty())
            yy_destroy_("Cleanup: discarding lookahead", yyla);

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


const signed char PipelineParserGen::yypact_ninf_ = -66;

const signed char PipelineParserGen::yytable_ninf_ = -1;

const signed char PipelineParserGen::yypact_[] = {
    -2,  3,   21,  61,  19,  -66, 30,  -66, 6,   6,   44,  50,  -66, -66, -66, -66, -66, -66,
    60,  52,  69,  -66, -66, -66, -66, -66, -66, -66, -66, -66, -66, -66, -66, 3,   -66, 46,
    -66, 37,  -66, -66, 70,  -66, -1,  -66, -66, -66, -66, -66, -66, -66, -66, -66, -66, -66,
    -1,  -66, -6,  63,  -66, -66, -66, -66, -66, -66, -66, -66, -66, -66, -66, -66, -66, -66,
    -66, -66, -66, -66, -66, -66, -66, 76,  84,  86,  87,  88,  89,  -66, 10,  10,  10,  10,
    10,  -66, -66, -66, 10,  -66, -66, -66, -66, -66, -66, 10,  10,  10,  90,  10,  91,  10,
    10,  94,  10,  93,  96,  95,  97,  -66, -66, 98,  -66, 100, 101, -66, -66, -66};

const signed char PipelineParserGen::yydefact_[] = {
    0,  3,  0,  0,  0,  1,  0,  5,  0,  0,  0,  0,  7,  8,  9,  10, 11, 2,  0,  0,  0,
    49, 51, 53, 48, 50, 52, 17, 14, 15, 16, 18, 20, 3,  12, 0,  6,  0,  4,  47, 0,  19,
    0,  37, 38, 39, 40, 41, 42, 43, 44, 45, 46, 36, 0,  21, 0,  0,  26, 28, 30, 32, 33,
    34, 25, 27, 29, 31, 22, 35, 65, 67, 68, 24, 66, 71, 72, 73, 23, 0,  0,  0,  0,  0,
    0,  13, 0,  0,  0,  0,  0,  54, 55, 64, 0,  56, 57, 58, 59, 60, 63, 0,  0,  0,  0,
    61, 0,  61, 61, 0,  61, 0,  0,  0,  0,  76, 62, 0,  70, 0,  0,  69, 74, 75};

const signed char PipelineParserGen::yypgoto_[] = {
    -66, 62, -66, -66, -66, 99,  -66, -66, -66, -66, 53,  -37, -20, -66, -66, -66,
    11,  23, 41,  -8,  -66, -66, -66, -66, -66, -66, -66, -66, -65, -66, -66, -66};

const signed char PipelineParserGen::yydefgoto_[] = {-1, 4,   11, 12, 13, 27, 14,  15, 16, 37, 68,
                                                     93, 110, 70, 71, 72, 95, 96,  97, 98, 99, 100,
                                                     74, 75,  76, 77, 54, 55, 111, 2,  19, 20};

const signed char PipelineParserGen::yytable_[] = {
    30, 30,  57,  1,   23,  69,  3,   58,  59,  60,  61,  62,  63,  57,  21, 22, 23, 69,  21,
    22, 23,  5,   91,  92,  26,  17,  39,  64,  65,  66,  67,  28,  28,  18, 24, 25, 26,  39,
    24, 25,  26,  41,  113, 114, 42,  116, 40,  32,  79,  29,  29,  43,  44, 73, 33, 45,  46,
    47, 48,  49,  50,  51,  52,  53,  34,  73,  94,  101, 102, 103, 104, 35, 36, 39, 105, 6,
    7,  8,   9,   10,  85,  106, 107, 108, 80,  81,  82,  83,  84,  86,  56, 87, 88, 89,  90,
    38, 109, 112, 115, 117, 118, 119, 121, 120, 122, 123, 0,   78,  31};

const signed char PipelineParserGen::yycheck_[] = {
    8,  9,   3,  5,  10, 42, 3,  8,  9,  10, 11, 12, 13, 3,  8,  9,   10,  54,  8,  9,  10,  0,
    12, 13,  30, 6,  27, 28, 29, 30, 31, 8,  9,  3,  28, 29, 30, 27,  28,  29,  30, 4,  107, 108,
    7,  110, 35, 3,  56, 8,  9,  14, 15, 42, 4,  18, 19, 20, 21, 22,  23,  24,  25, 26, 4,   54,
    86, 87,  88, 89, 90, 19, 3,  27, 94, 14, 15, 16, 17, 18, 4,  101, 102, 103, 21, 22, 23,  24,
    25, 5,   20, 5,  5,  5,  5,  33, 6,  6,  4,  6,  4,  6,  4,  6,   4,   4,   -1, 54, 9};

const signed char PipelineParserGen::yystos_[] = {
    0,  5,  61, 3,  33, 0,  14, 15, 16, 17, 18, 34, 35, 36, 38, 39, 40, 6,  3,  62, 63,
    8,  9,  10, 28, 29, 30, 37, 49, 50, 51, 37, 3,  4,  4,  19, 3,  41, 33, 27, 48, 4,
    7,  14, 15, 18, 19, 20, 21, 22, 23, 24, 25, 26, 58, 59, 20, 3,  8,  9,  10, 11, 12,
    13, 28, 29, 30, 31, 42, 43, 45, 46, 47, 48, 54, 55, 56, 57, 42, 51, 21, 22, 23, 24,
    25, 4,  5,  5,  5,  5,  5,  12, 13, 43, 44, 48, 49, 50, 51, 52, 53, 44, 44, 44, 44,
    44, 44, 44, 44, 6,  44, 60, 6,  60, 60, 4,  60, 6,  4,  6,  6,  4,  4,  4};

const signed char PipelineParserGen::yyr1_[] = {
    0,  32, 61, 33, 33, 63, 62, 34, 34, 34, 34, 34, 35, 36, 37, 37, 37, 38, 39, 40,
    41, 41, 59, 59, 42, 42, 42, 42, 42, 42, 42, 42, 42, 42, 42, 42, 58, 58, 58, 58,
    58, 58, 58, 58, 58, 58, 58, 48, 49, 49, 50, 50, 51, 51, 52, 52, 53, 53, 53, 53,
    53, 60, 60, 44, 44, 43, 43, 45, 45, 46, 47, 54, 54, 54, 55, 56, 57};

const signed char PipelineParserGen::yyr2_[] = {
    0, 2, 3, 0, 4, 0, 2, 1, 1, 1, 1, 1, 3, 7, 1, 1, 1, 2, 2, 4, 0, 2, 2, 2, 1, 1,
    1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,
    1, 1, 1, 1, 1, 1, 1, 1, 1, 0, 2, 1, 1, 1, 1, 1, 1, 8, 7, 1, 1, 1, 8, 8, 6};


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
                                                   "TRUE",
                                                   "FALSE",
                                                   "STAGE_INHIBIT_OPTIMIZATION",
                                                   "STAGE_UNION_WITH",
                                                   "STAGE_SKIP",
                                                   "STAGE_LIMIT",
                                                   "STAGE_PROJECT",
                                                   "COLL_ARG",
                                                   "PIPELINE_ARG",
                                                   "ADD",
                                                   "ATAN2",
                                                   "AND",
                                                   "OR",
                                                   "NOT",
                                                   "FIELDNAME",
                                                   "STRING",
                                                   "INT_NON_ZERO",
                                                   "LONG_NON_ZERO",
                                                   "DOUBLE_NON_ZERO",
                                                   "DECIMAL_NON_ZERO",
                                                   "$accept",
                                                   "stageList",
                                                   "stage",
                                                   "inhibitOptimization",
                                                   "unionWith",
                                                   "num",
                                                   "skip",
                                                   "limit",
                                                   "project",
                                                   "projectFields",
                                                   "projection",
                                                   "compoundExpression",
                                                   "expression",
                                                   "maths",
                                                   "add",
                                                   "atan2",
                                                   "string",
                                                   "int",
                                                   "long",
                                                   "double",
                                                   "bool",
                                                   "value",
                                                   "boolExps",
                                                   "and",
                                                   "or",
                                                   "not",
                                                   "projectionFieldname",
                                                   "projectField",
                                                   "expressions",
                                                   "pipeline",
                                                   "START_ORDERED_OBJECT",
                                                   "$@1",
                                                   YY_NULLPTR};
#endif


#if YYDEBUG
const short PipelineParserGen::yyrline_[] = {
    0,   171, 171, 177, 178, 186, 186, 189, 189, 189, 189, 189, 193, 199, 209, 209,
    209, 213, 218, 223, 229, 232, 239, 242, 248, 249, 252, 255, 258, 261, 264, 267,
    270, 273, 276, 279, 283, 288, 291, 294, 297, 300, 303, 306, 309, 312, 315, 321,
    327, 330, 336, 339, 345, 348, 354, 357, 363, 364, 365, 366, 367, 374, 375, 381,
    382, 386, 386, 390, 391, 394, 404, 411, 411, 411, 415, 425, 435};

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
#line 1801 "src/mongo/db/cst/pipeline_parser_gen.cpp"

#line 441 "src/mongo/db/cst/pipeline_grammar.yy"
