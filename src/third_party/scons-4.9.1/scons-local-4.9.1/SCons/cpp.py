# MIT License
#
# Copyright The SCons Foundation
#
# Permission is hereby granted, free of charge, to any person obtaining
# a copy of this software and associated documentation files (the
# "Software"), to deal in the Software without restriction, including
# without limitation the rights to use, copy, modify, merge, publish,
# distribute, sublicense, and/or sell copies of the Software, and to
# permit persons to whom the Software is furnished to do so, subject to
# the following conditions:
#
# The above copyright notice and this permission notice shall be included
# in all copies or substantial portions of the Software.
#
# THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY
# KIND, EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE
# WARRANTIES OF MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
# NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE
# LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION
# OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION
# WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.

"""SCons C Pre-Processor module"""

import os
import re

import SCons.Util

# First "subsystem" of regular expressions that we set up:
#
# Stuff to turn the C preprocessor directives in a file's contents into
# a list of tuples that we can process easily.
#
# A table of regular expressions that fetch the arguments from the rest of
# a C preprocessor line.  Different directives have different arguments
# that we want to fetch, using the regular expressions to which the lists
# of preprocessor directives map.
cpp_lines_dict = {
    # Fetch the rest of a #if/#elif as one argument,
    # with white space optional.
    ('if', 'elif')      : r'\s*(.+)',

    # Fetch the rest of a #ifdef/#ifndef as one argument,
    # separated from the keyword by white space.
    ('ifdef', 'ifndef',): r'\s+(.+)',

    # Fetch the rest of a #import/#include/#include_next line as one
    # argument, with white space optional.
    ('import', 'include', 'include_next',)
                        : r'\s*(.+)',

    # We don't care what comes after a #else or #endif line.
    ('else', 'endif',)  : '',

    # Fetch three arguments from a #define line:
    #   1) The #defined keyword.
    #   2) The optional parentheses and arguments (if it's a function-like
    #      macro, '' if it's not).
    #   3) The expansion value.
    ('define',)         : r'\s+([_A-Za-z][_A-Za-z0-9_]*)(\([^)]*\))?\s*(.*)',

    # Fetch the #undefed keyword from a #undef line.
    ('undef',)          : r'\s+([_A-Za-z][A-Za-z0-9_]*)',
}

# Create a table that maps each individual C preprocessor directive to
# the corresponding compiled regular expression that fetches the arguments
# we care about.
Table = {}
for op_list, expr in cpp_lines_dict.items():
    e = re.compile(expr)
    for op in op_list:
        Table[op] = e
del e
del op
del op_list

# Create a list of the expressions we'll use to match all of the
# preprocessor directives.  These are the same as the directives
# themselves *except* that we must use a negative lookahead assertion
# when matching "if" so it doesn't match the "if" in "ifdef" or "ifndef".
override = {
    'if'                        : 'if(?!n?def)',
}
l = [override.get(x, x) for x in Table.keys()]


# Turn the list of expressions into one big honkin' regular expression
# that will match all the preprocessor lines at once.  This will return
# a list of tuples, one for each preprocessor line.  The preprocessor
# directive will be the first element in each tuple, and the rest of
# the line will be the second element.
e = r'^\s*#\s*(' + '|'.join(l) + ')(.*)$'

# And last but not least, compile the expression.
CPP_Expression = re.compile(e, re.M)

# A list with RE to cleanup CPP Expressions (tuples)
# We should remove all comments and carriage returns (\r) before evaluating
CPP_Expression_Cleaner_List = [
    r"/\*.*\*/",
    r"/\*.*",
    r"//.*",
    r"\r"
]
CPP_Expression_Cleaner_RE = re.compile(
    r"\s*(" + "|".join(CPP_Expression_Cleaner_List) + ")")

def Cleanup_CPP_Expressions(ts):
    return [(t[0], CPP_Expression_Cleaner_RE.sub("", t[1])) for t in ts]

#
# Second "subsystem" of regular expressions that we set up:
#
# Stuff to translate a C preprocessor expression (as found on a #if or
# #elif line) into an equivalent Python expression that we can eval().
#

# A dictionary that maps the C representation of Boolean operators
# to their Python equivalents.
CPP_to_Python_Ops_Dict = {
    '!'         : ' not ',
    '!='        : ' != ',
    '&&'        : ' and ',
    '||'        : ' or ',
    '?'         : ' and ',
    ':'         : ' or ',
}

CPP_to_Python_Ops_Sub = lambda m: CPP_to_Python_Ops_Dict[m.group(0)]

# We have to sort the keys by length so that longer expressions
# come *before* shorter expressions--in particular, "!=" must
# come before "!" in the alternation.  Without this, the Python
# re module, as late as version 2.2.2, empirically matches the
# "!" in "!=" first, instead of finding the longest match.
# What's up with that?
l = sorted(list(CPP_to_Python_Ops_Dict.keys()), key=lambda a: len(a), reverse=True)

# Turn the list of keys into one regular expression that will allow us
# to substitute all of the operators at once.
expr = '|'.join(map(re.escape, l))

# ...and compile the expression.
CPP_to_Python_Ops_Expression = re.compile(expr)

# integer specifications
int_suffix_opt = r'(?:[uU](?:l{1,2}|L{1,2}|[zZ]|wb|WB)?|(?:l{1,2}|L{1,2}|[zZ]|wb|WB)[uU]?)?'

hex_integer = fr'(0[xX][0-9A-Fa-f]+){int_suffix_opt}'
bin_integer = fr'(0[bB][01]+){int_suffix_opt}'
oct_integer = fr'(0[0-7]*){int_suffix_opt}'
dec_integer = fr'([1-9][0-9]*){int_suffix_opt}'

int_boundary = r'[a-zA-Z0-9_]'
int_neg_lookbehind = fr'(?<!{int_boundary})'
int_neg_lookahead = fr'(?!{int_boundary})'

# A separate list of expressions to be evaluated and substituted
# sequentially, not all at once.
CPP_to_Python_Eval_List = [
    [r'defined\s+(\w+)', '"\\1" in __dict__'],
    [r'defined\s*\((\w+)\)', '"\\1" in __dict__'],
    [fr'{int_neg_lookbehind}{hex_integer}{int_neg_lookahead}', '\\1'],
    [fr'{int_neg_lookbehind}{bin_integer}{int_neg_lookahead}', '\\1'],
    [fr'{int_neg_lookbehind}{oct_integer}{int_neg_lookahead}', '0o\\1'],
    [fr'{int_neg_lookbehind}{dec_integer}{int_neg_lookahead}', '\\1'],
]

# Replace the string representations of the regular expressions in the
# list with compiled versions.
for l in CPP_to_Python_Eval_List:
    l[0] = re.compile(l[0])

# Wrap up all of the above into a handy function.
def CPP_to_Python(s):
    """
    Converts a C pre-processor expression into an equivalent
    Python expression that can be evaluated.
    """
    s = CPP_to_Python_Ops_Expression.sub(CPP_to_Python_Ops_Sub, s)
    for expr, repl in CPP_to_Python_Eval_List:
        s = re.sub(expr, repl, s)
    return s



del expr
del l
del override



class FunctionEvaluator:
    """Handles delayed evaluation of a #define function call."""

    def __init__(self, name, args, expansion) -> None:
        """
        Squirrels away the arguments and expansion value of a #define
        macro function for later evaluation when we must actually expand
        a value that uses it.
        """
        self.name = name
        self.args = function_arg_separator.split(args)
        try:
            expansion = expansion.split('##')
        except AttributeError:
            pass
        self.expansion = expansion

    def __call__(self, *values):
        """
        Evaluates the expansion of a #define macro function called
        with the specified values.
        """
        if len(self.args) != len(values):
            raise ValueError("Incorrect number of arguments to `%s'" % self.name)
        # Create a dictionary that maps the macro arguments to the
        # corresponding values in this "call."  We'll use this when we
        # eval() the expansion so that arguments will get expanded to
        # the right values.
        args = self.args
        localvars = {k: v for k, v in zip(args, values)}
        parts = [s if s in args else repr(s) for s in self.expansion]
        statement = ' + '.join(parts)

        return eval(statement, globals(), localvars)


# Find line continuations.
line_continuations = re.compile('\\\\\r?\n')

# Search for a "function call" macro on an expansion.  Returns the
# two-tuple of the "function" name itself, and a string containing the
# arguments within the call parentheses.
function_name = re.compile(r'(\S+)\(([^)]*)\)')

# Split a string containing comma-separated function call arguments into
# the separate arguments.
function_arg_separator = re.compile(r',\s*')



class PreProcessor:
    """The main workhorse class for handling C pre-processing."""

    def __init__(self, current=os.curdir, cpppath=(), dict={}, all: int=0, depth=-1) -> None:
        global Table

        cpppath = tuple(cpppath)

        self.searchpath = {
            '"': (current,) + cpppath,
            '<': cpppath + (current,),
        }

        # Initialize our C preprocessor namespace for tracking the
        # values of #defined keywords.  We use this namespace to look
        # for keywords on #ifdef/#ifndef lines, and to eval() the
        # expressions on #if/#elif lines (after massaging them from C to
        # Python).
        self.cpp_namespace = dict.copy()
        self.cpp_namespace['__dict__'] = self.cpp_namespace

        # Namespace for constant expression evaluation (literals only)
        self.constant_expression_namespace = {}

        # Return all includes without resolving
        if all:
            self.do_include = self.all_include

        # Max depth of nested includes:
        # -1 = unlimited
        # 0 - disabled nesting
        # >0 - number of allowed nested includes
        self.depth = depth

        # For efficiency, a dispatch table maps each C preprocessor
        # directive (#if, #define, etc.) to the method that should be
        # called when we see it.  We accomodate state changes (#if,
        # #ifdef, #ifndef) by pushing the current dispatch table on a
        # stack and changing what method gets called for each relevant
        # directive we might see next at this level (#else, #elif).
        # #endif will simply pop the stack.
        d = {'scons_current_file': self.scons_current_file}
        for op in Table.keys():
            d[op] = getattr(self, 'do_' + op)
        self.default_table = d

    def __call__(self, file):
        """
        Pre-processes a file.

        This is the main public entry point.
        """
        self.current_file = file
        return self.process_file(file)

    def process_file(self, file):
        """
        Pre-processes a file.

        This is the main internal entry point.
        """
        return self._process_tuples(self.tupleize(self.read_file(file)), file)

    def process_contents(self, contents):
        """
        Pre-processes a file contents.

        Is used by tests
        """
        return self._process_tuples(self.tupleize(contents))

    def _process_tuples(self, tuples, file=None):
        self.stack = []
        self.dispatch_table = self.default_table.copy()
        self.current_file = file
        self.tuples = tuples

        self.initialize_result(file)
        while self.tuples:
            t = self.tuples.pop(0)
            # Uncomment to see the list of tuples being processed (e.g.,
            # to validate the CPP lines are being translated correctly).
            # print(t)
            self.dispatch_table[t[0]](t)
        return self.finalize_result(file)

    def tupleize(self, contents):
        """
        Turns the contents of a file into a list of easily-processed
        tuples describing the CPP lines in the file.

        The first element of each tuple is the line's preprocessor
        directive (#if, #include, #define, etc., minus the initial '#').
        The remaining elements are specific to the type of directive, as
        pulled apart by the regular expression.
        """
        return self._match_tuples(self._parse_tuples(contents))

    def _parse_tuples(self, contents):
        global CPP_Expression
        contents = line_continuations.sub('', contents)
        tuples = CPP_Expression.findall(contents)
        return Cleanup_CPP_Expressions(tuples)

    def _match_tuples(self, tuples):
        global Table
        result = []
        for t in tuples:
            m = Table[t[0]].match(t[1])
            if m:
                result.append((t[0],) + m.groups())
        return result

    # Dispatch table stack manipulation methods.

    def save(self) -> None:
        """
        Pushes the current dispatch table on the stack and re-initializes
        the current dispatch table to the default.
        """
        self.stack.append(self.dispatch_table)
        self.dispatch_table = self.default_table.copy()

    def restore(self) -> None:
        """
        Pops the previous dispatch table off the stack and makes it the
        current one.
        """
        try: self.dispatch_table = self.stack.pop()
        except IndexError: pass

    # Utility methods.

    def do_nothing(self, t) -> None:
        """
        Null method for when we explicitly want the action for a
        specific preprocessor directive to do nothing.
        """
        pass

    def scons_current_file(self, t) -> None:
        self.current_file = t[1]

    def eval_constant_expression(self, s):
        """
        Evaluates a C preprocessor expression.

        This is done by converting it to a Python equivalent and
        eval()ing it in the C preprocessor namespace we use to
        track #define values.

        Returns None if the eval() result is not an integer.
        """
        s = CPP_to_Python(s)
        try:
            rval = eval(s, self.constant_expression_namespace)
        except (NameError, TypeError, SyntaxError) as e:
            rval = None
        if not isinstance(rval, int):
            rval = None
        return rval

    def eval_expression(self, t):
        """
        Evaluates a C preprocessor expression.

        This is done by converting it to a Python equivalent and
        eval()ing it in the C preprocessor namespace we use to
        track #define values.
        """
        t = CPP_to_Python(' '.join(t[1:]))
        try:
            return eval(t, self.cpp_namespace)
        except (NameError, TypeError, SyntaxError):
            return 0

    def initialize_result(self, fname) -> None:
        self.result = [fname]

    def finalize_result(self, fname):
        return self.result[1:]

    def find_include_file(self, t):
        """
        Finds the #include file for a given preprocessor tuple.
        """
        fname = t[2]
        for d in self.searchpath[t[1]]:
            if d == os.curdir:
                f = fname
            else:
                f = os.path.join(d, fname)
            if os.path.isfile(f):
                return f
        return None

    def read_file(self, file) -> str:
        with open(file, 'rb') as f:
            return SCons.Util.to_Text(f.read())

    # Start and stop processing include lines.

    def start_handling_includes(self, t=None) -> None:
        """
        Causes the PreProcessor object to start processing #import,
        #include and #include_next lines.

        This method will be called when a #if, #ifdef, #ifndef or #elif
        evaluates True, or when we reach the #else in a #if, #ifdef,
        #ifndef or #elif block where a condition already evaluated
        False.

        """
        d = self.dispatch_table
        p = self.stack[-1] if self.stack else self.default_table

        for k in ('import', 'include', 'include_next', 'define', 'undef'):
            d[k] = p[k]

    def stop_handling_includes(self, t=None) -> None:
        """
        Causes the PreProcessor object to stop processing #import,
        #include and #include_next lines.

        This method will be called when a #if, #ifdef, #ifndef or #elif
        evaluates False, or when we reach the #else in a #if, #ifdef,
        #ifndef or #elif block where a condition already evaluated True.
        """
        d = self.dispatch_table
        d['import'] = self.do_nothing
        d['include'] =  self.do_nothing
        d['include_next'] =  self.do_nothing
        d['define'] =  self.do_nothing
        d['undef'] =  self.do_nothing

    # Default methods for handling all of the preprocessor directives.
    # (Note that what actually gets called for a given directive at any
    # point in time is really controlled by the dispatch_table.)

    def _do_if_else_condition(self, condition) -> None:
        """
        Common logic for evaluating the conditions on #if, #ifdef and
        #ifndef lines.
        """
        self.save()
        d = self.dispatch_table
        if condition:
            self.start_handling_includes()
            d['elif'] = self.stop_handling_includes
            d['else'] = self.stop_handling_includes
        else:
            self.stop_handling_includes()
            d['elif'] = self.do_elif
            d['else'] = self.start_handling_includes

    def do_ifdef(self, t) -> None:
        """
        Default handling of a #ifdef line.
        """
        self._do_if_else_condition(t[1] in self.cpp_namespace)

    def do_ifndef(self, t) -> None:
        """
        Default handling of a #ifndef line.
        """
        self._do_if_else_condition(t[1] not in self.cpp_namespace)

    def do_if(self, t) -> None:
        """
        Default handling of a #if line.
        """
        self._do_if_else_condition(self.eval_expression(t))

    def do_elif(self, t) -> None:
        """
        Default handling of a #elif line.
        """
        d = self.dispatch_table
        if self.eval_expression(t):
            self.start_handling_includes()
            d['elif'] = self.stop_handling_includes
            d['else'] = self.stop_handling_includes

    def do_else(self, t) -> None:
        """
        Default handling of a #else line.
        """
        pass

    def do_endif(self, t) -> None:
        """
        Default handling of a #endif line.
        """
        self.restore()

    def do_define(self, t) -> None:
        """
        Default handling of a #define line.
        """
        _, name, args, expansion = t

        rval = self.eval_constant_expression(expansion)
        if rval is not None:
            expansion = rval
        else:
            # handle "defined" chain "! (defined (A) || defined (B)" ...
            if "defined " in expansion:
                self.cpp_namespace[name] = self.eval_expression(t[2:])
                return

        if args:
            evaluator = FunctionEvaluator(name, args[1:-1], expansion)
            self.cpp_namespace[name] = evaluator
        else:
            self.cpp_namespace[name] = expansion

    def do_undef(self, t) -> None:
        """
        Default handling of a #undef line.
        """
        try: del self.cpp_namespace[t[1]]
        except KeyError: pass

    def do_import(self, t) -> None:
        """
        Default handling of a #import line.
        """
        # XXX finish this -- maybe borrow/share logic from do_include()...?
        pass

    def do_include(self, t) -> None:
        """
        Default handling of a #include line.
        """
        t = self.resolve_include(t)
        if not t:
            return
        include_file = self.find_include_file(t)
        # avoid infinite recursion
        if not include_file or include_file in self.result:
            return
        self.result.append(include_file)
        # print include_file, len(self.tuples)

        # Handle maximum depth of nested includes
        if self.depth != -1:
            current_depth = 0
            for t in self.tuples:
                if t[0] == "scons_current_file":
                    current_depth += 1
            if current_depth >= self.depth:
                return

        new_tuples = [('scons_current_file', include_file)] + \
                      self.tupleize(self.read_file(include_file)) + \
                     [('scons_current_file', self.current_file)]
        self.tuples[:] = new_tuples + self.tuples

    # From: Stefan Seefeld <seefeld@sympatico.ca> (22 Nov 2005)
    #
    # By the way, #include_next is not the same as #include. The difference
    # being that #include_next starts its search in the path following the
    # path that let to the including file. In other words, if your system
    # include paths are ['/foo', '/bar'], and you are looking at a header
    # '/foo/baz.h', it might issue an '#include_next <baz.h>' which would
    # correctly resolve to '/bar/baz.h' (if that exists), but *not* see
    # '/foo/baz.h' again. See
    # https://gcc.gnu.org/onlinedocs/cpp/Wrapper-Headers.html for more notes.
    #
    # I have no idea in what context #import might be used.
    # Update: possibly these notes?
    # https://github.com/MicrosoftDocs/cpp-docs/blob/main/docs/preprocessor/hash-import-directive-cpp.md

    # XXX is #include_next really the same as #include ?
    do_include_next = do_include

    # Utility methods for handling resolution of include files.

    def resolve_include(self, t):
        """Resolve a tuple-ized #include line.

        This handles recursive expansion of values without "" or <>
        surrounding the name until an initial " or < is found, to handle
        #include FILE where FILE is a #define somewhere else.
        """
        s = t[1].strip()
        while not s[0] in '<"':
            try:
                s = self.cpp_namespace[s]
                # strip backslashes from the computed include (-DFOO_H=\"foo.h\")
                for c in '<">':
                    s = s.replace(f"\\{c}", c)
            except KeyError:
                m = function_name.search(s)

                # Date: Mon, 28 Nov 2016 17:47:13 UTC
                # From: Ivan Kravets <ikravets@platformio.org>
                #
                # Ignore `#include` directive that depends on dynamic macro
                # which is not located in state TABLE
                # For example, `#include MYCONFIG_FILE`
                if not m:
                    return None

                s = self.cpp_namespace[m.group(1)]
                if callable(s):
                    args = function_arg_separator.split(m.group(2))
                    s = s(*args)
            if not s:
                return None
        return (t[0], s[0], s[1:-1])

    def all_include(self, t) -> None:
        """
        """
        self.result.append(self.resolve_include(t))


class DumbPreProcessor(PreProcessor):
    """A preprocessor that ignores all #if/#elif/#else/#endif directives
    and just reports back *all* of the #include files (like the classic
    SCons scanner did).

    This is functionally equivalent to using a regular expression to
    find all of the #include lines, only slower.  It exists mainly as
    an example of how the main PreProcessor class can be sub-classed
    to tailor its behavior.
    """
    def __init__(self, *args, **kw) -> None:
        PreProcessor.__init__(self, *args, **kw)
        d = self.default_table
        for func in ['if', 'elif', 'else', 'endif', 'ifdef', 'ifndef']:
            d[func] = d[func] = self.do_nothing

# Local Variables:
# tab-width:4
# indent-tabs-mode:nil
# End:
# vim: set expandtab tabstop=4 shiftwidth=4:
