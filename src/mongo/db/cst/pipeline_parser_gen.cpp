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
#line 77 "src/mongo/db/cst/pipeline_grammar.yy"

#include "mongo/db/cst/bson_lexer.h"

namespace mongo {
// Mandatory error function.
void PipelineParserGen::error(const PipelineParserGen::location_type& loc, const std::string& msg) {
    uasserted(ErrorCodes::FailedToParse,
              str::stream() << msg << " at location " << loc.begin.line << ":" << loc.begin.column
                            << " of input BSON. Lexer produced token of type "
                            << lexer[loc.begin.column].type_get() << ".");
}
}  // namespace mongo

#line 62 "src/mongo/db/cst/pipeline_parser_gen.cpp"


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

#line 52 "src/mongo/db/cst/pipeline_grammar.yy"
namespace mongo {
#line 155 "src/mongo/db/cst/pipeline_parser_gen.cpp"

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
        case 19:  // stageList
        case 20:  // stage
        case 21:  // inhibitOptimization
        case 22:  // unionWith
        case 23:  // num
        case 24:  // skip
        case 25:  // limit
            value.YY_MOVE_OR_COPY<CNode>(YY_MOVE(that.value));
            break;

        case 17:  // BOOL
            value.YY_MOVE_OR_COPY<bool>(YY_MOVE(that.value));
            break;

        case 16:  // NUMBER_DOUBLE
            value.YY_MOVE_OR_COPY<double>(YY_MOVE(that.value));
            break;

        case 14:  // NUMBER_INT
            value.YY_MOVE_OR_COPY<int>(YY_MOVE(that.value));
            break;

        case 15:  // NUMBER_LONG
            value.YY_MOVE_OR_COPY<long long>(YY_MOVE(that.value));
            break;

        case 13:  // STRING
            value.YY_MOVE_OR_COPY<std::string>(YY_MOVE(that.value));
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
        case 19:  // stageList
        case 20:  // stage
        case 21:  // inhibitOptimization
        case 22:  // unionWith
        case 23:  // num
        case 24:  // skip
        case 25:  // limit
            value.move<CNode>(YY_MOVE(that.value));
            break;

        case 17:  // BOOL
            value.move<bool>(YY_MOVE(that.value));
            break;

        case 16:  // NUMBER_DOUBLE
            value.move<double>(YY_MOVE(that.value));
            break;

        case 14:  // NUMBER_INT
            value.move<int>(YY_MOVE(that.value));
            break;

        case 15:  // NUMBER_LONG
            value.move<long long>(YY_MOVE(that.value));
            break;

        case 13:  // STRING
            value.move<std::string>(YY_MOVE(that.value));
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
        case 19:  // stageList
        case 20:  // stage
        case 21:  // inhibitOptimization
        case 22:  // unionWith
        case 23:  // num
        case 24:  // skip
        case 25:  // limit
            value.copy<CNode>(that.value);
            break;

        case 17:  // BOOL
            value.copy<bool>(that.value);
            break;

        case 16:  // NUMBER_DOUBLE
            value.copy<double>(that.value);
            break;

        case 14:  // NUMBER_INT
            value.copy<int>(that.value);
            break;

        case 15:  // NUMBER_LONG
            value.copy<long long>(that.value);
            break;

        case 13:  // STRING
            value.copy<std::string>(that.value);
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
        case 19:  // stageList
        case 20:  // stage
        case 21:  // inhibitOptimization
        case 22:  // unionWith
        case 23:  // num
        case 24:  // skip
        case 25:  // limit
            value.move<CNode>(that.value);
            break;

        case 17:  // BOOL
            value.move<bool>(that.value);
            break;

        case 16:  // NUMBER_DOUBLE
            value.move<double>(that.value);
            break;

        case 14:  // NUMBER_INT
            value.move<int>(that.value);
            break;

        case 15:  // NUMBER_LONG
            value.move<long long>(that.value);
            break;

        case 13:  // STRING
            value.move<std::string>(that.value);
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
                case 19:  // stageList
                case 20:  // stage
                case 21:  // inhibitOptimization
                case 22:  // unionWith
                case 23:  // num
                case 24:  // skip
                case 25:  // limit
                    yylhs.value.emplace<CNode>();
                    break;

                case 17:  // BOOL
                    yylhs.value.emplace<bool>();
                    break;

                case 16:  // NUMBER_DOUBLE
                    yylhs.value.emplace<double>();
                    break;

                case 14:  // NUMBER_INT
                    yylhs.value.emplace<int>();
                    break;

                case 15:  // NUMBER_LONG
                    yylhs.value.emplace<long long>();
                    break;

                case 13:  // STRING
                    yylhs.value.emplace<std::string>();
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
#line 138 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        *cst = std::move(yystack_[1].value.as<CNode>());
                    }
#line 711 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 3:
#line 143 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                    }
#line 717 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 4:
#line 144 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() =
                            CNode{CNode::ArrayChildren{yystack_[2].value.as<CNode>()}};
                    }
#line 725 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 5:
#line 152 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        lexer.sortObjTokens();
                    }
#line 731 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 7:
#line 155 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = yystack_[0].value.as<CNode>();
                    }
#line 737 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 8:
#line 155 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = yystack_[0].value.as<CNode>();
                    }
#line 743 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 9:
#line 155 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = yystack_[0].value.as<CNode>();
                    }
#line 749 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 10:
#line 155 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = yystack_[0].value.as<CNode>();
                    }
#line 755 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 11:
#line 159 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            std::pair{KeyFieldname::inhibitOptimization, CNode::noopLeaf()}}};
                    }
#line 764 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 12:
#line 165 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        auto coll = CNode{UserString(yystack_[3].value.as<std::string>())};
                        auto pipeline = CNode{UserDouble(yystack_[1].value.as<double>())};
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            std::pair{KeyFieldname::unionWith,
                                      CNode{CNode::ObjectChildren{
                                          {KeyFieldname::collArg, std::move(coll)},
                                          {KeyFieldname::pipelineArg, std::move(pipeline)}}}}}};
                    }
#line 778 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 13:
#line 176 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{UserInt(yystack_[0].value.as<int>())};
                    }
#line 786 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 14:
#line 179 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() =
                            CNode{UserLong(yystack_[0].value.as<long long>())};
                    }
#line 794 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 15:
#line 182 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{UserDouble(yystack_[0].value.as<double>())};
                    }
#line 802 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 16:
#line 188 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            std::pair{KeyFieldname::skip, yystack_[0].value.as<CNode>()}}};
                    }
#line 810 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 17:
#line 193 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            std::pair{KeyFieldname::limit, yystack_[0].value.as<CNode>()}}};
                    }
#line 818 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;


#line 822 "src/mongo/db/cst/pipeline_parser_gen.cpp"

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


const signed char PipelineParserGen::yypact_ninf_ = -11;

const signed char PipelineParserGen::yytable_ninf_ = -1;

const signed char PipelineParserGen::yypact_[] = {
    2, 5,  9,   -7,  4,   -11, 8,   -11, -10, -10, 10,  -11, -11, -11, -11, -11, 11,
    1, 13, -11, -11, -11, -11, -11, 5,   -11, 0,   -11, -11, 6,   3,   16,  -11};

const signed char PipelineParserGen::yydefact_[] = {0,  3,  0, 0,  0, 1, 0, 5, 0,  0,  0,
                                                    7,  8,  9, 10, 2, 0, 0, 0, 13, 14, 15,
                                                    16, 17, 3, 11, 0, 6, 4, 0, 0,  0,  12};

const signed char PipelineParserGen::yypgoto_[] = {
    -11, -3, -11, -11, -11, 14, -11, -11, -11, -11, -11};

const signed char PipelineParserGen::yydefgoto_[] = {-1, 4, 10, 11, 12, 22, 13, 14, 2, 17, 18};

const signed char PipelineParserGen::yytable_[] = {6,  7,  8,  9,  19, 20, 21, 1,  3,  5,  15, 16,
                                                   26, 29, 24, 25, 27, 0,  30, 31, 32, 28, 0,  23};

const signed char PipelineParserGen::yycheck_[] = {7,  8,  9, 10, 14, 15, 16, 5,  3, 0,  6,  3,
                                                   11, 13, 4, 4,  3,  -1, 12, 16, 4, 24, -1, 9};

const signed char PipelineParserGen::yystos_[] = {0,  5,  26, 3,  19, 0, 7,  8,  9,  10, 20,
                                                  21, 22, 24, 25, 6,  3, 27, 28, 14, 15, 16,
                                                  23, 23, 4,  4,  11, 3, 19, 13, 12, 16, 4};

const signed char PipelineParserGen::yyr1_[] = {
    0, 18, 26, 19, 19, 28, 27, 20, 20, 20, 20, 21, 22, 23, 23, 23, 24, 25};

const signed char PipelineParserGen::yyr2_[] = {
    0, 2, 3, 0, 4, 0, 2, 1, 1, 1, 1, 3, 7, 1, 1, 1, 2, 2};


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
                                                   "STAGE_INHIBIT_OPTIMIZATION",
                                                   "STAGE_UNION_WITH",
                                                   "STAGE_SKIP",
                                                   "STAGE_LIMIT",
                                                   "COLL_ARG",
                                                   "PIPELINE_ARG",
                                                   "STRING",
                                                   "NUMBER_INT",
                                                   "NUMBER_LONG",
                                                   "NUMBER_DOUBLE",
                                                   "BOOL",
                                                   "$accept",
                                                   "stageList",
                                                   "stage",
                                                   "inhibitOptimization",
                                                   "unionWith",
                                                   "num",
                                                   "skip",
                                                   "limit",
                                                   "pipeline",
                                                   "START_ORDERED_OBJECT",
                                                   "$@1",
                                                   YY_NULLPTR};
#endif


#if YYDEBUG
const unsigned char PipelineParserGen::yyrline_[] = {
    0, 138, 138, 143, 144, 152, 152, 155, 155, 155, 155, 159, 165, 176, 179, 182, 188, 193};

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


#line 52 "src/mongo/db/cst/pipeline_grammar.yy"
}  // namespace mongo
#line 1131 "src/mongo/db/cst/pipeline_parser_gen.cpp"

#line 197 "src/mongo/db/cst/pipeline_grammar.yy"
