/**
 * Copyright (C) 2015 MongoDB Inc.
 *
 * This program is free software: you can redistribute it and/or  modify
 * it under the terms of the GNU Affero General Public License, version 3,
 * as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Affero General Public License for more details.
 *
 * You should have received a copy of the GNU Affero General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 * As a special exception, the copyright holders give permission to link the
 * code of portions of this program with the OpenSSL library under certain
 * conditions as described in each individual source file and distribute
 * linked combinations including the program with the OpenSSL library. You
 * must comply with the GNU Affero General Public License in all respects
 * for all of the code used other than as permitted herein. If you modify
 * file(s) with this exception, you may extend this exception to your
 * version of the file(s), but you are not obligated to do so. If you do not
 * wish to do so, delete this exception statement from your version. If you
 * delete this exception statement from all source files in the program,
 * then also delete it in the license file.
 */

// This file has JavaScript functions that should be attached to the MongoHelpers object

// The contents of exportToMongoHelpers will be copied into the MongoHelpers object and
// this dictionary will be removed from the global scope.
exportToMongoHelpers = {
    // This function accepts an expression or function body and returns a function definition
    'functionExpressionParser': function functionExpressionParser(fnSrc) {
        var parseTree;
        try {
            parseTree = this.Reflect.parse(fnSrc);
        } catch (e) {
            if (e == 'SyntaxError: function statement requires a name') {
                return fnSrc;
            } else if (e == 'SyntaxError: return not in function') {
                return 'function() { ' + fnSrc + ' }';
            } else {
                throw(e);
            }
        }
        // Input source is a series of expressions. we should prepend the last one with return
        var lastStatement = parseTree.body.length - 1;
        var lastStatementType = parseTree.body[lastStatement].type;
        if (lastStatementType == 'ExpressionStatement') {
            var prevExprEnd = 0;
            var loc = parseTree.body[lastStatement].loc.start;

            // When we're actually doing the pre-pending of return later on we need to know
            // whether we've reached the beginning of the line, or the end of the 2nd-to-last
            // expression.
            if (lastStatement > 0) {
                prevExprEnd = parseTree.body[lastStatement - 1].loc.end;
                if (prevExprEnd.line != loc.line) {
                    prevExprEnd = 0;
                } else {
                    prevExprEnd = prevExprEnd.column;
                }
            }

            var lines = fnSrc.split('\n');
            var col = loc.column;
            var fnSrc;
            var origLine = lines[loc.line - 1];

            // The parser has a weird behavior where sometimes if you have an expression like
            // ((x == 5)), it says that the expression string is "x == 5))", so we may need to
            // adjust where we prepend "return".
            while (col >= prevExprEnd) {
                var modLine = origLine.substr(0, col) + "return " + origLine.substr(col);
                lines[loc.line - 1] = modLine;
                fnSrc = '{ ' + lines.join('\n') + ' }';
                try {
                    tmpTree = this.Reflect.parse("function x() " + fnSrc);
                } catch (e) {
                    col -= 1;
                    continue;
                }
                break;
            }

            return "function() " + fnSrc;
        } else if (lastStatementType == 'FunctionDeclaration') {
            return fnSrc;
        } else {
            return 'function() { ' + fnSrc + ' }';
        }
    }
};

// WARNING: Anything outside the exportToMongoHelpers dictionary will be available in the
// global scope and visible to users!
