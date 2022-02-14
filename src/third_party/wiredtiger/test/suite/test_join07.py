#!/usr/bin/env python
#
# Public Domain 2014-present MongoDB, Inc.
# Public Domain 2008-2014 WiredTiger, Inc.
#
# This is free and unencumbered software released into the public domain.
#
# Anyone is free to copy, modify, publish, use, compile, sell, or
# distribute this software, either in source code form or as a compiled
# binary, for any purpose, commercial or non-commercial, and by any
# means.
#
# In jurisdictions that recognize copyright laws, the author or authors
# of this software dedicate any and all copyright interest in the
# software to the public domain. We make this dedication for the benefit
# of the public at large and to the detriment of our heirs and
# successors. We intend this dedication to be an overt act of
# relinquishment in perpetuity of all present and future rights to this
# software under copyright law.
#
# THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
# EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
# MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.
# IN NO EVENT SHALL THE AUTHORS BE LIABLE FOR ANY CLAIM, DAMAGES OR
# OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE,
# ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR
# OTHER DEALINGS IN THE SOFTWARE.

import re
import wttest
from wtscenario import make_scenarios

class ParseException(Exception):
    def __init__(self, msg):
        super(ParseException, self).__init__(msg)

class Token:
    UNKNOWN = '<unknown>'
    NUMBER = 'Number'
    STRING = 'String'
    COLUMN = 'Column'
    LPAREN = '('
    RPAREN = ')'
    LBRACKET = '{'
    RBRACKET = '}'
    COMMA = ','
    OR = '||'
    AND = '&&'
    LT = '<'
    GT = '>'
    LE = '<='
    GE = '>='
    EQ = '=='
    ATTRIBUTE = 'Attribute'  # bracketed key value pair

    COMPARE_OPS = [LT, GT, LE, GE, EQ]
    COMPARATORS = [NUMBER, STRING]

    def __init__(self, kind, tokenizer):
        self.kind = kind
        self.pos = tokenizer.off + tokenizer.pos
        self.n = 0
        self.s = ''
        self.index = ''
        self.attr_key = ''
        self.attr_value = ''
        self.groups = None

    def __str__(self):
        return '<Token ' + self.kind + ' at char ' + str(self.pos) + '>'

class Tokenizer:
    def __init__(self, s):
        self.off = 0
        self.s = s + '?'  # add a char that won't match anything
        self.pos = 0
        self.end = len(s)
        self.re_num = re.compile(r"(\d+)")
        self.re_quote1 = re.compile(r"'([^']*)'")
        self.re_quote2 = re.compile(r"\"([^\"]*)\"")
        self.re_attr = re.compile(r"\[(\w+)=(\w+)\]")
        self.pushed = None

    def newToken(self, kind, sz):
        t = Token(kind, self)
        self.pos += sz
        return t

    def error(self, s):
        raise ParseException(str(self.pos) + ': ' + s)

    def matched(self, kind, repat):
        pos = self.pos
        match = re.match(repat, self.s[pos:])
        if not match:
            end = pos + 10
            if end > self.end:
                end = self.end
            self.error('matching ' + kind + ' at "' +
                       self.s[pos:end] + '..."')
        t = self.newToken(kind, match.end())
        t.groups = match.groups()
        t.s = self.s[pos:pos + match.end()]
        return t

    def available(self):
        if self.pushed == None:
            self.pushback(self.token())
        return (self.pushed != None)

    def pushback(self, token):
        if self.pushed != None:
            raise AssertionError('pushback more than once')
        self.pushed = token

    def peek(self):
        token = self.token()
        self.pushback(token)
        return token

    def scan(self):
        while self.pos < self.end and self.s[self.pos].isspace():
            self.pos += 1
        return '' if self.pos >= self.end else self.s[self.pos]

    def token(self):
        if self.pushed != None:
            ret = self.pushed
            self.pushed = None
            return ret
        c = self.scan()
        if self.pos >= self.end:
            return None
        lookahead = '' if self.pos + 1 >= self.end else self.s[self.pos+1]
        #self.tty("Tokenizer.token char=" + c + ", lookahead=" + lookahead)
        if c == "'":
            t = self.matched(Token.STRING, self.re_quote1)
            t.s = t.groups[0]
            return t
        if c == '"':
            t = self.matched(Token.STRING, self.re_quote2)
            t.s = t.groups[0]
            return t
        if c in "{}(),":
            return self.newToken(c, 1)
        if c == "|":
            if lookahead != "|":
                self.error('matching OR')
            return self.newToken(Token.OR, 2)
        if c == "&":
            if lookahead != "&":
                self.error('matching AND')
            return self.newToken(Token.AND, 2)
        if c in "0123456789":
            t = self.matched(Token.NUMBER, self.re_num)
            t.s = t.groups[0]
            t.n = int(t.s)
            return t
        if c in "ABCDEFGHIJ":
            t = self.newToken(Token.COLUMN, 1)
            t.s = c
            return t
        if c == '<':
            if lookahead == '=':
                return self.newToken(Token.LE, 2)
            else:
                return self.newToken(Token.LT, 1)
        if c == '>':
            if lookahead == '=':
                return self.newToken(Token.GE, 2)
            else:
                return self.newToken(Token.GT, 1)
        if c in "=":
            if lookahead != "=":
                self.error('matching EQ')
            return self.newToken(Token.EQ, 2)
        if c in "[":
            t = self.matched(Token.ATTRIBUTE, self.re_attr)
            t.attr_key = t.groups[0]
            t.attr_value = t.groups[1]
            return t
        return None

    def tty(self, s):
        wttest.WiredTigerTestCase.tty(s)

# test_join07.py
#    Join interpreter
class test_join07(wttest.WiredTigerTestCase):
    reverseop = { '==' : '==', '<=' : '>=', '<' : '>', '>=' : '<=', '>' : '<' }
    compareop = { '==' : 'eq', '<=' : 'le', '<' : 'lt', '>=' : 'ge',
                  '>' : 'gt' }
    columnmult = { 'A' : 1,  'B' : 2,  'C' : 3,  'D' : 4,  'E' : 5,
                   'F' : 6,  'G' : 7,  'H' : 8,  'I' : 9,  'J' : 10 }

    extractscen = [
        ('extractor', dict(extractor=True)),
        ('noextractor', dict(extractor=False))
    ]

    scenarios = make_scenarios(extractscen)

    def conn_extensions(self, extlist):
        extlist.skip_if_missing = True
        extlist.extension('extractors', 'csv')

    def expect(self, token, expected):
        if token == None or token.kind not in expected:
            self.err(token, 'expected one of: ' + str(expected))
        return token

    def err(self, token, msg):
        self.assertTrue(False, 'ERROR at token ' + str(token) + ': ' + msg)

    def gen_key(self, i):
        if self.keyformat == 'S':
            return [ 'key%06d' % i ]  # zero pad so it sorts expectedly
        else:
            return [ i ]

    def gen_values(self, i):
        s = ""
        ret = []
        for x in range(1, 11):
            v = (i * x) % self.N
            if x <= 5:
                ret.append(v)
            else:
                ret.append(str(v))
            if s != "":
                s += ","
            s += str(v)
        ret.insert(0, s)
        return ret

    def iterate(self, jc, mbr):
        mbr = set(mbr)   # we need a mutable set
        gotkeys = []
        #self.tty('iteration expects ' + str(len(mbr)) +
        #         ' entries: ' + str(mbr))
        while jc.next() == 0:
            [k] = jc.get_keys()
            values = jc.get_values()
            if self.keyformat == 'S':
                i = int(str(k[3:]))
            else:
                i = k
            #self.tty('GOT key=' + str(k) + ', values=' + str(values))

            # Duplicates may be returned when the disjunctions are used,
            # so we ignore them.
            if not i in gotkeys:
                self.assertEquals(self.gen_values(i), values)
                if not i in mbr:
                    self.tty('ERROR: result ' + str(i) + ' is not in: ' +
                             str(mbr))
                    self.assertTrue(i in mbr)
                mbr.remove(i)
                gotkeys.append(i)
        self.assertEquals(0, len(mbr))

    def token_literal(self, token):
        if token.kind == Token.STRING:
            return token.s
        elif token.kind == Token.NUMBER:
            return token.n

    def idx_sim(self, x, mult, isstr):
        if isstr:
            return str(int(x) * mult % self.N)
        else:
            return (x * mult % self.N)

    def mkmbr(self, expr):
        return frozenset([x for x in self.allN if expr(x)])

    def join_one_side(self, jc, coltok, littok, optok, conjunction,
                      isright, mbr):
        idxname = 'index:join07:' + coltok.s
        cursor = self.session.open_cursor(idxname, None, None)
        jc.cursors.append(cursor)
        literal = self.token_literal(littok)
        cursor.set_key(literal)
        searchret = cursor.search()
        if searchret != 0:
            self.tty('ERROR: cannot find value ' + str(literal) +
                     ' in ' + idxname)
        self.assertEquals(0, searchret)
        op = optok.kind
        if not isright:
            op = self.reverseop[op]
        mult = self.columnmult[coltok.s]
        config = 'compare=' + self.compareop[op] + ',operation=' + \
                 ('and' if conjunction else 'or')
        if hasattr(coltok, 'bloom'):
            config += ',strategy=bloom,count=' + str(coltok.bloom)
        #self.tty('join(jc, cursor=' + str(literal) + ', ' + config)
        self.session.join(jc, cursor, config)
        isstr = type(literal) is str
        if op == '==':
            tmbr = self.mkmbr(lambda x: self.idx_sim(x, mult, isstr) == literal)
        elif op == '<=':
            tmbr = self.mkmbr(lambda x: self.idx_sim(x, mult, isstr) <= literal)
        elif op == '<':
            tmbr = self.mkmbr(lambda x: self.idx_sim(x, mult, isstr) < literal)
        elif op == '>=':
            tmbr = self.mkmbr(lambda x: self.idx_sim(x, mult, isstr) >= literal)
        elif op == '>':
            tmbr = self.mkmbr(lambda x: self.idx_sim(x, mult, isstr) > literal)
        if conjunction:
            mbr = mbr.intersection(tmbr)
        else:
            mbr = mbr.union(tmbr)
        return mbr

    def parse_join(self, jc, tokenizer, conjunction, mbr):
        left = None
        right = None
        leftop = None
        rightop = None
        col = None
        token = tokenizer.token()
        if token.kind == Token.LPAREN:
            subjc = self.session.open_cursor('join:table:join07', None, None)
            jc.cursors.append(subjc)
            submbr = self.parse_junction(subjc, tokenizer)
            config = 'operation=' + ('and' if conjunction else 'or')
            self.session.join(jc, subjc, config)
            if conjunction:
                mbr = mbr.intersection(submbr)
            else:
                mbr = mbr.union(submbr)
            return mbr
        if token.kind in Token.COMPARATORS:
            left = token
            leftop = self.expect(tokenizer.token(), Token.COMPARE_OPS)
            token = tokenizer.token()
        col = self.expect(token, [Token.COLUMN])
        token = tokenizer.token()
        if token.kind in Token.ATTRIBUTE:
            tokenizer.pushback(token)
            self.parse_column_attributes(tokenizer, col)
            token = tokenizer.token()
        if token.kind in Token.COMPARE_OPS:
            rightop = token
            right = self.expect(tokenizer.token(), Token.COMPARATORS)
            token = tokenizer.token()
        tokenizer.pushback(token)

        # Now we have everything we need to do a join.
        if left != None:
            mbr = self.join_one_side(jc, col, left, leftop, conjunction,
                                     False, mbr)
        if right != None:
            mbr = self.join_one_side(jc, col, right, rightop, conjunction,
                                     True, mbr)
        return mbr

    # Parse a set of joins, grouped by && or ||
    def parse_junction(self, jc, tokenizer):
        jc.cursors = []

        # Take a peek at the tokenizer's stream to see if we
        # have a conjunction or disjunction
        token = tokenizer.peek()
        s = tokenizer.s[token.pos:]
        (andpos, orpos) = self.find_nonparen(s, ['&', '|'])
        if orpos >= 0 and (andpos < 0 or orpos < andpos):
            conjunction = False
            mbr = frozenset()
        else:
            conjunction = True
            mbr = frozenset(self.allN)

        while tokenizer.available():
            mbr = self.parse_join(jc, tokenizer, conjunction, mbr)
            token = tokenizer.token()
            if token != None:
                if token.kind == Token.OR:
                    self.assertTrue(not conjunction)
                elif token.kind == Token.AND:
                    self.assertTrue(conjunction)
                elif token.kind == Token.RPAREN:
                    break
                else:
                    self.err(token, 'unexpected token')
        return mbr

    def parse_attributes(self, tokenizer):
        attributes = []
        token = tokenizer.token()
        while token != None and token.kind == Token.ATTRIBUTE:
            attributes.append(token)
            token = tokenizer.token()
        tokenizer.pushback(token)
        return attributes

    # Find a set of chars that aren't within parentheses.
    # For this simple language, we don't allow parentheses in quoted literals.
    def find_nonparen(self, s, matchlist):
        pos = 0
        end = len(s)
        nmatch = len(matchlist)
        nfound = 0
        result = [-1 for i in range(0, nmatch)]
        parennest = 0
        while pos < end and nfound < nmatch:
            c = s[pos]
            if c == '(':
                parennest += 1
            elif c == ')':
                parennest -= 1
                if parennest < 0:
                    break
            elif parennest == 0 and c in matchlist:
                m = matchlist.index(c)
                if result[m] < 0:
                    result[m] = pos
                    nfound += 1
            pos += 1
        return result

    def parse_toplevel(self, jc, tokenizer):
        return self.parse_junction(jc, tokenizer)

    def parse_toplevel_attributes(self, tokenizer):
        for attrtoken in self.parse_attributes(tokenizer):
            key = attrtoken.attr_key
            value = attrtoken.attr_value
            #self.tty('ATTR:' + str([key,value]))
            if key == 'N':
                self.N = int(value)
            elif key == 'key':
                self.keyformat = value
            else:
                tokenizer.error('bad attribute key: ' + str(key))

    def parse_column_attributes(self, tokenizer, c):
        for attrtoken in self.parse_attributes(tokenizer):
            key = attrtoken.attr_key
            value = attrtoken.attr_value
            #self.tty('ATTR:' + str([key,value]))
            if key == 'bloom':
                c.bloom = int(value)
            else:
                tokenizer.error('bad column attribute key: ' + str(key))

    def close_cursors(self, jc):
        jc.close()
        for c in jc.cursors:
            if c.uri[0:5] == 'join:':
                self.close_cursors(c)
            else:
                c.close()

    def interpret(self, s):
        #self.tty('INTERPRET: ' + s)
        self.N = 1000
        self.keyformat = "r"
        self.keycols = 'k'

        # Grab attributes before creating anything, as some attributes
        # may override needed parameters.
        tokenizer = Tokenizer(s)
        self.parse_toplevel_attributes(tokenizer)
        self.allN = range(1, self.N + 1)

        self.session.create('table:join07', 'key_format=' + self.keyformat +
                            ',value_format=SiiiiiSSSSS,' +
                            'columns=(' + self.keycols +
                            ',S,A,B,C,D,E,F,G,H,I,J)')
        mdfieldnum = 0
        mdformat = 'i'
        mdconfig = ''
        for colname in [ 'A','B','C','D','E','F','G','H','I','J' ]:
            if self.extractor:
                if colname == 'F':
                    mdformat = 'S'
                mdconfig = 'app_metadata={"format" : "%s","field" : "%d"}' % \
                           (mdformat, mdfieldnum)
                config = 'extractor=csv,key_format=%s' % mdformat
                mdfieldnum += 1
            else:
                config = 'columns=(%s)' % colname
            self.session.create('index:join07:%s' % colname,
                                '%s,%s' % (config, mdconfig))
        c = self.session.open_cursor('table:join07', None, None)
        for i in self.allN:
            c.set_key(*self.gen_key(i))
            c.set_value(*self.gen_values(i))
            c.insert()
        c.close()

        jc = self.session.open_cursor('join:table:join07', None, None)
        mbr = self.parse_toplevel(jc, tokenizer)
        self.iterate(jc, mbr)

        self.close_cursors(jc)
        self.session.drop('table:join07')

    def test_join_string(self):
        self.interpret("[N=1000][key=r] 7 < A <= 500 && B < 150 && C > 17")
        self.interpret("[N=1001][key=r] 7 < A <= 500 && B < 150 && F > '234'")
        self.interpret("[N=10000][key=r] 7 < A <= 500 && B < 150 && " +
                       "(F > '234' || G < '100')")
        self.interpret("[N=7919][key=r](7 < A <= 9)&&(F > '234')")
        self.interpret("[N=1000][key=S](A>=0 && A<0)||(A>999)")
        self.interpret("[N=2000][key=S](A>=0 && A<0)||(A>1999)")
        self.interpret("(7<A<=10 && B < 150)||(B>998)")
        self.interpret("(7<A<=10 && B < 150)||(J=='990')")
        clause1 = "(7 < A <= 500 && B < 150)"
        clause2 = "(F > '234' || G < '100')"
        self.interpret("[N=1000][key=r]" + clause1 + "&&" + clause2)
        self.interpret("(7<A<=10)||(B>994||C<12)")
        self.interpret("(7<A<=10 && B < 150)||(B>996||C<6)")
        self.interpret("[N=1000][key=r]" + clause2 + "||" + clause1)
        self.interpret("[N=1000][key=r]" + clause1 + "||" + clause2)
        self.interpret("[N=1000][key=S]" + clause2 + "&&" + clause1)
        clause1 = "(7 < A <= 500 && B[bloom=300] < 150)"
        clause2 = "(F[bloom=500] > '234' || G[bloom=20] < '100')"
        self.interpret("[N=1000][key=S]" + clause1 + "&&" + clause2)

if __name__ == '__main__':
    wttest.run()
