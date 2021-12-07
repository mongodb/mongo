/**
 *    Copyright (C) 2019-present MongoDB, Inc.
 *
 *    This program is free software: you can redistribute it and/or modify
 *    it under the terms of the Server Side Public License, version 1,
 *    as published by MongoDB, Inc.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    Server Side Public License for more details.
 *
 *    You should have received a copy of the Server Side Public License
 *    along with this program. If not, see
 *    <http://www.mongodb.com/licensing/server-side-public-license>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the Server Side Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */
#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kQuery

#include "mongo/platform/basic.h"

#include "mongo/db/exec/sbe/parser/parser.h"

#include <charconv>

#include "mongo/db/exec/sbe/stages/branch.h"
#include "mongo/db/exec/sbe/stages/co_scan.h"
#include "mongo/db/exec/sbe/stages/exchange.h"
#include "mongo/db/exec/sbe/stages/filter.h"
#include "mongo/db/exec/sbe/stages/hash_agg.h"
#include "mongo/db/exec/sbe/stages/hash_join.h"
#include "mongo/db/exec/sbe/stages/ix_scan.h"
#include "mongo/db/exec/sbe/stages/limit_skip.h"
#include "mongo/db/exec/sbe/stages/loop_join.h"
#include "mongo/db/exec/sbe/stages/makeobj.h"
#include "mongo/db/exec/sbe/stages/project.h"
#include "mongo/db/exec/sbe/stages/scan.h"
#include "mongo/db/exec/sbe/stages/sort.h"
#include "mongo/db/exec/sbe/stages/sorted_merge.h"
#include "mongo/db/exec/sbe/stages/traverse.h"
#include "mongo/db/exec/sbe/stages/union.h"
#include "mongo/db/exec/sbe/stages/unique.h"
#include "mongo/db/exec/sbe/stages/unwind.h"
#include "mongo/logv2/log.h"
#include "mongo/util/str.h"

namespace mongo {
namespace sbe {

static std::string format_error_message(size_t ln, size_t col, const std::string& msg) {
    return str::stream() << ln << ":" << col << ": " << msg << '\n';
}

static constexpr auto kSyntax = R"(
                ROOT <- OPERATOR

                PLAN_NODE_ID <- ('['([0-9])+']')

                OPERATOR <- PLAN_NODE_ID? (SCAN / PSCAN / SEEK / IXSCAN / IXSEEK / PROJECT / FILTER / CFILTER /
                            MKOBJ / MKBSON / GROUP / HJOIN / NLJOIN / LIMIT / SKIP / COSCAN / TRAVERSE /
                            EXCHANGE / SORT / UNWIND / UNION / BRANCH / SIMPLE_PROJ / PFO /
                            ESPOOL / LSPOOL / CSPOOL / SSPOOL / UNIQUE / SORTED_MERGE)

                FORWARD_FLAG <- <'true'> / <'false'>

                NEED_SLOT_FOR_OPLOG_TS <- <'true'> / <'false'>

                SCAN <- 'scan' IDENT? # optional variable name of the root object (record) delivered by the scan
                               IDENT? # optional variable name of the record id delivered by the scan
                               IDENT? # optional variable name of the snapshot id read by the scan
                               IDENT? # optional variable name of the index name read by the scan
                               IDENT? # optional variable name of the index key read by the scan
                               IDENT? # optional variable name of the index key pattern read by the scan
                               IDENT_LIST_WITH_RENAMES  # list of projected fields (may be empty)
                               IDENT # collection name to scan
                               FORWARD_FLAG # forward scan or not
                               NEED_SLOT_FOR_OPLOG_TS # Whether a slot needs to be created for oplog timestamp

                PSCAN <- 'pscan' IDENT? # optional variable name of the root object (record) delivered by the scan
                                 IDENT? # optional variable name of the record id delivered by the scan
                                 IDENT? # optional variable name of the snapshot id read by the scan
                                 IDENT? # optional variable name of the index name read by the scan
                                 IDENT? # optional variable name of the index key read by the scan
                                 IDENT? # optional variable name of the index key pattern read by the scan
                                 IDENT_LIST_WITH_RENAMES  # list of projected fields (may be empty)
                                 IDENT # collection name to scan

                SEEK <- 'seek' IDENT # variable name of the key
                               IDENT? # optional variable name of the root object (record) delivered by the scan
                               IDENT? # optional variable name of the record id delivered by the scan
                               IDENT? # optional variable name of the snapshot id delivered by the scan
                               IDENT? # optional variable name of the index name read by the scan
                               IDENT? # optional variable name of the index key read by the scan
                               IDENT? # optional variable name of the index key pattern read by the scan
                               IDENT_LIST_WITH_RENAMES  # list of projected fields (may be empty)
                               IDENT # collection name to scan
                               FORWARD_FLAG # forward scan or not
                               NEED_SLOT_FOR_OPLOG_TS # Whether a slot needs to be created for oplog timestamp

                IXSCAN <- 'ixscan' IDENT? # optional variable name of the root object (record) delivered by the scan
                                   IDENT? # optional variable name of the record id delivered by the scan
                                   IDENT? # optional variable name of the snapshot id delivered by the scan
                                   IX_KEY_LIST_WITH_RENAMES  # list of projected fields (may be empty)
                                   IDENT # collection name
                                   IDENT # index name to scan
                                   FORWARD_FLAG # forward scan or not

                IXSEEK <- 'ixseek' IDENT # variable name of the low key
                                   IDENT # variable name of the high key
                                   IDENT? # optional variable name of the root object (record) delivered by the scan
                                   IDENT? # optional variable name of the record id delivered by the scan
                                   IDENT? # optional variable name of the snapshot id delivered by the scan
                                   IX_KEY_LIST_WITH_RENAMES  # list of projected fields (may be empty)
                                   IDENT # collection name
                                   IDENT # index name to scan
                                   FORWARD_FLAG # forward seek or not

                PROJECT <- 'project' PROJECT_LIST OPERATOR
                SIMPLE_PROJ <- '$p' IDENT # output
                                    IDENT # input
                                    IDENT_LIST # correlated slots
                                    PFV # path
                                    OPERATOR # input
                PFV <- (IDENT '.' PFV) / ( '|' EXPR / IDENT)

                FILTER <- 'filter' '{' EXPR '}' OPERATOR
                CFILTER <- 'cfilter' '{' EXPR '}' OPERATOR

                MKOBJ_FLAG <- <'true'> / <'false'>
                MKOBJ_DROP_KEEP_FLAG <- <'drop'> / <'keep'>
                MKOBJ <- 'mkobj' IDENT
                                 (IDENT # Old root
                                  IDENT_LIST # field names
                                  MKOBJ_DROP_KEEP_FLAG)? # drop or keep
                                 IDENT_LIST_WITH_RENAMES # project list
                                 MKOBJ_FLAG # Force new object
                                 MKOBJ_FLAG # Return old object
                                 OPERATOR # child

                MKBSON <- 'mkbson' IDENT
                                   (IDENT # Old root
                                    IDENT_LIST # field names
                                    MKOBJ_DROP_KEEP_FLAG)? # drop or keep
                                   IDENT_LIST_WITH_RENAMES # project list
                                   MKOBJ_FLAG # Force new object
                                   MKOBJ_FLAG # Return old object
                                   OPERATOR # child

                GROUP <- 'group' IDENT_LIST
                                 PROJECT_LIST
                                 IDENT? # optional collator slot
                                 OPERATOR
                HJOIN <- 'hj' IDENT? # optional collator slot
                              LEFT
                              RIGHT
                LEFT <- 'left' IDENT_LIST IDENT_LIST OPERATOR
                RIGHT <- 'right' IDENT_LIST IDENT_LIST OPERATOR
                NLJOIN <- 'nlj' IDENT_LIST # projected outer variables
                                IDENT_LIST # correlated parameters
                                ('{' EXPR '}')? # optional predicate
                                'left' OPERATOR # outer side
                                'right' OPERATOR # inner side

                LIMIT <- 'limit' NUMBER OPERATOR
                SKIP <- 'limitskip' (NUMBER / 'none') NUMBER OPERATOR
                COSCAN <- 'coscan'
                TRAVERSE <- 'traverse' IDENT # output of traverse
                                       IDENT # output of traverse as seen inside the 'in' branch
                                       IDENT # input of traverse
                                       IDENT_LIST? # optional correlated slots
                                       (('{' (EXPR) '}') / EMPTY_EXPR_TOK) # optional fold expression
                                       (('{' (EXPR) '}') / EMPTY_EXPR_TOK) # optional final expression
                                       'from' OPERATOR
                                       'in' OPERATOR
                EXCHANGE <- 'exchange' IDENT_LIST NUMBER IDENT OPERATOR
                SORT <- 'sort' IDENT_LIST
                               SORT_DIR_LIST # sort directions
                               IDENT_LIST
                               NUMBER? # optional 'limit'
                               OPERATOR
                UNWIND <- 'unwind' IDENT IDENT IDENT UNWIND_FLAG OPERATOR
                UNWIND_FLAG <- <'true'> / <'false'>
                UNION <- 'union' IDENT_LIST UNION_BRANCH_LIST

                UNION_BRANCH_LIST <- '[' (UNION_BRANCH (',' UNION_BRANCH)* )?']'
                UNION_BRANCH <- IDENT_LIST OPERATOR

                SORTED_MERGE <- 'smerge' IDENT_LIST SORT_DIR_LIST SORTED_MERGE_BRANCH_LIST
                SORTED_MERGE_BRANCH_LIST <- '[' (SORTED_MERGE_BRANCH (',' SORTED_MERGE_BRANCH)* )?']'
                SORTED_MERGE_BRANCH <- IDENT_LIST # key slots
                                       IDENT_LIST # value slots
                                       OPERATOR

                BRANCH <- 'branch' '{' EXPR '}' # boolean condition/switch
                                   IDENT_LIST # output of the operator
                                   IDENT_LIST # output of the then branch
                                   OPERATOR # then branch
                                   IDENT_LIST # output of the else branch
                                   OPERATOR # else branch
                PFO <- '$pfo' IDENT # output
                              IDENT # input
                              IDENT_LIST # correlated slots
                              PATH # path
                              OPERATOR # input operator
                PATH <- '{' PF (',' PF)* '}'
                PF <- (IDENT ':' PF_ACTION) / PF_DROPALL
                PF_ACTION <- PATH / PF_EXPR / PF_MEXPR / PF_DROP / PF_INCL
                PF_DROP <- '0'
                PF_INCL <- '1'
                PF_DROPALL <- '~'
                PF_EXPR <- '=' EXPR
                PF_MEXPR <- '|' EXPR

                ESPOOL <- 'espool' IDENT # buffer
                                   IDENT_LIST # slots
                                   OPERATOR # input stage

                LSPOOL <- 'lspool' IDENT # buffer
                                   IDENT_LIST # slots
                                   ('{' EXPR '}')? # optional predicate
                                   OPERATOR # input stage

                CSPOOL <- 'cspool' IDENT # buffer
                                   IDENT_LIST # slots

                SSPOOL <- 'sspool' IDENT # buffer
                                   IDENT_LIST # slots

                UNIQUE <- 'unique' IDENT_LIST # slots
                                   OPERATOR # input

                PROJECT_LIST <- '[' (ASSIGN (',' ASSIGN)* )?']'
                ASSIGN <- IDENT '=' EXPR

                EXPR <- EQOP_EXPR? LOG_TOK EXPR / EQOP_EXPR
                LOG_TOK <- <'&&'> / <'||'> / <'!'>

                EQOP_EXPR <- RELOP_EXPR EQ_TOK ('[' EXPR ']')? EQOP_EXPR / RELOP_EXPR
                EQ_TOK <- <'=='> / <'!='>

                RELOP_EXPR <- ADD_EXPR REL_TOK ('[' EXPR ']')? RELOP_EXPR / ADD_EXPR
                REL_TOK <- <'<=>'> / <'<='> / <'<'> / <'>='> / <'>'>

                ADD_EXPR <- MUL_EXPR ADD_TOK ADD_EXPR / MUL_EXPR
                ADD_TOK <- <'+'> / <'-'>

                MUL_EXPR <- PRIMARY_EXPR MUL_TOK MUL_EXPR / PRIMARY_EXPR
                MUL_TOK <- <'*'> / <'/'>

                PRIMARY_EXPR <- '(' EXPR ')' / CONST_TOK / IF_EXPR / LET_EXPR / FUN_CALL / LAMBDA_EXPR / IDENT / NUMBER / STRING
                CONST_TOK <- <'true'> / <'false'> / <'null'> / <'#'> / EMPTY_EXPR_TOK
                EMPTY_EXPR_TOK <- <'{}'>

                IF_EXPR <- 'if' '(' EXPR ',' EXPR ',' EXPR ')'

                LET_EXPR <- 'let' FRAME_PROJECT_LIST EXPR
                FRAME_PROJECT_LIST <- '[' (ASSIGN (',' ASSIGN)* )?']'

                LAMBDA_EXPR <- '\\' IDENT '.' EXPR

                FUN_CALL <- IDENT # function call identifier
                            '(' (EXPR (',' EXPR)*)? ')' # function call arguments

                IDENT_LIST_WITH_RENAMES <- '[' (IDENT_WITH_RENAME (',' IDENT_WITH_RENAME)*)? ']'
                IDENT_WITH_RENAME <- IDENT ('=' IDENT)?

                IX_KEY_LIST_WITH_RENAMES <- '[' (IX_KEY_WITH_RENAME (',' IX_KEY_WITH_RENAME)*)? ']'
                IX_KEY_WITH_RENAME <- IDENT '=' NUMBER

                IDENT_LIST <- '[' (IDENT (',' IDENT)*)? ']'
                IDENT <- RAW_IDENT/ESC_IDENT

                STRING <- < '"' (!'"' .)* '"' > / < '\'' (!'\'' .)* '\'' >
                STRING_LIST <- '[' (STRING (',' STRING)*)? ']'

                NUMBER      <- < [0-9]+ >

                RAW_IDENT <- < !STAGE_KEYWORDS ([$a-zA-Z_] [$a-zA-Z0-9-_]*) >

                ESC_IDENT <- < '@' '"' (!'"' .)* '"' >

                SORT_DIR <- <'asc'> / <'desc'>
                SORT_DIR_LIST <- '[' (SORT_DIR (',' SORT_DIR)* )? ']'

                STAGE_KEYWORDS <- <'left'> / <'right'> # reserved keywords to avoid collision with IDENT names

                %whitespace  <-  ([ \t\r\n]* ('#' (!'\n' .)* '\n' [ \t\r\n]*)*)
                %word        <-  [a-z]+
        )";

std::pair<IndexKeysInclusionSet, sbe::value::SlotVector> Parser::lookupIndexKeyRenames(
    const std::vector<std::string>& renames, const std::vector<size_t>& indexKeys) {
    IndexKeysInclusionSet indexKeysInclusion;

    // Each indexKey is associated with the parallel remap from the 'renames' vector. This
    // map explicitly binds each indexKey with its remap and sorts them by 'indexKey' order.
    std::map<int, sbe::value::SlotId> slotsByKeyIndex;
    invariant(renames.size() == indexKeys.size());
    for (size_t idx = 0; idx < renames.size(); idx++) {
        uassert(4872100,
                str::stream() << "Cannot project index key at position " << indexKeys[idx],
                indexKeys[idx] < indexKeysInclusion.size());
        slotsByKeyIndex[indexKeys[idx]] = lookupSlotStrict(renames[idx]);
    }

    sbe::value::SlotVector slots;
    for (auto&& [indexKey, slot] : slotsByKeyIndex) {
        indexKeysInclusion.set(indexKey);
        slots.push_back(slot);
    }

    return {indexKeysInclusion, std::move(slots)};
}

std::vector<sbe::value::SortDirection> parseSortDirList(const AstQuery& ast) {
    std::vector<value::SortDirection> dirs;
    for (const auto& node : ast.nodes) {
        if (node->token == "asc") {
            dirs.push_back(value::SortDirection::Ascending);
        } else if (node->token == "desc") {
            dirs.push_back(value::SortDirection::Descending);
        } else {
            MONGO_UNREACHABLE;
        }
    }
    return dirs;
}

MakeObjFieldBehavior parseFieldBehavior(StringData val) {
    if (val == "drop") {
        return MakeObjFieldBehavior::drop;
    } else if (val == "keep") {
        return MakeObjFieldBehavior::keep;
    }
    MONGO_UNREACHABLE_TASSERT(5389100);
}

void Parser::walkChildren(AstQuery& ast) {
    for (const auto& node : ast.nodes) {
        walk(*node);
    }
}

void Parser::walkIdent(AstQuery& ast) {
    auto str = ast.nodes[0]->token;
    // Drop @" .. ".
    if (!str.empty() && str[0] == '@') {
        str = str.substr(2, str.size() - 3);
    }
    ast.identifier = std::move(str);
}

void Parser::walkIdentList(AstQuery& ast) {
    walkChildren(ast);
    for (auto& node : ast.nodes) {
        ast.identifiers.emplace_back(std::move(node->identifier));
    }
}

void Parser::walkIdentWithRename(AstQuery& ast) {
    walkChildren(ast);
    std::string identifier;
    std::string rename;

    if (ast.nodes.size() == 1) {
        rename = ast.nodes[0]->identifier;
        identifier = rename;
    } else {
        rename = ast.nodes[0]->identifier;
        identifier = ast.nodes[1]->identifier;
    }

    ast.identifier = std::move(identifier);
    ast.rename = std::move(rename);
}

void Parser::walkIdentListWithRename(AstQuery& ast) {
    walkChildren(ast);

    for (auto& node : ast.nodes) {
        ast.identifiers.emplace_back(std::move(node->identifier));
        ast.renames.emplace_back(std::move(node->rename));
    }
}

void Parser::walkIxKeyWithRename(AstQuery& ast) {
    walkChildren(ast);

    ast.rename = ast.nodes[0]->identifier;

    // This token cannot be negative, because the parser does not accept a "-" prefix.
    ast.indexKey = static_cast<size_t>(std::stoi(ast.nodes[1]->token));
}

void Parser::walkIxKeyListWithRename(AstQuery& ast) {
    walkChildren(ast);

    for (auto& node : ast.nodes) {
        ast.indexKeys.emplace_back(std::move(node->indexKey));
        ast.renames.emplace_back(std::move(node->rename));
    }
}

void Parser::walkProjectList(AstQuery& ast) {
    walkChildren(ast);

    for (size_t idx = 0; idx < ast.nodes.size(); ++idx) {
        ast.projects[ast.nodes[idx]->identifier] = std::move(ast.nodes[idx]->expr);
    }
}

void Parser::walkAssign(AstQuery& ast) {
    walkChildren(ast);

    ast.identifier = ast.nodes[0]->identifier;
    ast.expr = std::move(ast.nodes[1]->expr);
}

void Parser::walkExpr(AstQuery& ast) {
    walkChildren(ast);
    if (ast.nodes.size() == 1) {
        ast.expr = std::move(ast.nodes[0]->expr);
    } else if (ast.nodes.size() == 2) {
        // Check if it is an expression with a unary op.
        EPrimUnary::Op op;
        if (ast.nodes[0]->token == "!") {
            op = EPrimUnary::logicNot;
        } else {
            MONGO_UNREACHABLE_TASSERT(5088501);
        }

        ast.expr = makeE<EPrimUnary>(op, std::move(ast.nodes[1]->expr));
    } else {
        // Otherwise we have an expression with a binary op.
        EPrimBinary::Op op;
        if (ast.nodes[1]->token == "&&") {
            op = EPrimBinary::logicAnd;
        } else if (ast.nodes[1]->token == "||") {
            op = EPrimBinary::logicOr;
        } else {
            MONGO_UNREACHABLE_TASSERT(5088502);
        }

        ast.expr =
            makeE<EPrimBinary>(op, std::move(ast.nodes[0]->expr), std::move(ast.nodes[2]->expr));
    }
}

void Parser::walkEqopExpr(AstQuery& ast) {
    walkChildren(ast);
    if (ast.nodes.size() == 1) {
        ast.expr = std::move(ast.nodes[0]->expr);
    } else {
        EPrimBinary::Op op;
        if (ast.nodes[1]->token == "==") {
            op = EPrimBinary::eq;
        } else if (ast.nodes[1]->token == "!=") {
            op = EPrimBinary::neq;
        }

        if (ast.nodes.size() == 4) {
            ast.expr = makeE<EPrimBinary>(op,
                                          std::move(ast.nodes[0]->expr),
                                          std::move(ast.nodes[3]->expr),
                                          std::move(ast.nodes[2]->expr));
        } else {
            ast.expr = makeE<EPrimBinary>(
                op, std::move(ast.nodes[0]->expr), std::move(ast.nodes[2]->expr));
        }
    }
}

void Parser::walkRelopExpr(AstQuery& ast) {
    walkChildren(ast);
    if (ast.nodes.size() == 1) {
        ast.expr = std::move(ast.nodes[0]->expr);
    } else {
        EPrimBinary::Op op;
        if (ast.nodes[1]->token == "<=>") {
            op = EPrimBinary::cmp3w;
        } else if (ast.nodes[1]->token == "<=") {
            op = EPrimBinary::lessEq;
        } else if (ast.nodes[1]->token == "<") {
            op = EPrimBinary::less;
        } else if (ast.nodes[1]->token == ">=") {
            op = EPrimBinary::greaterEq;
        } else if (ast.nodes[1]->token == ">") {
            op = EPrimBinary::greater;
        } else {
            MONGO_UNREACHABLE;
        }

        if (ast.nodes.size() == 4) {
            ast.expr = makeE<EPrimBinary>(op,
                                          std::move(ast.nodes[0]->expr),
                                          std::move(ast.nodes[3]->expr),
                                          std::move(ast.nodes[2]->expr));
        } else {
            ast.expr = makeE<EPrimBinary>(
                op, std::move(ast.nodes[0]->expr), std::move(ast.nodes[2]->expr));
        }
    }
}

void Parser::walkAddExpr(AstQuery& ast) {
    walkChildren(ast);
    if (ast.nodes.size() == 1) {
        ast.expr = std::move(ast.nodes[0]->expr);
    } else {
        ast.expr =
            makeE<EPrimBinary>(ast.nodes[1]->token == "+" ? EPrimBinary::add : EPrimBinary::sub,
                               std::move(ast.nodes[0]->expr),
                               std::move(ast.nodes[2]->expr));
    }
}

void Parser::walkMulExpr(AstQuery& ast) {
    walkChildren(ast);
    if (ast.nodes.size() == 1) {
        ast.expr = std::move(ast.nodes[0]->expr);
    } else {
        ast.expr =
            makeE<EPrimBinary>(ast.nodes[1]->token == "*" ? EPrimBinary::mul : EPrimBinary::div,
                               std::move(ast.nodes[0]->expr),
                               std::move(ast.nodes[2]->expr));
    }
}

void Parser::walkPrimaryExpr(AstQuery& ast) {
    using namespace peg::udl;

    walkChildren(ast);

    if (ast.nodes[0]->tag == "IDENT"_) {
        // Lookup local binds (let) first.
        auto symbol = lookupSymbol(ast.nodes[0]->identifier);
        if (symbol) {
            ast.expr = makeE<EVariable>(symbol->id, symbol->slotId);
        } else {
            ast.expr = makeE<EVariable>(lookupSlotStrict(ast.nodes[0]->identifier));
        }
    } else if (ast.nodes[0]->tag == "NUMBER"_) {
        ast.expr = makeE<EConstant>(value::TypeTags::NumberInt64,
                                    value::bitcastFrom<int64_t>(std::stoll(ast.nodes[0]->token)));
    } else if (ast.nodes[0]->tag == "CONST_TOK"_) {
        if (ast.nodes[0]->token == "true") {
            ast.expr = makeE<EConstant>(value::TypeTags::Boolean, value::bitcastFrom<bool>(true));
        } else if (ast.nodes[0]->token == "false") {
            ast.expr = makeE<EConstant>(value::TypeTags::Boolean, value::bitcastFrom<bool>(false));
        } else if (ast.nodes[0]->token == "null") {
            ast.expr = makeE<EConstant>(value::TypeTags::Null, 0);
        } else if (ast.nodes[0]->token == "#") {
            ast.expr = makeE<EConstant>(value::TypeTags::Nothing, 0);
        }
    } else if (ast.nodes[0]->tag == "EXPR"_) {
        ast.expr = std::move(ast.nodes[0]->expr);
    } else if (ast.nodes[0]->tag == "IF_EXPR"_) {
        ast.expr = std::move(ast.nodes[0]->expr);
    } else if (ast.nodes[0]->tag == "LET_EXPR"_) {
        ast.expr = std::move(ast.nodes[0]->expr);
    } else if (ast.nodes[0]->tag == "LAMBDA_EXPR"_) {
        ast.expr = std::move(ast.nodes[0]->expr);
    } else if (ast.nodes[0]->tag == "FUN_CALL"_) {
        ast.expr = std::move(ast.nodes[0]->expr);
    } else if (ast.nodes[0]->tag == "STRING"_) {
        std::string str = ast.nodes[0]->token;
        // Drop quotes.
        str = str.substr(1, str.size() - 2);
        ast.expr = makeE<EConstant>(str);
    }
}
void Parser::walkLetExpr(AstQuery& ast) {
    auto frame = newFrameSymbolTable();
    walkChildren(ast);

    auto plist = ast.nodes[0];
    EExpression::Vector binds;

    binds.resize(frame->table.size());
    for (auto& symbol : frame->table) {
        auto it = plist->projects.find(symbol.first);
        invariant(it != plist->projects.end());
        binds[symbol.second] = std::move(it->second);
    }

    ast.expr = makeE<ELocalBind>(frame->id, std::move(binds), std::move(ast.nodes[1]->expr));

    popFrameSymbolTable();
}

void Parser::walkLambdaExpr(AstQuery& ast) {
    auto frame = newFrameSymbolTable();

    walk(*ast.nodes[0]);
    frame->table[ast.nodes[0]->identifier] = value::SlotId{0};

    walk(*ast.nodes[1]);

    ast.expr = makeE<ELocalLambda>(frame->id, std::move(ast.nodes[1]->expr));

    popFrameSymbolTable();
}

void Parser::walkFrameProjectList(AstQuery& ast) {
    value::SlotId slotId{0};
    for (size_t idx = 0; idx < ast.nodes.size(); ++idx) {
        walk(*ast.nodes[idx]);
        currentFrameSymbolTable()->table[ast.nodes[idx]->identifier] = slotId++;

        ast.projects[ast.nodes[idx]->identifier] = std::move(ast.nodes[idx]->expr);
    }
}

void Parser::walkIfExpr(AstQuery& ast) {
    walkChildren(ast);

    ast.expr = makeE<EIf>(std::move(ast.nodes[0]->expr),
                          std::move(ast.nodes[1]->expr),
                          std::move(ast.nodes[2]->expr));
}

void Parser::walkFunCall(AstQuery& ast) {
    walkChildren(ast);
    EExpression::Vector args;

    for (size_t idx = 1; idx < ast.nodes.size(); ++idx) {
        args.emplace_back(std::move(ast.nodes[idx]->expr));
    }

    ast.expr = makeE<EFunction>(ast.nodes[0]->identifier, std::move(args));
}

void Parser::walkScan(AstQuery& ast) {
    walkChildren(ast);

    std::string recordName;
    std::string recordIdName;
    std::string snapshotIdName;
    std::string indexIdName;
    std::string indexKeyName;
    std::string indexKeyPatternName;
    int projectsPos;

    if (ast.nodes.size() == 10) {
        recordName = std::move(ast.nodes[0]->identifier);
        recordIdName = std::move(ast.nodes[1]->identifier);
        snapshotIdName = std::move(ast.nodes[2]->identifier);
        indexIdName = std::move(ast.nodes[3]->identifier);
        indexKeyName = std::move(ast.nodes[4]->identifier);
        indexKeyPatternName = std::move(ast.nodes[5]->identifier);
        projectsPos = 6;
    } else if (ast.nodes.size() == 6) {
        recordName = std::move(ast.nodes[0]->identifier);
        recordIdName = std::move(ast.nodes[1]->identifier);
        projectsPos = 2;
    } else if (ast.nodes.size() == 5) {
        recordName = std::move(ast.nodes[0]->identifier);
        projectsPos = 1;
    } else if (ast.nodes.size() == 4) {
        projectsPos = 0;
    } else {
        uasserted(5290715, "Wrong number of arguments for SCAN");
    }
    auto lastPos = ast.nodes.size() - 1;

    // The 'collName' should be third from last.
    auto collName = std::move(ast.nodes[lastPos - 2]->identifier);

    // The 'FORWARD' should be second last.
    const auto forward = (ast.nodes[lastPos - 1]->token == "true") ? true : false;

    // The 'NEED_SLOT_FOR_OPLOG_TS' always comes at the end.
    const auto oplogTs = (ast.nodes[lastPos]->token == "true")
        ? boost::optional<value::SlotId>(_env->registerSlot(
              "oplogTs"_sd, value::TypeTags::Nothing, 0, false, &_slotIdGenerator))
        : boost::none;

    ast.stage = makeS<ScanStage>(getCollectionUuid(collName),
                                 lookupSlot(recordName),
                                 lookupSlot(recordIdName),
                                 lookupSlot(snapshotIdName),
                                 lookupSlot(indexIdName),
                                 lookupSlot(indexKeyName),
                                 lookupSlot(indexKeyPatternName),
                                 oplogTs,
                                 ast.nodes[projectsPos]->identifiers,
                                 lookupSlots(ast.nodes[projectsPos]->renames),
                                 boost::none,
                                 forward,
                                 nullptr,
                                 getCurrentPlanNodeId(),
                                 ScanCallbacks{});
}

void Parser::walkParallelScan(AstQuery& ast) {
    walkChildren(ast);

    std::string recordName;
    std::string recordIdName;
    std::string snapshotIdName;
    std::string indexIdName;
    std::string indexKeyName;
    std::string indexKeyPatternName;
    std::string collName;
    int projectsPos;

    if (ast.nodes.size() == 8) {
        recordName = std::move(ast.nodes[0]->identifier);
        recordIdName = std::move(ast.nodes[1]->identifier);
        snapshotIdName = std::move(ast.nodes[2]->identifier);
        indexIdName = std::move(ast.nodes[3]->identifier);
        indexKeyName = std::move(ast.nodes[4]->identifier);
        indexKeyPatternName = std::move(ast.nodes[5]->identifier);
        projectsPos = 6;
        collName = std::move(ast.nodes[7]->identifier);
    } else if (ast.nodes.size() == 4) {
        recordName = std::move(ast.nodes[0]->identifier);
        recordIdName = std::move(ast.nodes[1]->identifier);
        projectsPos = 2;
        collName = std::move(ast.nodes[3]->identifier);
    } else if (ast.nodes.size() == 3) {
        recordName = std::move(ast.nodes[0]->identifier);
        projectsPos = 1;
        collName = std::move(ast.nodes[2]->identifier);
    } else if (ast.nodes.size() == 2) {
        projectsPos = 0;
        collName = std::move(ast.nodes[1]->identifier);
    } else {
        uasserted(5290716, "Wrong number of arguments for PSCAN");
    }

    ast.stage = makeS<ParallelScanStage>(getCollectionUuid(collName),
                                         lookupSlot(recordName),
                                         lookupSlot(recordIdName),
                                         lookupSlot(snapshotIdName),
                                         lookupSlot(indexIdName),
                                         lookupSlot(indexKeyName),
                                         lookupSlot(indexKeyPatternName),
                                         ast.nodes[projectsPos]->identifiers,
                                         lookupSlots(ast.nodes[projectsPos]->renames),
                                         nullptr,
                                         getCurrentPlanNodeId(),
                                         ScanCallbacks{});
}

void Parser::walkSeek(AstQuery& ast) {
    walkChildren(ast);

    std::string recordName;
    std::string recordIdName;
    std::string snapshotIdName;
    std::string indexIdName;
    std::string indexKeyName;
    std::string indexKeyPatternName;

    int projectsPos;

    if (ast.nodes.size() == 11) {
        recordName = std::move(ast.nodes[1]->identifier);
        recordIdName = std::move(ast.nodes[2]->identifier);
        snapshotIdName = std::move(ast.nodes[3]->identifier);
        indexIdName = std::move(ast.nodes[4]->identifier);
        indexKeyName = std::move(ast.nodes[5]->identifier);
        indexKeyPatternName = std::move(ast.nodes[6]->identifier);
        projectsPos = 7;
    } else if (ast.nodes.size() == 7) {
        recordName = std::move(ast.nodes[1]->identifier);
        recordIdName = std::move(ast.nodes[2]->identifier);
        projectsPos = 3;
    } else if (ast.nodes.size() == 6) {
        recordName = std::move(ast.nodes[1]->identifier);
        projectsPos = 2;
    } else if (ast.nodes.size() == 5) {
        projectsPos = 1;
    } else {
        uasserted(5290717, "Wrong number of arguments for SEEK");
    }
    auto lastPos = ast.nodes.size() - 1;

    // The 'collName' should be third from last.
    auto collName = std::move(ast.nodes[lastPos - 2]->identifier);

    // The 'FORWARD' should be second last.
    const auto forward = (ast.nodes[lastPos - 1]->token == "true") ? true : false;

    // The 'NEED_SLOT_FOR_OPLOG_TS' always comes at the end.
    const auto oplogTs = (ast.nodes[lastPos]->token == "true")
        ? boost::optional<value::SlotId>(_env->registerSlot(
              "oplogTs"_sd, value::TypeTags::Nothing, 0, false, &_slotIdGenerator))
        : boost::none;

    ast.stage = makeS<ScanStage>(getCollectionUuid(collName),
                                 lookupSlot(recordName),
                                 lookupSlot(recordIdName),
                                 lookupSlot(snapshotIdName),
                                 lookupSlot(indexIdName),
                                 lookupSlot(indexKeyName),
                                 lookupSlot(indexKeyPatternName),
                                 oplogTs,
                                 ast.nodes[projectsPos]->identifiers,
                                 lookupSlots(ast.nodes[projectsPos]->renames),
                                 lookupSlot(ast.nodes[0]->identifier),
                                 forward,
                                 nullptr,
                                 getCurrentPlanNodeId(),
                                 ScanCallbacks{});
}

void Parser::walkIndexScan(AstQuery& ast) {
    walkChildren(ast);

    std::string recordName;
    std::string recordIdName;
    std::string snapshotIdName;
    std::string collName;
    std::string indexName;
    int projectsPos;
    int forwardPos;

    if (ast.nodes.size() == 7) {
        recordName = std::move(ast.nodes[0]->identifier);
        recordIdName = std::move(ast.nodes[1]->identifier);
        snapshotIdName = std::move(ast.nodes[2]->identifier);
        projectsPos = 3;
        collName = std::move(ast.nodes[4]->identifier);
        indexName = std::move(ast.nodes[5]->identifier);
        forwardPos = 6;
    } else if (ast.nodes.size() == 6) {
        recordName = std::move(ast.nodes[0]->identifier);
        recordIdName = std::move(ast.nodes[1]->identifier);
        projectsPos = 2;
        collName = std::move(ast.nodes[3]->identifier);
        indexName = std::move(ast.nodes[4]->identifier);
        forwardPos = 5;
    } else if (ast.nodes.size() == 5) {
        recordName = std::move(ast.nodes[0]->identifier);
        projectsPos = 1;
        collName = std::move(ast.nodes[2]->identifier);
        indexName = std::move(ast.nodes[3]->identifier);
        forwardPos = 4;
    } else if (ast.nodes.size() == 4) {
        projectsPos = 0;
        collName = std::move(ast.nodes[1]->identifier);
        indexName = std::move(ast.nodes[2]->identifier);
        forwardPos = 3;
    } else {
        MONGO_UNREACHABLE
    }

    const auto forward = (ast.nodes[forwardPos]->token == "true") ? true : false;

    auto [indexKeysInclusion, vars] =
        lookupIndexKeyRenames(ast.nodes[projectsPos]->renames, ast.nodes[projectsPos]->indexKeys);

    ast.stage = makeS<IndexScanStage>(getCollectionUuid(collName),
                                      indexName,
                                      forward,
                                      lookupSlot(recordName),
                                      lookupSlot(recordIdName),
                                      lookupSlot(snapshotIdName),
                                      indexKeysInclusion,
                                      vars,
                                      boost::none,
                                      boost::none,
                                      nullptr,
                                      getCurrentPlanNodeId());
}

void Parser::walkIndexSeek(AstQuery& ast) {
    walkChildren(ast);

    std::string recordName;
    std::string recordIdName;
    std::string snapshotIdName;
    std::string collName;
    std::string indexName;
    int projectsPos;
    int forwardPos;

    if (ast.nodes.size() == 9) {
        recordName = std::move(ast.nodes[2]->identifier);
        recordIdName = std::move(ast.nodes[3]->identifier);
        snapshotIdName = std::move(ast.nodes[4]->identifier);
        projectsPos = 5;
        collName = std::move(ast.nodes[6]->identifier);
        indexName = std::move(ast.nodes[7]->identifier);
        forwardPos = 8;
    } else if (ast.nodes.size() == 8) {
        recordName = std::move(ast.nodes[2]->identifier);
        recordIdName = std::move(ast.nodes[3]->identifier);
        projectsPos = 4;
        collName = std::move(ast.nodes[5]->identifier);
        indexName = std::move(ast.nodes[6]->identifier);
        forwardPos = 7;
    } else if (ast.nodes.size() == 7) {
        recordName = std::move(ast.nodes[2]->identifier);
        projectsPos = 3;
        collName = std::move(ast.nodes[4]->identifier);
        indexName = std::move(ast.nodes[5]->identifier);
        forwardPos = 6;
    } else if (ast.nodes.size() == 6) {
        projectsPos = 2;
        collName = std::move(ast.nodes[3]->identifier);
        indexName = std::move(ast.nodes[4]->identifier);
        forwardPos = 5;
    } else {
        MONGO_UNREACHABLE
    }

    const auto forward = (ast.nodes[forwardPos]->token == "true") ? true : false;

    auto [indexKeysInclusion, vars] =
        lookupIndexKeyRenames(ast.nodes[projectsPos]->renames, ast.nodes[projectsPos]->indexKeys);

    ast.stage = makeS<IndexScanStage>(getCollectionUuid(collName),
                                      indexName,
                                      forward,
                                      lookupSlot(recordName),
                                      lookupSlot(recordIdName),
                                      lookupSlot(snapshotIdName),
                                      indexKeysInclusion,
                                      vars,
                                      lookupSlot(ast.nodes[0]->identifier),
                                      lookupSlot(ast.nodes[1]->identifier),
                                      nullptr,
                                      getCurrentPlanNodeId());
}

void Parser::walkProject(AstQuery& ast) {
    walkChildren(ast);

    ast.stage = makeS<ProjectStage>(std::move(ast.nodes[1]->stage),
                                    lookupSlots(std::move(ast.nodes[0]->projects)),
                                    getCurrentPlanNodeId());
}

void Parser::walkFilter(AstQuery& ast) {
    walkChildren(ast);

    ast.stage = makeS<FilterStage<false>>(
        std::move(ast.nodes[1]->stage), std::move(ast.nodes[0]->expr), getCurrentPlanNodeId());
}

void Parser::walkCFilter(AstQuery& ast) {
    walkChildren(ast);

    ast.stage = makeS<FilterStage<true>>(
        std::move(ast.nodes[1]->stage), std::move(ast.nodes[0]->expr), getCurrentPlanNodeId());
}

void Parser::walkSort(AstQuery& ast) {
    walkChildren(ast);

    int inputStagePos;
    size_t limit = std::numeric_limits<std::size_t>::max();

    // Check if 'limit' was specified.
    if (ast.nodes.size() == 5) {
        limit = std::stoi(ast.nodes[3]->token);
        inputStagePos = 4;
    } else {
        inputStagePos = 3;
    }
    auto dirs = parseSortDirList(*ast.nodes[1]);

    ast.stage = makeS<SortStage>(std::move(ast.nodes[inputStagePos]->stage),
                                 lookupSlots(ast.nodes[0]->identifiers),
                                 std::move(dirs),
                                 lookupSlots(ast.nodes[2]->identifiers),
                                 limit,
                                 std::numeric_limits<std::size_t>::max(),
                                 true /* allowDiskUse */,
                                 getCurrentPlanNodeId());
}

void Parser::walkUnion(AstQuery& ast) {
    walkChildren(ast);

    PlanStage::Vector inputStages;
    std::vector<value::SlotVector> inputVals;
    value::SlotVector outputVals{lookupSlots(ast.nodes[0]->identifiers)};

    for (size_t idx = 0; idx < ast.nodes[1]->nodes.size(); idx++) {
        inputVals.push_back(lookupSlots(ast.nodes[1]->nodes[idx]->identifiers));
        inputStages.push_back(std::move(ast.nodes[1]->nodes[idx]->stage));
    }

    uassert(ErrorCodes::FailedToParse,
            "Union output values and input values mismatch",
            std::all_of(
                inputVals.begin(), inputVals.end(), [size = outputVals.size()](const auto& slots) {
                    return slots.size() == size;
                }));

    ast.stage = makeS<UnionStage>(std::move(inputStages),
                                  std::move(inputVals),
                                  std::move(outputVals),
                                  getCurrentPlanNodeId());
}

void Parser::walkUnionBranch(AstQuery& ast) {
    walkChildren(ast);

    ast.identifiers = std::move(ast.nodes[0]->identifiers);
    ast.stage = std::move(ast.nodes[1]->stage);
}

void Parser::walkSortedMerge(AstQuery& ast) {
    walkChildren(ast);

    PlanStage::Vector inputStages;
    std::vector<value::SlotVector> inputKeys;
    std::vector<value::SlotVector> inputVals;
    value::SlotVector outputVals{lookupSlots(ast.nodes[0]->identifiers)};

    auto dirs = parseSortDirList(*ast.nodes[1]);

    const int kBranchesIdx = 2;
    for (size_t idx = 0; idx < ast.nodes[kBranchesIdx]->nodes.size(); idx++) {
        inputKeys.push_back(
            lookupSlots(ast.nodes[kBranchesIdx]->nodes[idx]->nodes[0]->identifiers));
        inputVals.push_back(
            lookupSlots(ast.nodes[kBranchesIdx]->nodes[idx]->nodes[1]->identifiers));
        inputStages.push_back(std::move(ast.nodes[kBranchesIdx]->nodes[idx]->stage));
    }

    uassert(ErrorCodes::FailedToParse,
            "SortedMerge output values and input values mismatch",
            std::all_of(
                inputVals.begin(), inputVals.end(), [size = outputVals.size()](const auto& slots) {
                    return slots.size() == size;
                }));
    uassert(ErrorCodes::FailedToParse,
            "SortedMerge dirs/keys mismatch",
            std::all_of(inputKeys.begin(),
                        inputKeys.end(),
                        [size = dirs.size()](const auto& slots) { return slots.size() == size; }));

    ast.stage = makeS<SortedMergeStage>(std::move(inputStages),
                                        std::move(inputKeys),
                                        std::move(dirs),
                                        std::move(inputVals),
                                        std::move(outputVals),
                                        getCurrentPlanNodeId());
}

void Parser::walkSortedMergeBranch(AstQuery& ast) {
    walkChildren(ast);

    ast.stage = std::move(ast.nodes[2]->stage);
    invariant(ast.stage);
}

void Parser::walkUnwind(AstQuery& ast) {
    walkChildren(ast);

    bool preserveNullAndEmptyArrays = (ast.nodes[3]->token == "true") ? true : false;
    ast.stage = makeS<UnwindStage>(std::move(ast.nodes[4]->stage),
                                   lookupSlotStrict(ast.nodes[2]->identifier),
                                   lookupSlotStrict(ast.nodes[0]->identifier),
                                   lookupSlotStrict(ast.nodes[1]->identifier),
                                   preserveNullAndEmptyArrays,
                                   getCurrentPlanNodeId());
}

void Parser::walkMkObj(AstQuery& ast) {
    using namespace peg::udl;
    using namespace std::literals;

    walkChildren(ast);

    std::string newRootName = ast.nodes[0]->identifier;
    std::string oldRootName;
    std::vector<std::string> fields;
    boost::optional<MakeObjFieldBehavior> fieldBehavior;

    size_t projectListPos = 1;
    size_t forceNewObjPos = 2;
    size_t retOldObjPos = 3;
    size_t inputPos = 4;

    if (ast.nodes.size() != 5) {
        oldRootName = ast.nodes[1]->identifier;
        fields = std::move(ast.nodes[2]->identifiers);
        fieldBehavior = parseFieldBehavior(ast.nodes[3]->token);
        projectListPos = 4;
        forceNewObjPos = 5;
        retOldObjPos = 6;
        inputPos = 7;
    }

    const bool forceNewObj = ast.nodes[forceNewObjPos]->token == "true";
    const bool retOldObj = ast.nodes[retOldObjPos]->token == "true";

    if (ast.tag == "MKOBJ"_) {
        ast.stage =
            makeS<MakeObjStage>(std::move(ast.nodes[inputPos]->stage),
                                lookupSlotStrict(newRootName),
                                lookupSlot(oldRootName),
                                fieldBehavior,
                                std::move(fields),
                                std::move(ast.nodes[projectListPos]->renames),
                                lookupSlots(std::move(ast.nodes[projectListPos]->identifiers)),
                                forceNewObj,
                                retOldObj,
                                getCurrentPlanNodeId());
    } else {
        ast.stage =
            makeS<MakeBsonObjStage>(std::move(ast.nodes[inputPos]->stage),
                                    lookupSlotStrict(newRootName),
                                    lookupSlot(oldRootName),
                                    fieldBehavior,
                                    std::move(fields),
                                    std::move(ast.nodes[projectListPos]->renames),
                                    lookupSlots(std::move(ast.nodes[projectListPos]->identifiers)),
                                    forceNewObj,
                                    retOldObj,
                                    getCurrentPlanNodeId());
    }
}

void Parser::walkGroup(AstQuery& ast) {
    walkChildren(ast);

    size_t collatorSlotPos = 0;
    size_t inputPos = 0;
    // Check if an optional collator slot was provided.
    if (ast.nodes.size() == 4) {
        inputPos = 3;
        collatorSlotPos = 2;
    } else {
        inputPos = 2;
    }

    ast.stage = makeS<HashAggStage>(
        std::move(ast.nodes[inputPos]->stage),
        lookupSlots(std::move(ast.nodes[0]->identifiers)),
        lookupSlots(std::move(ast.nodes[1]->projects)),
        makeSV(),
        true,
        collatorSlotPos ? lookupSlot(std::move(ast.nodes[collatorSlotPos]->identifier))
                        : boost::none,
        true,  // allowDiskUse
        getCurrentPlanNodeId());
}

void Parser::walkHashJoin(AstQuery& ast) {
    walkChildren(ast);

    boost::optional<value::SlotId> collatorSlot;
    auto outerNode = ast.nodes[0];
    auto innerNode = ast.nodes[1];
    if (ast.nodes.size() == 3) {
        outerNode = ast.nodes[1];
        innerNode = ast.nodes[2];
        collatorSlot = lookupSlot(ast.nodes[0]->identifier);
    }

    ast.stage =
        makeS<HashJoinStage>(std::move(outerNode->nodes[2]->stage),          // outer
                             std::move(innerNode->nodes[2]->stage),          // inner
                             lookupSlots(outerNode->nodes[0]->identifiers),  // outer conditions
                             lookupSlots(outerNode->nodes[1]->identifiers),  // outer projections
                             lookupSlots(innerNode->nodes[0]->identifiers),  // inner conditions
                             lookupSlots(innerNode->nodes[1]->identifiers),  // inner projections
                             collatorSlot,                                   // collator
                             getCurrentPlanNodeId());
}

void Parser::walkNLJoin(AstQuery& ast) {
    walkChildren(ast);
    size_t outerPos;
    size_t innerPos;
    std::unique_ptr<EExpression> predicate;

    if (ast.nodes.size() == 5) {
        predicate = std::move(ast.nodes[2]->expr);
        outerPos = 3;
        innerPos = 4;
    } else {
        outerPos = 2;
        innerPos = 3;
    }

    ast.stage = makeS<LoopJoinStage>(std::move(ast.nodes[outerPos]->stage),
                                     std::move(ast.nodes[innerPos]->stage),
                                     lookupSlots(ast.nodes[0]->identifiers),
                                     lookupSlots(ast.nodes[1]->identifiers),
                                     std::move(predicate),
                                     getCurrentPlanNodeId());
}

void Parser::walkLimit(AstQuery& ast) {
    walkChildren(ast);

    ast.stage = makeS<LimitSkipStage>(std::move(ast.nodes[1]->stage),
                                      std::stoi(ast.nodes[0]->token),
                                      boost::none,
                                      getCurrentPlanNodeId());
}

void Parser::walkSkip(AstQuery& ast) {
    walkChildren(ast);

    if (ast.nodes.size() == 3) {
        // This is a case where we have both a 'limit' and a 'skip'.
        ast.stage = makeS<LimitSkipStage>(std::move(ast.nodes[2]->stage),
                                          std::stoi(ast.nodes[0]->token),  // limit
                                          std::stoi(ast.nodes[1]->token),  // skip
                                          getCurrentPlanNodeId());
    } else {
        ast.stage = makeS<LimitSkipStage>(std::move(ast.nodes[1]->stage),
                                          boost::none,
                                          std::stoi(ast.nodes[0]->token),
                                          getCurrentPlanNodeId());
    }
}

void Parser::walkCoScan(AstQuery& ast) {
    walkChildren(ast);

    ast.stage = makeS<CoScanStage>(getCurrentPlanNodeId(), nullptr);
}

void Parser::walkTraverse(AstQuery& ast) {
    walkChildren(ast);
    size_t inPos;
    size_t fromPos;
    size_t foldPos = 0;
    size_t finalPos = 0;
    size_t correlatedPos = 0;

    // The ast for the traverse stage has three nodes of 'inField', 'outField', and 'outFieldInner',
    // two nodes for an 'from' and 'in' child stages, and two nodes for 'foldExpr' and 'finalExpr'.
    // Note that even if one of these expressions is not specified, we output a '{}' token in order
    // to parse the string correctly. This leaves us with two cases: correlated slot vector
    // specified or unspecified. If 'outerCorrelated' is specified, then we have an extra child node
    // and thus eight children, otherwise we are in the original case.
    if (ast.nodes.size() == 8) {
        correlatedPos = 3;
        foldPos = 4;
        finalPos = 5;
        fromPos = 6;
        inPos = 7;
    } else {
        foldPos = 3;
        finalPos = 4;
        fromPos = 5;
        inPos = 6;
    }
    ast.stage = makeS<TraverseStage>(
        std::move(ast.nodes[fromPos]->stage),
        std::move(ast.nodes[inPos]->stage),
        lookupSlotStrict(ast.nodes[2]->identifier),
        lookupSlotStrict(ast.nodes[0]->identifier),
        lookupSlotStrict(ast.nodes[1]->identifier),
        correlatedPos ? lookupSlots(ast.nodes[correlatedPos]->identifiers) : makeSV(),
        std::move(ast.nodes[foldPos]->expr),
        std::move(ast.nodes[finalPos]->expr),
        getCurrentPlanNodeId(),
        boost::none);
}

void Parser::walkExchange(AstQuery& ast) {
    walkChildren(ast);
    ExchangePolicy policy = [&ast] {
        if (ast.nodes[2]->identifier == "round") {
            return ExchangePolicy::roundrobin;
        }

        if (ast.nodes[2]->identifier == "bcast") {
            return ExchangePolicy::broadcast;
        }
        uasserted(4885901, "unknown exchange policy");
    }();
    ast.stage = makeS<ExchangeConsumer>(std::move(ast.nodes[3]->stage),
                                        std::stoll(ast.nodes[1]->token),
                                        lookupSlots(ast.nodes[0]->identifiers),
                                        policy,
                                        nullptr,
                                        nullptr,
                                        getCurrentPlanNodeId());
}

void Parser::walkBranch(AstQuery& ast) {
    walkChildren(ast);

    value::SlotVector outputVals{lookupSlots(ast.nodes[1]->identifiers)};
    value::SlotVector inputThenVals{lookupSlots(ast.nodes[2]->identifiers)};
    value::SlotVector inputElseVals{lookupSlots(ast.nodes[4]->identifiers)};

    ast.stage = makeS<BranchStage>(std::move(ast.nodes[3]->stage),
                                   std::move(ast.nodes[5]->stage),
                                   std::move(ast.nodes[0]->expr),
                                   std::move(inputThenVals),
                                   std::move(inputElseVals),
                                   std::move(outputVals),
                                   getCurrentPlanNodeId());
}

std::unique_ptr<PlanStage> Parser::walkPathValue(AstQuery& ast,
                                                 value::SlotId inputSlot,
                                                 std::unique_ptr<PlanStage> inputStage,
                                                 value::SlotVector correlated,
                                                 value::SlotId outputSlot) {
    using namespace peg::udl;
    using namespace std::literals;

    if (ast.nodes.size() == 1) {
        if (ast.nodes[0]->tag == "EXPR"_) {
            auto [it, inserted] = _symbolsLookupTable.insert({"__self", inputSlot});
            invariant(inserted);
            walk(*ast.nodes[0]);
            _symbolsLookupTable.erase(it);
            return makeProjectStage(std::move(inputStage),
                                    getCurrentPlanNodeId(),
                                    outputSlot,
                                    std::move(ast.nodes[0]->expr));
        } else {
            walk(*ast.nodes[0]);
            return makeProjectStage(
                std::move(inputStage),
                getCurrentPlanNodeId(),
                outputSlot,
                makeE<EFunction>("getField"_sd,
                                 makeEs(makeE<EVariable>(inputSlot),
                                        makeE<EConstant>(ast.nodes[0]->identifier))));
        }
    } else {
        walk(*ast.nodes[0]);
        auto traverseIn = _slotIdGenerator.generate();
        auto from =
            makeProjectStage(std::move(inputStage),
                             getCurrentPlanNodeId(),
                             traverseIn,
                             makeE<EFunction>("getField"_sd,
                                              makeEs(makeE<EVariable>(inputSlot),
                                                     makeE<EConstant>(ast.nodes[0]->identifier))));
        auto in = makeS<LimitSkipStage>(
            makeS<CoScanStage>(getCurrentPlanNodeId()), 1, boost::none, getCurrentPlanNodeId());
        auto stage = makeS<TraverseStage>(
            std::move(from),
            walkPathValue(*ast.nodes[1], traverseIn, std::move(in), {}, outputSlot),
            traverseIn,
            outputSlot,
            outputSlot,
            std::move(correlated),
            nullptr,
            nullptr,
            getCurrentPlanNodeId(),
            boost::none);

        return stage;
    }
}

void Parser::walkSimpleProj(AstQuery& ast) {
    walk(*ast.nodes[0]);
    walk(*ast.nodes[1]);
    walk(*ast.nodes[2]);
    walk(*ast.nodes[4]);

    auto inputStage = std::move(ast.nodes[4]->stage);
    auto outputSlot = lookupSlotStrict(ast.nodes[0]->identifier);
    auto inputSlot = lookupSlotStrict(ast.nodes[1]->identifier);

    ast.stage = walkPathValue(*ast.nodes[3],
                              inputSlot,
                              std::move(inputStage),
                              lookupSlots(ast.nodes[2]->identifiers),
                              outputSlot);
}

bool needNewObject(AstQuery& ast) {
    using namespace peg::udl;

    switch (ast.tag) {
        case "PATH"_: {
            // If anything in the path needs a new object then this path needs a new object.
            bool newObj = false;
            for (const auto& node : ast.nodes) {
                newObj |= needNewObject(*node);
            }
            return newObj;
        }
        case "PF"_: {
            if (ast.nodes.size() == 1) {
                // PF_DROPALL
                return false;
            }

            // Follow the action on the field.
            return needNewObject(*ast.nodes[1]);
        }
        case "PF_ACTION"_: {

            return needNewObject(*ast.nodes[0]);
        }
        case "PF_DROP"_: {
            return false;
        }
        case "PF_INCL"_: {
            return false;
        }
        case "PF_EXPR"_: {
            // This is the only place that forces a new object.
            return true;
        }
        case "PF_MEXPR"_: {
            return false;
        }
        default:;
    }
    MONGO_UNREACHABLE;
}

bool returnOldObject(AstQuery& ast) {
    using namespace peg::udl;

    switch (ast.tag) {
        case "PATH"_: {
            // If anything in the path needs a new object then this path needs a new object.
            bool retOldObj = true;
            for (const auto& node : ast.nodes) {
                retOldObj &= returnOldObject(*node);
            }
            return retOldObj;
        }
        case "PF"_: {
            if (ast.nodes.size() == 1) {
                // PF_DROPALL
                return true;
            }

            // Follow the action on the field.
            return returnOldObject(*ast.nodes[1]);
        }
        case "PF_ACTION"_: {

            return returnOldObject(*ast.nodes[0]);
        }
        case "PF_DROP"_: {
            return true;
        }
        case "PF_INCL"_: {
            return false;
        }
        case "PF_EXPR"_: {
            return false;
        }
        case "PF_MEXPR"_: {
            return true;
        }
        default:;
    }
    MONGO_UNREACHABLE;
}

std::unique_ptr<PlanStage> Parser::walkPath(AstQuery& ast,
                                            value::SlotId inputSlot,
                                            value::SlotId outputSlot) {
    using namespace peg::udl;

    // Do we have to unconditionally create a new object?
    // The algorithm is recursive - if any action anythere is the path needs a new object then the
    // request is bubbled all the way up.
    bool newObj = needNewObject(ast);
    bool restrictAll = false;
    bool retOldObj = returnOldObject(ast);

    std::vector<std::string> fieldNames;
    std::vector<std::string> fieldRestrictNames;
    value::SlotVector fieldVars;
    std::unique_ptr<PlanStage> stage = makeS<LimitSkipStage>(
        makeS<CoScanStage>(getCurrentPlanNodeId()), 1, boost::none, getCurrentPlanNodeId());

    for (size_t idx = ast.nodes.size(); idx-- > 0;) {
        const auto& pf = ast.nodes[idx];
        if (pf->nodes[0]->name == "PF_DROPALL") {
            restrictAll = true;
        } else {
            walk(*pf->nodes[0]);
            const auto& action = *pf->nodes[1];
            switch (action.nodes[0]->tag) {
                case "PF_DROP"_: {
                    fieldRestrictNames.emplace(fieldRestrictNames.begin(),
                                               std::move(pf->nodes[0]->identifier));

                    break;
                }
                case "PF_INCL"_: {
                    fieldNames.emplace(fieldNames.begin(), std::move(pf->nodes[0]->identifier));
                    fieldVars.emplace(fieldVars.begin(), _slotIdGenerator.generate());
                    stage = makeProjectStage(
                        std::move(stage),
                        getCurrentPlanNodeId(),
                        fieldVars.front(),
                        makeE<EFunction>("getField",
                                         makeEs(makeE<EVariable>(inputSlot),
                                                makeE<EConstant>(fieldNames.front()))));
                    break;
                }
                case "PF_EXPR"_: {
                    fieldNames.emplace(fieldNames.begin(), std::move(pf->nodes[0]->identifier));
                    walk(*action.nodes[0]);
                    fieldVars.emplace(fieldVars.begin(), _slotIdGenerator.generate());
                    stage = makeProjectStage(std::move(stage),
                                             getCurrentPlanNodeId(),
                                             fieldVars.front(),
                                             std::move(action.nodes[0]->nodes[0]->expr));
                    break;
                }
                case "PF_MEXPR"_: {
                    auto [it, inserted] = _symbolsLookupTable.insert({"__self", inputSlot});
                    invariant(inserted);

                    fieldNames.emplace(fieldNames.begin(), std::move(pf->nodes[0]->identifier));
                    walk(*action.nodes[0]);
                    fieldVars.emplace(fieldVars.begin(), _slotIdGenerator.generate());
                    stage = makeProjectStage(std::move(stage),
                                             getCurrentPlanNodeId(),
                                             fieldVars.front(),
                                             std::move(action.nodes[0]->nodes[0]->expr));

                    _symbolsLookupTable.erase(it);
                    break;
                }
                case "PATH"_: {
                    fieldNames.emplace(fieldNames.begin(), std::move(pf->nodes[0]->identifier));
                    fieldVars.emplace(fieldVars.begin(), _slotIdGenerator.generate());
                    auto traverseOut = fieldVars.front();
                    auto traverseIn = _slotIdGenerator.generate();
                    stage = makeProjectStage(
                        std::move(stage),
                        getCurrentPlanNodeId(),
                        traverseIn,
                        makeE<EFunction>("getField",
                                         makeEs(makeE<EVariable>(inputSlot),
                                                makeE<EConstant>(fieldNames.front()))));

                    stage =
                        makeS<TraverseStage>(std::move(stage),
                                             walkPath(*action.nodes[0], traverseIn, traverseOut),
                                             traverseIn,
                                             traverseOut,
                                             traverseOut,
                                             sbe::makeSV(),
                                             nullptr,
                                             nullptr,
                                             getCurrentPlanNodeId(),
                                             boost::none);
                    break;
                }
            }
        }
    }

    if (restrictAll) {
        fieldRestrictNames.clear();
        fieldRestrictNames.emplace_back("");
    }
    stage = makeS<MakeObjStage>(std::move(stage),
                                outputSlot,
                                inputSlot,
                                sbe::MakeObjStage::FieldBehavior::drop,
                                std::move(fieldRestrictNames),
                                std::move(fieldNames),
                                std::move(fieldVars),
                                newObj,
                                retOldObj,
                                getCurrentPlanNodeId());

    return stage;
}

void Parser::walkPFO(AstQuery& ast) {
    walk(*ast.nodes[0]);
    walk(*ast.nodes[1]);
    walk(*ast.nodes[2]);
    walk(*ast.nodes[4]);


    ast.stage = makeS<TraverseStage>(std::move(ast.nodes[4]->stage),
                                     walkPath(*ast.nodes[3],
                                              lookupSlotStrict(ast.nodes[1]->identifier),
                                              lookupSlotStrict(ast.nodes[0]->identifier)),
                                     lookupSlotStrict(ast.nodes[1]->identifier),
                                     lookupSlotStrict(ast.nodes[0]->identifier),
                                     lookupSlotStrict(ast.nodes[0]->identifier),
                                     lookupSlots(ast.nodes[2]->identifiers),
                                     nullptr,
                                     nullptr,
                                     getCurrentPlanNodeId(),
                                     boost::none);
}

void Parser::walkLazyProducerSpool(AstQuery& ast) {
    walkChildren(ast);

    std::unique_ptr<EExpression> predicate;
    size_t inputPos;

    if (ast.nodes.size() == 4) {
        predicate = std::move(ast.nodes[2]->expr);
        inputPos = 3;
    } else {
        inputPos = 2;
    }

    ast.stage = makeS<SpoolLazyProducerStage>(std::move(ast.nodes[inputPos]->stage),
                                              lookupSpoolBuffer(ast.nodes[0]->identifier),
                                              lookupSlots(ast.nodes[1]->identifiers),
                                              std::move(predicate),
                                              getCurrentPlanNodeId());
}

void Parser::walkEagerProducerSpool(AstQuery& ast) {
    walkChildren(ast);

    ast.stage = makeS<SpoolEagerProducerStage>(std::move(ast.nodes[2]->stage),
                                               lookupSpoolBuffer(ast.nodes[0]->identifier),
                                               lookupSlots(ast.nodes[1]->identifiers),
                                               getCurrentPlanNodeId());
}

void Parser::walkConsumerSpool(AstQuery& ast) {
    walkChildren(ast);

    ast.stage = makeS<SpoolConsumerStage<false>>(lookupSpoolBuffer(ast.nodes[0]->identifier),
                                                 lookupSlots(ast.nodes[1]->identifiers),
                                                 getCurrentPlanNodeId());
}

void Parser::walkStackConsumerSpool(AstQuery& ast) {
    walkChildren(ast);

    ast.stage = makeS<SpoolConsumerStage<true>>(lookupSpoolBuffer(ast.nodes[0]->identifier),
                                                lookupSlots(ast.nodes[1]->identifiers),
                                                getCurrentPlanNodeId());
}

void Parser::walkPlanNodeId(AstQuery& ast) {
    auto& str = ast.token;
    auto idBegin = str.data() + 1;
    auto idEnd = str.data() + str.length();
    PlanNodeId id = 0;
    auto [ptr, ec] = std::from_chars(idBegin, idEnd, id);
    uassert(5107701, "Invalid plan node id literal.", ec == std::errc());
    planNodeIdStack.push(id);
}

void Parser::walkUnique(AstQuery& ast) {
    walkChildren(ast);

    ast.stage = makeS<UniqueStage>(std::move(ast.nodes[1]->stage),
                                   lookupSlots(ast.nodes[0]->identifiers),
                                   getCurrentPlanNodeId());
}

void Parser::walk(AstQuery& ast) {
    using namespace peg::udl;

    switch (ast.tag) {
        case "OPERATOR"_: {
            walkChildren(ast);
            size_t stageIndex = 0;
            if (ast.nodes.size() == 2) {
                // First child contains plan node id. Stage is stored in the second child.
                stageIndex = 1;
                // Remove current plan node id because it was only relevant to the parsed stage.
                planNodeIdStack.pop();
            }
            ast.stage = std::move(ast.nodes[stageIndex]->stage);
            break;
        }
        case "ROOT"_:
            walkChildren(ast);
            ast.stage = std::move(ast.nodes[0]->stage);
            break;
        case "SCAN"_:
            walkScan(ast);
            break;
        case "PSCAN"_:
            walkParallelScan(ast);
            break;
        case "SEEK"_:
            walkSeek(ast);
            break;
        case "IXSCAN"_:
            walkIndexScan(ast);
            break;
        case "IXSEEK"_:
            walkIndexSeek(ast);
            break;
        case "PROJECT"_:
            walkProject(ast);
            break;
        case "FILTER"_:
            walkFilter(ast);
            break;
        case "CFILTER"_:
            walkCFilter(ast);
            break;
        case "SORT"_:
            walkSort(ast);
            break;
        case "UNION"_:
            walkUnion(ast);
            break;
        case "UNION_BRANCH_LIST"_:
            walkChildren(ast);
            break;
        case "UNION_BRANCH"_:
            walkUnionBranch(ast);
            break;
        case "SORTED_MERGE"_:
            walkSortedMerge(ast);
            break;
        case "SORTED_MERGE_BRANCH_LIST"_:
            walkChildren(ast);
            break;
        case "SORTED_MERGE_BRANCH"_:
            walkSortedMergeBranch(ast);
            break;
        case "UNWIND"_:
            walkUnwind(ast);
            break;
        case "MKOBJ"_:
        case "MKBSON"_:
            walkMkObj(ast);
            break;
        case "GROUP"_:
            walkGroup(ast);
            break;
        case "HJOIN"_:
            walkHashJoin(ast);
            break;
        case "NLJOIN"_:
            walkNLJoin(ast);
            break;
        case "LIMIT"_:
            walkLimit(ast);
            break;
        case "SKIP"_:
            walkSkip(ast);
            break;
        case "COSCAN"_:
            walkCoScan(ast);
            break;
        case "TRAVERSE"_:
            walkTraverse(ast);
            break;
        case "EXCHANGE"_:
            walkExchange(ast);
            break;
        case "IDENT"_:
            walkIdent(ast);
            break;
        case "IDENT_LIST"_:
            walkIdentList(ast);
            break;
        case "IDENT_WITH_RENAME"_:
            walkIdentWithRename(ast);
            break;
        case "IDENT_LIST_WITH_RENAMES"_:
            walkIdentListWithRename(ast);
            break;
        case "IX_KEY_WITH_RENAME"_:
            walkIxKeyWithRename(ast);
            break;
        case "IX_KEY_LIST_WITH_RENAMES"_:
            walkIxKeyListWithRename(ast);
            break;
        case "PROJECT_LIST"_:
            walkProjectList(ast);
            break;
        case "ASSIGN"_:
            walkAssign(ast);
            break;
        case "EXPR"_:
            walkExpr(ast);
            break;
        case "EQOP_EXPR"_:
            walkEqopExpr(ast);
            break;
        case "RELOP_EXPR"_:
            walkRelopExpr(ast);
            break;
        case "ADD_EXPR"_:
            walkAddExpr(ast);
            break;
        case "MUL_EXPR"_:
            walkMulExpr(ast);
            break;
        case "PRIMARY_EXPR"_:
            walkPrimaryExpr(ast);
            break;
        case "IF_EXPR"_:
            walkIfExpr(ast);
            break;
        case "LET_EXPR"_:
            walkLetExpr(ast);
            break;
        case "LAMBDA_EXPR"_:
            walkLambdaExpr(ast);
            break;
        case "FRAME_PROJECT_LIST"_:
            walkFrameProjectList(ast);
            break;
        case "FUN_CALL"_:
            walkFunCall(ast);
            break;
        case "BRANCH"_:
            walkBranch(ast);
            break;
        case "SIMPLE_PROJ"_:
            walkSimpleProj(ast);
            break;
        case "PFO"_:
            walkPFO(ast);
            break;
        case "ESPOOL"_:
            walkEagerProducerSpool(ast);
            break;
        case "LSPOOL"_:
            walkLazyProducerSpool(ast);
            break;
        case "CSPOOL"_:
            walkConsumerSpool(ast);
            break;
        case "SSPOOL"_:
            walkStackConsumerSpool(ast);
            break;
        case "PLAN_NODE_ID"_:
            walkPlanNodeId(ast);
            break;
        case "UNIQUE"_:
            walkUnique(ast);
            break;
        default:
            walkChildren(ast);
    }
}

Parser::Parser(RuntimeEnvironment* env) : _env(env) {
    invariant(_env);

    _parser.log = [&](size_t ln, size_t col, const std::string& msg) {
        LOGV2(4885902, "{msg}", "msg"_attr = format_error_message(ln, col, msg));
    };

    if (!_parser.load_grammar(kSyntax)) {
        uasserted(4885903, "Invalid syntax definition.");
    }

    _parser.enable_packrat_parsing();

    _parser.enable_ast<AstQuery>();
}

std::unique_ptr<PlanStage> Parser::parse(OperationContext* opCtx,
                                         StringData defaultDb,
                                         StringData line) {
    std::shared_ptr<AstQuery> ast;

    _opCtx = opCtx;
    _defaultDb = defaultDb.toString();

    auto result = _parser.parse_n(line.rawData(), line.size(), ast);
    uassert(4885904, str::stream() << "Syntax error in query: " << line, result);

    walk(*ast);
    uassert(4885905, "Query does not have the root.", ast->stage);

    return std::move(ast->stage);
}

UUID Parser::getCollectionUuid(const std::string& collName) {
    if (!_opCtx) {
        // The SBE plan cannot actually run without a valid UUID, but it's useful to allow the
        // parser to run in isolation for unit testing.
        auto uuid = UUID::parse("00000000-0000-0000-0000-000000000000");
        invariant(uuid.isOK());
        return uuid.getValue();
    }

    auto catalog = CollectionCatalog::get(_opCtx);

    // Try to parse the collection name as a UUID directly, otherwise fallback to lookup by
    // NamespaceString.
    auto parsedUuid = UUID::parse(collName);
    if (parsedUuid.isOK()) {
        auto uuid = parsedUuid.getValue();

        // Verify that the UUID corresponds to an existing collection.
        auto collPtr = catalog->lookupCollectionByUUID(_opCtx, uuid);
        uassert(6056700,
                str::stream() << "SBE command parser could not find collection: " << collName,
                collPtr);
        return uuid;
    }

    auto uuid = catalog->lookupUUIDByNSS(_opCtx, NamespaceString{collName});
    uassert(5162900,
            str::stream() << "SBE command parser could not find collection: " << collName,
            uuid);
    return *uuid;
}

PlanNodeId Parser::getCurrentPlanNodeId() {
    if (planNodeIdStack.empty()) {
        return kEmptyPlanNodeId;
    }
    return planNodeIdStack.top();
}

}  // namespace sbe
}  // namespace mongo
