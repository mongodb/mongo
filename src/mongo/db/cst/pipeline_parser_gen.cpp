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
        case 30:  // stageList
        case 31:  // stage
        case 32:  // inhibitOptimization
        case 33:  // unionWith
        case 34:  // num
        case 35:  // skip
        case 36:  // limit
        case 37:  // project
        case 38:  // projectFields
        case 39:  // projection
        case 40:  // compoundExpression
        case 41:  // expression
        case 42:  // maths
        case 43:  // add
        case 44:  // atan2
        case 45:  // string
        case 46:  // int
        case 47:  // long
        case 48:  // double
        case 49:  // bool
        case 50:  // value
            value.YY_MOVE_OR_COPY<CNode>(YY_MOVE(that.value));
            break;

        case 51:  // projectionFieldname
            value.YY_MOVE_OR_COPY<CNode::Fieldname>(YY_MOVE(that.value));
            break;

        case 28:  // DECIMAL_NON_ZERO
            value.YY_MOVE_OR_COPY<Decimal128>(YY_MOVE(that.value));
            break;

        case 27:  // DOUBLE_NON_ZERO
            value.YY_MOVE_OR_COPY<double>(YY_MOVE(that.value));
            break;

        case 25:  // INT_NON_ZERO
            value.YY_MOVE_OR_COPY<int>(YY_MOVE(that.value));
            break;

        case 26:  // LONG_NON_ZERO
            value.YY_MOVE_OR_COPY<long long>(YY_MOVE(that.value));
            break;

        case 52:  // projectField
            value.YY_MOVE_OR_COPY<std::pair<CNode::Fieldname, CNode>>(YY_MOVE(that.value));
            break;

        case 23:  // FIELDNAME
        case 24:  // STRING
            value.YY_MOVE_OR_COPY<std::string>(YY_MOVE(that.value));
            break;

        case 53:  // expressions
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
        case 30:  // stageList
        case 31:  // stage
        case 32:  // inhibitOptimization
        case 33:  // unionWith
        case 34:  // num
        case 35:  // skip
        case 36:  // limit
        case 37:  // project
        case 38:  // projectFields
        case 39:  // projection
        case 40:  // compoundExpression
        case 41:  // expression
        case 42:  // maths
        case 43:  // add
        case 44:  // atan2
        case 45:  // string
        case 46:  // int
        case 47:  // long
        case 48:  // double
        case 49:  // bool
        case 50:  // value
            value.move<CNode>(YY_MOVE(that.value));
            break;

        case 51:  // projectionFieldname
            value.move<CNode::Fieldname>(YY_MOVE(that.value));
            break;

        case 28:  // DECIMAL_NON_ZERO
            value.move<Decimal128>(YY_MOVE(that.value));
            break;

        case 27:  // DOUBLE_NON_ZERO
            value.move<double>(YY_MOVE(that.value));
            break;

        case 25:  // INT_NON_ZERO
            value.move<int>(YY_MOVE(that.value));
            break;

        case 26:  // LONG_NON_ZERO
            value.move<long long>(YY_MOVE(that.value));
            break;

        case 52:  // projectField
            value.move<std::pair<CNode::Fieldname, CNode>>(YY_MOVE(that.value));
            break;

        case 23:  // FIELDNAME
        case 24:  // STRING
            value.move<std::string>(YY_MOVE(that.value));
            break;

        case 53:  // expressions
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
        case 30:  // stageList
        case 31:  // stage
        case 32:  // inhibitOptimization
        case 33:  // unionWith
        case 34:  // num
        case 35:  // skip
        case 36:  // limit
        case 37:  // project
        case 38:  // projectFields
        case 39:  // projection
        case 40:  // compoundExpression
        case 41:  // expression
        case 42:  // maths
        case 43:  // add
        case 44:  // atan2
        case 45:  // string
        case 46:  // int
        case 47:  // long
        case 48:  // double
        case 49:  // bool
        case 50:  // value
            value.copy<CNode>(that.value);
            break;

        case 51:  // projectionFieldname
            value.copy<CNode::Fieldname>(that.value);
            break;

        case 28:  // DECIMAL_NON_ZERO
            value.copy<Decimal128>(that.value);
            break;

        case 27:  // DOUBLE_NON_ZERO
            value.copy<double>(that.value);
            break;

        case 25:  // INT_NON_ZERO
            value.copy<int>(that.value);
            break;

        case 26:  // LONG_NON_ZERO
            value.copy<long long>(that.value);
            break;

        case 52:  // projectField
            value.copy<std::pair<CNode::Fieldname, CNode>>(that.value);
            break;

        case 23:  // FIELDNAME
        case 24:  // STRING
            value.copy<std::string>(that.value);
            break;

        case 53:  // expressions
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
        case 30:  // stageList
        case 31:  // stage
        case 32:  // inhibitOptimization
        case 33:  // unionWith
        case 34:  // num
        case 35:  // skip
        case 36:  // limit
        case 37:  // project
        case 38:  // projectFields
        case 39:  // projection
        case 40:  // compoundExpression
        case 41:  // expression
        case 42:  // maths
        case 43:  // add
        case 44:  // atan2
        case 45:  // string
        case 46:  // int
        case 47:  // long
        case 48:  // double
        case 49:  // bool
        case 50:  // value
            value.move<CNode>(that.value);
            break;

        case 51:  // projectionFieldname
            value.move<CNode::Fieldname>(that.value);
            break;

        case 28:  // DECIMAL_NON_ZERO
            value.move<Decimal128>(that.value);
            break;

        case 27:  // DOUBLE_NON_ZERO
            value.move<double>(that.value);
            break;

        case 25:  // INT_NON_ZERO
            value.move<int>(that.value);
            break;

        case 26:  // LONG_NON_ZERO
            value.move<long long>(that.value);
            break;

        case 52:  // projectField
            value.move<std::pair<CNode::Fieldname, CNode>>(that.value);
            break;

        case 23:  // FIELDNAME
        case 24:  // STRING
            value.move<std::string>(that.value);
            break;

        case 53:  // expressions
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
                case 30:  // stageList
                case 31:  // stage
                case 32:  // inhibitOptimization
                case 33:  // unionWith
                case 34:  // num
                case 35:  // skip
                case 36:  // limit
                case 37:  // project
                case 38:  // projectFields
                case 39:  // projection
                case 40:  // compoundExpression
                case 41:  // expression
                case 42:  // maths
                case 43:  // add
                case 44:  // atan2
                case 45:  // string
                case 46:  // int
                case 47:  // long
                case 48:  // double
                case 49:  // bool
                case 50:  // value
                    yylhs.value.emplace<CNode>();
                    break;

                case 51:  // projectionFieldname
                    yylhs.value.emplace<CNode::Fieldname>();
                    break;

                case 28:  // DECIMAL_NON_ZERO
                    yylhs.value.emplace<Decimal128>();
                    break;

                case 27:  // DOUBLE_NON_ZERO
                    yylhs.value.emplace<double>();
                    break;

                case 25:  // INT_NON_ZERO
                    yylhs.value.emplace<int>();
                    break;

                case 26:  // LONG_NON_ZERO
                    yylhs.value.emplace<long long>();
                    break;

                case 52:  // projectField
                    yylhs.value.emplace<std::pair<CNode::Fieldname, CNode>>();
                    break;

                case 23:  // FIELDNAME
                case 24:  // STRING
                    yylhs.value.emplace<std::string>();
                    break;

                case 53:  // expressions
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
#line 167 "pipeline_grammar.yy"
                    {
                        *cst = YY_MOVE(yystack_[1].value.as<CNode>());
                    }
#line 838 "pipeline_parser_gen.cpp"
                    break;

                    case 3:
#line 173 "pipeline_grammar.yy"
                    {
                    }
#line 844 "pipeline_parser_gen.cpp"
                    break;

                    case 4:
#line 174 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() =
                            CNode{CNode::ArrayChildren{YY_MOVE(yystack_[2].value.as<CNode>())}};
                    }
#line 852 "pipeline_parser_gen.cpp"
                    break;

                    case 5:
#line 182 "pipeline_grammar.yy"
                    {
                        lexer.sortObjTokens();
                    }
#line 858 "pipeline_parser_gen.cpp"
                    break;

                    case 7:
#line 185 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 864 "pipeline_parser_gen.cpp"
                    break;

                    case 8:
#line 185 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 870 "pipeline_parser_gen.cpp"
                    break;

                    case 9:
#line 185 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 876 "pipeline_parser_gen.cpp"
                    break;

                    case 10:
#line 185 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 882 "pipeline_parser_gen.cpp"
                    break;

                    case 11:
#line 185 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 888 "pipeline_parser_gen.cpp"
                    break;

                    case 12:
#line 189 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            std::pair{KeyFieldname::inhibitOptimization, CNode::noopLeaf()}}};
                    }
#line 896 "pipeline_parser_gen.cpp"
                    break;

                    case 13:
#line 195 "pipeline_grammar.yy"
                    {
                        auto pipeline = YY_MOVE(yystack_[1].value.as<CNode>());
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{std::pair{
                            KeyFieldname::unionWith,
                            CNode{CNode::ObjectChildren{
                                {KeyFieldname::collArg, YY_MOVE(yystack_[3].value.as<CNode>())},
                                {KeyFieldname::pipelineArg, std::move(pipeline)}}}}}};
                    }
#line 909 "pipeline_parser_gen.cpp"
                    break;

                    case 14:
#line 205 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 915 "pipeline_parser_gen.cpp"
                    break;

                    case 15:
#line 205 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 921 "pipeline_parser_gen.cpp"
                    break;

                    case 16:
#line 205 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 927 "pipeline_parser_gen.cpp"
                    break;

                    case 17:
#line 209 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            std::pair{KeyFieldname::skip, YY_MOVE(yystack_[0].value.as<CNode>())}}};
                    }
#line 935 "pipeline_parser_gen.cpp"
                    break;

                    case 18:
#line 214 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{std::pair{
                            KeyFieldname::limit, YY_MOVE(yystack_[0].value.as<CNode>())}}};
                    }
#line 943 "pipeline_parser_gen.cpp"
                    break;

                    case 19:
#line 219 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{std::pair{
                            KeyFieldname::project, YY_MOVE(yystack_[1].value.as<CNode>())}}};
                    }
#line 951 "pipeline_parser_gen.cpp"
                    break;

                    case 20:
#line 225 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode::noopLeaf();
                    }
#line 959 "pipeline_parser_gen.cpp"
                    break;

                    case 21:
#line 228 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[1].value.as<CNode>());
                        yylhs.value.as<CNode>().objectChildren().emplace_back(
                            YY_MOVE(yystack_[0].value.as<std::pair<CNode::Fieldname, CNode>>()));
                    }
#line 968 "pipeline_parser_gen.cpp"
                    break;

                    case 22:
#line 235 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<std::pair<CNode::Fieldname, CNode>>() = {
                            KeyFieldname::id, YY_MOVE(yystack_[0].value.as<CNode>())};
                    }
#line 976 "pipeline_parser_gen.cpp"
                    break;

                    case 23:
#line 238 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<std::pair<CNode::Fieldname, CNode>>() = {
                            YY_MOVE(yystack_[1].value.as<CNode::Fieldname>()),
                            YY_MOVE(yystack_[0].value.as<CNode>())};
                    }
#line 984 "pipeline_parser_gen.cpp"
                    break;

                    case 24:
#line 244 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 990 "pipeline_parser_gen.cpp"
                    break;

                    case 25:
#line 245 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() =
                            CNode{NonZeroKey{YY_MOVE(yystack_[0].value.as<int>())}};
                    }
#line 998 "pipeline_parser_gen.cpp"
                    break;

                    case 26:
#line 248 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{KeyValue::intZeroKey};
                    }
#line 1006 "pipeline_parser_gen.cpp"
                    break;

                    case 27:
#line 251 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() =
                            CNode{NonZeroKey{YY_MOVE(yystack_[0].value.as<long long>())}};
                    }
#line 1014 "pipeline_parser_gen.cpp"
                    break;

                    case 28:
#line 254 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{KeyValue::longZeroKey};
                    }
#line 1022 "pipeline_parser_gen.cpp"
                    break;

                    case 29:
#line 257 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() =
                            CNode{NonZeroKey{YY_MOVE(yystack_[0].value.as<double>())}};
                    }
#line 1030 "pipeline_parser_gen.cpp"
                    break;

                    case 30:
#line 260 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{KeyValue::doubleZeroKey};
                    }
#line 1038 "pipeline_parser_gen.cpp"
                    break;

                    case 31:
#line 263 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() =
                            CNode{NonZeroKey{YY_MOVE(yystack_[0].value.as<Decimal128>())}};
                    }
#line 1046 "pipeline_parser_gen.cpp"
                    break;

                    case 32:
#line 266 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{KeyValue::decimalZeroKey};
                    }
#line 1054 "pipeline_parser_gen.cpp"
                    break;

                    case 33:
#line 269 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{KeyValue::trueKey};
                    }
#line 1062 "pipeline_parser_gen.cpp"
                    break;

                    case 34:
#line 272 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{KeyValue::falseKey};
                    }
#line 1070 "pipeline_parser_gen.cpp"
                    break;

                    case 35:
#line 275 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 1076 "pipeline_parser_gen.cpp"
                    break;

                    case 36:
#line 279 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() =
                            UserFieldname{YY_MOVE(yystack_[0].value.as<std::string>())};
                    }
#line 1084 "pipeline_parser_gen.cpp"
                    break;

                    case 37:
#line 284 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() =
                            UserFieldname{"$_internalInhibitOptimization"};
                    }
#line 1092 "pipeline_parser_gen.cpp"
                    break;

                    case 38:
#line 287 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$unionWith"};
                    }
#line 1100 "pipeline_parser_gen.cpp"
                    break;

                    case 39:
#line 290 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$project"};
                    }
#line 1108 "pipeline_parser_gen.cpp"
                    break;

                    case 40:
#line 293 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"coll"};
                    }
#line 1116 "pipeline_parser_gen.cpp"
                    break;

                    case 41:
#line 296 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"pipeline"};
                    }
#line 1124 "pipeline_parser_gen.cpp"
                    break;

                    case 42:
#line 299 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$add"};
                    }
#line 1132 "pipeline_parser_gen.cpp"
                    break;

                    case 43:
#line 302 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$atan2"};
                    }
#line 1140 "pipeline_parser_gen.cpp"
                    break;

                    case 44:
#line 308 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() =
                            CNode{UserString{YY_MOVE(yystack_[0].value.as<std::string>())}};
                    }
#line 1148 "pipeline_parser_gen.cpp"
                    break;

                    case 45:
#line 314 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() =
                            CNode{UserInt{YY_MOVE(yystack_[0].value.as<int>())}};
                    }
#line 1156 "pipeline_parser_gen.cpp"
                    break;

                    case 46:
#line 317 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{UserLong{0}};
                    }
#line 1164 "pipeline_parser_gen.cpp"
                    break;

                    case 47:
#line 323 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() =
                            CNode{UserLong{YY_MOVE(yystack_[0].value.as<long long>())}};
                    }
#line 1172 "pipeline_parser_gen.cpp"
                    break;

                    case 48:
#line 326 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{UserLong{0ll}};
                    }
#line 1180 "pipeline_parser_gen.cpp"
                    break;

                    case 49:
#line 332 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() =
                            CNode{UserDouble{YY_MOVE(yystack_[0].value.as<double>())}};
                    }
#line 1188 "pipeline_parser_gen.cpp"
                    break;

                    case 50:
#line 335 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{UserDouble{0.0}};
                    }
#line 1196 "pipeline_parser_gen.cpp"
                    break;

                    case 51:
#line 341 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{UserBoolean{true}};
                    }
#line 1204 "pipeline_parser_gen.cpp"
                    break;

                    case 52:
#line 344 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{UserBoolean{false}};
                    }
#line 1212 "pipeline_parser_gen.cpp"
                    break;

                    case 53:
#line 350 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 1218 "pipeline_parser_gen.cpp"
                    break;

                    case 54:
#line 351 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 1224 "pipeline_parser_gen.cpp"
                    break;

                    case 55:
#line 352 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 1230 "pipeline_parser_gen.cpp"
                    break;

                    case 56:
#line 353 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 1236 "pipeline_parser_gen.cpp"
                    break;

                    case 57:
#line 354 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 1242 "pipeline_parser_gen.cpp"
                    break;

                    case 58:
#line 361 "pipeline_grammar.yy"
                    {
                    }
#line 1248 "pipeline_parser_gen.cpp"
                    break;

                    case 59:
#line 362 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<std::vector<CNode>>() =
                            YY_MOVE(yystack_[0].value.as<std::vector<CNode>>());
                        yylhs.value.as<std::vector<CNode>>().emplace_back(
                            YY_MOVE(yystack_[1].value.as<CNode>()));
                    }
#line 1257 "pipeline_parser_gen.cpp"
                    break;

                    case 60:
#line 368 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 1263 "pipeline_parser_gen.cpp"
                    break;

                    case 61:
#line 369 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 1269 "pipeline_parser_gen.cpp"
                    break;

                    case 62:
#line 373 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 1275 "pipeline_parser_gen.cpp"
                    break;

                    case 63:
#line 377 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 1281 "pipeline_parser_gen.cpp"
                    break;

                    case 64:
#line 378 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 1287 "pipeline_parser_gen.cpp"
                    break;

                    case 65:
#line 381 "pipeline_grammar.yy"
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
#line 1299 "pipeline_parser_gen.cpp"
                    break;

                    case 66:
#line 391 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::atan2,
                             CNode{CNode::ArrayChildren{YY_MOVE(yystack_[3].value.as<CNode>()),
                                                        YY_MOVE(yystack_[2].value.as<CNode>())}}}}};
                    }
#line 1308 "pipeline_parser_gen.cpp"
                    break;


#line 1312 "pipeline_parser_gen.cpp"

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

#if YYDEBUG || 0
const char* PipelineParserGen::symbol_name(symbol_kind_type yysymbol) {
    return yytname_[yysymbol];
}
#endif  // #if YYDEBUG || 0


const signed char PipelineParserGen::yypact_ninf_ = -37;

const signed char PipelineParserGen::yytable_ninf_ = -1;

const signed char PipelineParserGen::yypact_[] = {
    9,   13,  19,  55,  23,  -37, 28,  -37, -5,  -5,  33,  38,  -37, -37, -37, -37, -37,
    -37, 40,  46,  44,  -37, -37, -37, -37, -37, -37, -37, -37, -37, -37, -37, -37, 13,
    -37, 42,  -37, 39,  -37, -37, 54,  -37, -1,  -37, -37, -37, -37, -37, -37, -37, -37,
    -1,  -37, 3,   -4,  -37, -37, -37, -37, -37, -37, -37, -37, -37, -37, -37, -37, -37,
    -37, -37, -37, -37, 63,  71,  72,  -37, 25,  25,  -37, -37, -37, 25,  -37, -37, -37,
    -37, -37, -37, 25,  25,  73,  25,  74,  77,  -37, 78,  -37, -37};

const signed char PipelineParserGen::yydefact_[] = {
    0,  3,  0,  0,  0,  1,  0,  5,  0,  0,  0,  0,  7,  8, 9,  10, 11, 2,  0,  0,
    0,  46, 48, 50, 45, 47, 49, 17, 14, 15, 16, 18, 20, 3, 12, 0,  6,  0,  4,  44,
    0,  19, 0,  37, 38, 39, 40, 41, 42, 43, 36, 0,  21, 0, 0,  26, 28, 30, 32, 33,
    34, 25, 27, 29, 31, 22, 35, 62, 63, 64, 24, 23, 0,  0, 0,  13, 0,  0,  51, 52,
    61, 0,  53, 54, 55, 56, 57, 60, 0,  58, 0,  58, 0,  0, 59, 0,  66, 65};

const signed char PipelineParserGen::yypgoto_[] = {-37, 45,  -37, -37, -37, 75,  -37, -37, -37, -37,
                                                   34,  -36, -13, -37, -37, -37, -3,  32,  47,  -8,
                                                   -37, -37, -37, -37, -2,  -37, -37, -37};

const signed char PipelineParserGen::yydefgoto_[] = {-1, 4,  11, 12, 13, 27, 14, 15, 16, 37,
                                                     65, 80, 91, 67, 68, 69, 82, 83, 84, 85,
                                                     86, 87, 51, 52, 92, 2,  19, 20};

const signed char PipelineParserGen::yytable_[] = {
    30, 30, 54, 21, 22, 23, 66, 55, 56, 57, 58, 59, 60, 23, 1,  66, 3,  73, 74, 5,  24, 25, 26,
    39, 61, 62, 63, 64, 54, 17, 26, 18, 40, 21, 22, 23, 32, 78, 79, 70, 28, 28, 33, 41, 34, 72,
    42, 36, 70, 39, 24, 25, 26, 43, 44, 29, 29, 45, 46, 47, 48, 49, 50, 81, 88, 35, 39, 75, 89,
    6,  7,  8,  9,  10, 53, 90, 76, 77, 38, 93, 95, 96, 97, 0,  31, 71, 0,  0,  0,  94};

const signed char PipelineParserGen::yycheck_[] = {
    8,  9,  3,  8,  9,  10, 42, 8,  9,  10, 11, 12, 13, 10, 5,  51, 3,  21, 22, 0,  25, 26, 27,
    24, 25, 26, 27, 28, 3,  6,  27, 3,  35, 8,  9,  10, 3,  12, 13, 42, 8,  9,  4,  4,  4,  53,
    7,  3,  51, 24, 25, 26, 27, 14, 15, 8,  9,  18, 19, 20, 21, 22, 23, 76, 77, 19, 24, 4,  81,
    14, 15, 16, 17, 18, 20, 88, 5,  5,  33, 6,  6,  4,  4,  -1, 9,  51, -1, -1, -1, 91};

const signed char PipelineParserGen::yystos_[] = {
    0,  5,  54, 3,  30, 0,  14, 15, 16, 17, 18, 31, 32, 33, 35, 36, 37, 6,  3,  55,
    56, 8,  9,  10, 25, 26, 27, 34, 46, 47, 48, 34, 3,  4,  4,  19, 3,  38, 30, 24,
    45, 4,  7,  14, 15, 18, 19, 20, 21, 22, 23, 51, 52, 20, 3,  8,  9,  10, 11, 12,
    13, 25, 26, 27, 28, 39, 40, 42, 43, 44, 45, 39, 48, 21, 22, 4,  5,  5,  12, 13,
    40, 41, 45, 46, 47, 48, 49, 50, 41, 41, 41, 41, 53, 6,  53, 6,  4,  4};

const signed char PipelineParserGen::yyr1_[] = {
    0,  29, 54, 30, 30, 56, 55, 31, 31, 31, 31, 31, 32, 33, 34, 34, 34, 35, 36, 37, 38, 38, 52,
    52, 39, 39, 39, 39, 39, 39, 39, 39, 39, 39, 39, 39, 51, 51, 51, 51, 51, 51, 51, 51, 45, 46,
    46, 47, 47, 48, 48, 49, 49, 50, 50, 50, 50, 50, 53, 53, 41, 41, 40, 42, 42, 43, 44};

const signed char PipelineParserGen::yyr2_[] = {0, 2, 3, 0, 4, 0, 2, 1, 1, 1, 1, 1, 3, 7, 1, 1, 1,
                                                2, 2, 4, 0, 2, 2, 2, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,
                                                1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,
                                                1, 1, 1, 1, 1, 1, 1, 0, 2, 1, 1, 1, 1, 1, 8, 7};


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
    0,   167, 167, 173, 174, 182, 182, 185, 185, 185, 185, 185, 189, 195, 205, 205, 205,
    209, 214, 219, 225, 228, 235, 238, 244, 245, 248, 251, 254, 257, 260, 263, 266, 269,
    272, 275, 279, 284, 287, 290, 293, 296, 299, 302, 308, 314, 317, 323, 326, 332, 335,
    341, 344, 350, 351, 352, 353, 354, 361, 362, 368, 369, 373, 377, 378, 381, 391};

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
#line 1683 "pipeline_parser_gen.cpp"

#line 397 "pipeline_grammar.yy"
