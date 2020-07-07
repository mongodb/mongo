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
#line 72 "src/mongo/db/cst/pipeline_grammar.yy"

#include "mongo/db/cst/bson_lexer.h"

namespace mongo {
// Mandatory error function.
void PipelineParserGen::error(const PipelineParserGen::location_type& loc, const std::string& msg) {
    std::cerr << msg << " at loc " << loc << std::endl;
}
}  // namespace mongo

#line 57 "src/mongo/db/cst/pipeline_parser_gen.cpp"


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

#line 52 "src/mongo/db/cst/pipeline_grammar.yy"
namespace mongo {
#line 149 "src/mongo/db/cst/pipeline_parser_gen.cpp"


/// Build a parser object.
PipelineParserGen::PipelineParserGen(BSONLexer& driver_yyarg, CNode* cst_yyarg)
#if YYDEBUG
    : yydebug_(false),
      yycdebug_(&std::cerr),
#else
    :
#endif
      driver(driver_yyarg),
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
        case 17:  // stageList
            value.YY_MOVE_OR_COPY<CNode>(YY_MOVE(that.value));
            break;

        case 14:  // BOOL
            value.YY_MOVE_OR_COPY<bool>(YY_MOVE(that.value));
            break;

        case 13:  // NUMBER_DOUBLE
            value.YY_MOVE_OR_COPY<double>(YY_MOVE(that.value));
            break;

        case 11:  // NUMBER_INT
            value.YY_MOVE_OR_COPY<int>(YY_MOVE(that.value));
            break;

        case 12:  // NUMBER_LONG
            value.YY_MOVE_OR_COPY<long long>(YY_MOVE(that.value));
            break;

        case 18:  // stage
            value.YY_MOVE_OR_COPY<std::pair<KeyFieldname, CNode>>(YY_MOVE(that.value));
            break;

        case 10:  // STRING
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
    switch (that.type_get()) {
        case 17:  // stageList
            value.move<CNode>(YY_MOVE(that.value));
            break;

        case 14:  // BOOL
            value.move<bool>(YY_MOVE(that.value));
            break;

        case 13:  // NUMBER_DOUBLE
            value.move<double>(YY_MOVE(that.value));
            break;

        case 11:  // NUMBER_INT
            value.move<int>(YY_MOVE(that.value));
            break;

        case 12:  // NUMBER_LONG
            value.move<long long>(YY_MOVE(that.value));
            break;

        case 18:  // stage
            value.move<std::pair<KeyFieldname, CNode>>(YY_MOVE(that.value));
            break;

        case 10:  // STRING
            value.move<std::string>(YY_MOVE(that.value));
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
        case 17:  // stageList
            value.copy<CNode>(that.value);
            break;

        case 14:  // BOOL
            value.copy<bool>(that.value);
            break;

        case 13:  // NUMBER_DOUBLE
            value.copy<double>(that.value);
            break;

        case 11:  // NUMBER_INT
            value.copy<int>(that.value);
            break;

        case 12:  // NUMBER_LONG
            value.copy<long long>(that.value);
            break;

        case 18:  // stage
            value.copy<std::pair<KeyFieldname, CNode>>(that.value);
            break;

        case 10:  // STRING
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
    switch (that.type_get()) {
        case 17:  // stageList
            value.move<CNode>(that.value);
            break;

        case 14:  // BOOL
            value.move<bool>(that.value);
            break;

        case 13:  // NUMBER_DOUBLE
            value.move<double>(that.value);
            break;

        case 11:  // NUMBER_INT
            value.move<int>(that.value);
            break;

        case 12:  // NUMBER_LONG
            value.move<long long>(that.value);
            break;

        case 18:  // stage
            value.move<std::pair<KeyFieldname, CNode>>(that.value);
            break;

        case 10:  // STRING
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
                symbol_type yylookahead(yylex(driver));
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
                case 17:  // stageList
                    yylhs.value.emplace<CNode>();
                    break;

                case 14:  // BOOL
                    yylhs.value.emplace<bool>();
                    break;

                case 13:  // NUMBER_DOUBLE
                    yylhs.value.emplace<double>();
                    break;

                case 11:  // NUMBER_INT
                    yylhs.value.emplace<int>();
                    break;

                case 12:  // NUMBER_LONG
                    yylhs.value.emplace<long long>();
                    break;

                case 18:  // stage
                    yylhs.value.emplace<std::pair<KeyFieldname, CNode>>();
                    break;

                case 10:  // STRING
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
#line 125 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        *cst = std::move(yystack_[1].value.as<CNode>());
                    }
#line 678 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 3:
#line 130 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                    }
#line 684 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 4:
#line 131 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = std::move(yystack_[0].value.as<CNode>());
                        auto& children =
                            stdx::get<CNode::Children>(yylhs.value.as<CNode>().payload);
                        children.emplace_back(
                            std::move(yystack_[2].value.as<std::pair<KeyFieldname, CNode>>()));
                    }
#line 694 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 5:
#line 139 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<std::pair<KeyFieldname, CNode>>() =
                            std::pair{KeyFieldname::inhibitOptimization, CNode::noopLeaf()};
                    }
#line 702 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;


#line 706 "src/mongo/db/cst/pipeline_parser_gen.cpp"

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
std::string PipelineParserGen::yysyntax_error_(state_type, const symbol_type&) const {
    return YY_("syntax error");
}


const signed char PipelineParserGen::yypact_ninf_ = -8;

const signed char PipelineParserGen::yytable_ninf_ = -1;

const signed char PipelineParserGen::yypact_[] = {-7, -2, 2, -6, -4, -8, 3, 1, -8, 4, -2, -8, -8};

const signed char PipelineParserGen::yydefact_[] = {0, 3, 0, 0, 0, 1, 0, 0, 2, 0, 3, 5, 4};

const signed char PipelineParserGen::yypgoto_[] = {-8, -8, -3, -8};

const signed char PipelineParserGen::yydefgoto_[] = {-1, 2, 4, 7};

const signed char PipelineParserGen::yytable_[] = {1, 3, 5, 6, 8, 10, 9, 12, 11};

const signed char PipelineParserGen::yycheck_[] = {7, 3, 0, 9, 8, 4, 3, 10, 4};

const signed char PipelineParserGen::yystos_[] = {0, 7, 16, 3, 17, 0, 9, 18, 8, 3, 4, 4, 17};

const signed char PipelineParserGen::yyr1_[] = {0, 15, 16, 17, 17, 18};

const signed char PipelineParserGen::yyr2_[] = {0, 2, 3, 0, 4, 3};


#if YYDEBUG
// YYTNAME[SYMBOL-NUM] -- String name of the symbol SYMBOL-NUM.
// First, the terminals, then, starting at \a yyntokens_, nonterminals.
const char* const PipelineParserGen::yytname_[] = {"\"EOF\"",
                                                   "error",
                                                   "$undefined",
                                                   "START_OBJECT",
                                                   "END_OBJECT",
                                                   "START_ORDERED_OBJECT",
                                                   "END_ORDERED_OBJECT",
                                                   "START_ARRAY",
                                                   "END_ARRAY",
                                                   "STAGE_INHIBIT_OPTIMIZATION",
                                                   "STRING",
                                                   "NUMBER_INT",
                                                   "NUMBER_LONG",
                                                   "NUMBER_DOUBLE",
                                                   "BOOL",
                                                   "$accept",
                                                   "pipeline",
                                                   "stageList",
                                                   "stage",
                                                   YY_NULLPTR};


const unsigned char PipelineParserGen::yyrline_[] = {0, 125, 125, 130, 131, 139};

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


#line 52 "src/mongo/db/cst/pipeline_grammar.yy"
}  // namespace mongo
#line 1000 "src/mongo/db/cst/pipeline_parser_gen.cpp"

#line 144 "src/mongo/db/cst/pipeline_grammar.yy"
