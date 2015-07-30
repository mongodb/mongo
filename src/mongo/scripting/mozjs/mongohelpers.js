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
        } catch(e) {
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
            var lines = fnSrc.split('\n');
            var loc = parseTree.body[lastStatement].loc.start;
            var mod_line = lines[loc.line - 1];
            mod_line = mod_line.substr(0, loc.column) + 'return ' + mod_line.substr(loc.column);
            lines[loc.line - 1] = mod_line;
            fnSrc = 'function() { ' + lines.join('\n') + ' }'
            return fnSrc;
        } else if(lastStatementType == 'FunctionDeclaration') {
            return fnSrc;
        } else {
            return 'function() { ' + fnSrc + ' }';
        }
    }
}

// WARNING: Anything outside the exportToMongoHelpers dictionary will be available in the
// global scope and visible to users!
