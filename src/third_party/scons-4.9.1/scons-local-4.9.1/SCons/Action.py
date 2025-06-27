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

"""SCons Actions.

Information about executing any sort of action that
can build one or more target Nodes (typically files) from one or more
source Nodes (also typically files) given a specific Environment.

The base class here is ActionBase.  The base class supplies just a few
utility methods and some generic methods for displaying information
about an Action in response to the various commands that control printing.

A second-level base class is _ActionAction.  This extends ActionBase
by providing the methods that can be used to show and perform an
action.  True Action objects will subclass _ActionAction; Action
factory class objects will subclass ActionBase.

The heavy lifting is handled by subclasses for the different types of
actions we might execute:

    CommandAction
    CommandGeneratorAction
    FunctionAction
    ListAction

The subclasses supply the following public interface methods used by
other modules:

    __call__()
        THE public interface, "calling" an Action object executes the
        command or Python function.  This also takes care of printing
        a pre-substitution command for debugging purposes.

    get_contents()
        Fetches the "contents" of an Action for signature calculation
        plus the varlist.  This is what gets checksummed to decide
        if a target needs to be rebuilt because its action changed.

    genstring()
        Returns a string representation of the Action *without*
        command substitution, but allows a CommandGeneratorAction to
        generate the right action based on the specified target,
        source and env.  This is used by the Signature subsystem
        (through the Executor) to obtain an (imprecise) representation
        of the Action operation for informative purposes.


Subclasses also supply the following methods for internal use within
this module:

    __str__()
        Returns a string approximation of the Action; no variable
        substitution is performed.

    execute()
        The internal method that really, truly, actually handles the
        execution of a command or Python function.  This is used so
        that the __call__() methods can take care of displaying any
        pre-substitution representations, and *then* execute an action
        without worrying about the specific Actions involved.

    get_presig()
        Fetches the "contents" of a subclass for signature calculation.
        The varlist is added to this to produce the Action's contents.
        TODO(?): Change this to always return bytes and not str?

    strfunction()
        Returns a substituted string representation of the Action.
        This is used by the _ActionAction.show() command to display the
        command/function that will be executed to generate the target(s).

There is a related independent ActionCaller class that looks like a
regular Action, and which serves as a wrapper for arbitrary functions
that we want to let the user specify the arguments to now, but actually
execute later (when an out-of-date check determines that it's needed to
be executed, for example).  Objects of this class are returned by an
ActionFactory class that provides a __call__() method as a convenient
way for wrapping up the functions.

"""

from __future__ import annotations

import inspect
import os
import pickle
import re
import subprocess
import sys
from abc import ABC, abstractmethod
from collections import OrderedDict
from subprocess import DEVNULL, PIPE
from typing import TYPE_CHECKING

import SCons.Debug
import SCons.Errors
import SCons.Subst
import SCons.Util

# we use these a lot, so try to optimize them
from SCons.Debug import logInstanceCreation
from SCons.Subst import SUBST_CMD, SUBST_RAW, SUBST_SIG
from SCons.Util import is_String, is_List

if TYPE_CHECKING:
    from SCons.Executor import Executor

class _null:
    pass

print_actions = True
execute_actions = True
print_actions_presub = False

# Use pickle protocol 4 when pickling functions for signature.
# This is the common format since Python 3.4
# TODO: use is commented out as not stable since 2017: e0bc3a04d5. Drop?
# ACTION_SIGNATURE_PICKLE_PROTOCOL = 4


def rfile(n):
    try:
        return n.rfile()
    except AttributeError:
        return n


def default_exitstatfunc(s):
    return s

strip_quotes = re.compile(r'^[\'"](.*)[\'"]$')


def _callable_contents(obj) -> bytearray:
    """Return the signature contents of a callable Python object."""
    try:
        # Test if obj is a method.
        return _function_contents(obj.__func__)

    except AttributeError:
        try:
            # Test if obj is a callable object.
            return _function_contents(obj.__call__.__func__)

        except AttributeError:
            try:
                # Test if obj is a code object.
                return _code_contents(obj)

            except AttributeError:
                # Test if obj is a function object.
                return _function_contents(obj)


def _object_contents(obj) -> bytearray:
    """Return the signature contents of any Python object.

    We have to handle the case where object contains a code object
    since it can be pickled directly.
    """
    try:
        # Test if obj is a method.
        return _function_contents(obj.__func__)

    except AttributeError:
        try:
            # Test if obj is a callable object.
            return _function_contents(obj.__call__.__func__)

        except AttributeError:
            try:
                # Test if obj is a code object.
                return _code_contents(obj)

            except AttributeError:
                try:
                    # Test if obj is a function object.
                    return _function_contents(obj)

                except AttributeError as ae:
                    # Should be a pickle-able Python object.
                    try:
                        return _object_instance_content(obj)
                        # pickling an Action instance or object doesn't yield a stable
                        # content as instance property may be dumped in different orders
                        # return pickle.dumps(obj, ACTION_SIGNATURE_PICKLE_PROTOCOL)
                    except (pickle.PicklingError, TypeError, AttributeError) as ex:
                        # This is weird, but it seems that nested classes
                        # are unpickable. The Python docs say it should
                        # always be a PicklingError, but some Python
                        # versions seem to return TypeError.  Just do
                        # the best we can.
                        return bytearray(repr(obj), 'utf-8')

# TODO: docstrings for _code_contents and _function_contents
#   do not render well with Sphinx. Consider reworking.

def _code_contents(code, docstring=None) -> bytearray:
    r"""Return the signature contents of a code object.

    By providing direct access to the code object of the
    function, Python makes this extremely easy.  Hooray!

    Unfortunately, older versions of Python include line
    number indications in the compiled byte code.  Boo!
    So we remove the line number byte codes to prevent
    recompilations from moving a Python function.

    See:
      - https://docs.python.org/3/library/inspect.html
      - http://python-reference.readthedocs.io/en/latest/docs/code/index.html

    For info on what each co\_ variable provides

    The signature is as follows (should be byte/chars):
    co_argcount, len(co_varnames), len(co_cellvars), len(co_freevars),
    ( comma separated signature for each object in co_consts ),
    ( comma separated signature for each object in co_names ),
    ( The bytecode with line number bytecodes removed from  co_code )

    co_argcount - Returns the number of positional arguments (including arguments with default values).
    co_varnames - Returns a tuple containing the names of the local variables (starting with the argument names).
    co_cellvars - Returns a tuple containing the names of local variables that are referenced by nested functions.
    co_freevars - Returns a tuple containing the names of free variables. (?)
    co_consts   - Returns a tuple containing the literals used by the bytecode.
    co_names    - Returns a tuple containing the names used by the bytecode.
    co_code     - Returns a string representing the sequence of bytecode instructions.

    """
    # contents = []

    # The code contents depends on the number of local variables
    # but not their actual names.
    contents = bytearray(f"{code.co_argcount}, {len(code.co_varnames)}", 'utf-8')

    contents.extend(b", ")
    contents.extend(bytearray(str(len(code.co_cellvars)), 'utf-8'))
    contents.extend(b", ")
    contents.extend(bytearray(str(len(code.co_freevars)), 'utf-8'))

    # The code contents depends on any constants accessed by the
    # function. Note that we have to call _object_contents on each
    # constants because the code object of nested functions can
    # show-up among the constants.
    z = [_object_contents(cc) for cc in code.co_consts if cc != docstring]
    contents.extend(b',(')
    contents.extend(bytearray(',', 'utf-8').join(z))
    contents.extend(b')')

    # The code contents depends on the variable names used to
    # accessed global variable, as changing the variable name changes
    # the variable actually accessed and therefore changes the
    # function result.
    z= [bytearray(_object_contents(cc)) for cc in code.co_names]
    contents.extend(b',(')
    contents.extend(bytearray(',','utf-8').join(z))
    contents.extend(b')')

    # The code contents depends on its actual code!!!
    contents.extend(b',(')
    contents.extend(code.co_code)
    contents.extend(b')')

    return contents


def _function_contents(func) -> bytearray:
    """Return the signature contents of a function.

    The signature is as follows (should be byte/chars):
    < _code_contents (see above) from func.__code__ >
    ,( comma separated _object_contents for function argument defaults)
    ,( comma separated _object_contents for any closure contents )


    See also: https://docs.python.org/3/reference/datamodel.html
      - func.__code__     - The code object representing the compiled function body.
      - func.__defaults__ - A tuple containing default argument values for those arguments that have defaults, or None if no arguments have a default value
      - func.__closure__  - None or a tuple of cells that contain bindings for the function's free variables.
    """
    contents = [_code_contents(func.__code__, func.__doc__)]

    # The function contents depends on the value of defaults arguments
    if func.__defaults__:

        function_defaults_contents = [_object_contents(cc) for cc in func.__defaults__]

        defaults = bytearray(b',(')
        defaults.extend(bytearray(b',').join(function_defaults_contents))
        defaults.extend(b')')

        contents.append(defaults)
    else:
        contents.append(b',()')

    # The function contents depends on the closure captured cell values.
    closure = func.__closure__ or []

    try:
        closure_contents = [_object_contents(x.cell_contents) for x in closure]
    except AttributeError:
        closure_contents = []

    contents.append(b',(')
    contents.append(bytearray(b',').join(closure_contents))
    contents.append(b')')

    retval = bytearray(b'').join(contents)
    return retval


def _object_instance_content(obj):
    """
    Returns consistant content for a action class or an instance thereof

    :Parameters:
      - `obj` Should be either and action class or an instance thereof

    :Returns:
      bytearray or bytes representing the obj suitable for generating a signature from.
    """
    retval = bytearray()

    if obj is None:
        return b'N.'

    if isinstance(obj, SCons.Util.BaseStringTypes):
        return SCons.Util.to_bytes(obj)

    inst_class = obj.__class__
    inst_class_name = bytearray(obj.__class__.__name__,'utf-8')
    inst_class_module = bytearray(obj.__class__.__module__,'utf-8')
    inst_class_hierarchy = bytearray(repr(inspect.getclasstree([obj.__class__,])),'utf-8')
    # print("ICH:%s : %s"%(inst_class_hierarchy, repr(obj)))

    properties = [(p, getattr(obj, p, "None")) for p in dir(obj) if not (p[:2] == '__' or inspect.ismethod(getattr(obj, p)) or inspect.isbuiltin(getattr(obj,p))) ]
    properties.sort()
    properties_str = ','.join(["%s=%s"%(p[0],p[1]) for p in properties])
    properties_bytes = bytearray(properties_str,'utf-8')

    methods = [p for p in dir(obj) if inspect.ismethod(getattr(obj, p))]
    methods.sort()

    method_contents = []
    for m in methods:
        # print("Method:%s"%m)
        v = _function_contents(getattr(obj, m))
        # print("[%s->]V:%s [%s]"%(m,v,type(v)))
        method_contents.append(v)

    retval = bytearray(b'{')
    retval.extend(inst_class_name)
    retval.extend(b":")
    retval.extend(inst_class_module)
    retval.extend(b'}[[')
    retval.extend(inst_class_hierarchy)
    retval.extend(b']]{{')
    retval.extend(bytearray(b",").join(method_contents))
    retval.extend(b"}}{{{")
    retval.extend(properties_bytes)
    retval.extend(b'}}}')
    return retval

    # print("class          :%s"%inst_class)
    # print("class_name     :%s"%inst_class_name)
    # print("class_module   :%s"%inst_class_module)
    # print("Class hier     :\n%s"%pp.pformat(inst_class_hierarchy))
    # print("Inst Properties:\n%s"%pp.pformat(properties))
    # print("Inst Methods   :\n%s"%pp.pformat(methods))

def _actionAppend(act1, act2):
    """Joins two actions together.

    Mainly, it handles ListActions by concatenating into a single ListAction.
    """
    a1 = Action(act1)
    a2 = Action(act2)
    if a1 is None:
        return a2
    if a2 is None:
        return a1
    if isinstance(a1, ListAction):
        if isinstance(a2, ListAction):
            return ListAction(a1.list + a2.list)
        return ListAction(a1.list + [ a2 ])

    if isinstance(a2, ListAction):
        return ListAction([ a1 ] + a2.list)

    return ListAction([ a1, a2 ])


def _do_create_keywords(args, kw):
    """This converts any arguments after the action argument into
    their equivalent keywords and adds them to the kw argument.
    """
    v = kw.get('varlist', ())
    # prevent varlist="FOO" from being interpreted as ['F', 'O', 'O']
    if is_String(v): v = (v,)
    kw['varlist'] = tuple(v)
    if args:
        # turn positional args into equivalent keywords
        cmdstrfunc = args[0]
        if cmdstrfunc is None or is_String(cmdstrfunc):
            kw['cmdstr'] = cmdstrfunc
        elif callable(cmdstrfunc):
            kw['strfunction'] = cmdstrfunc
        else:
            raise SCons.Errors.UserError(
                'Invalid command display variable type. '
                'You must either pass a string or a callback which '
                'accepts (target, source, env) as parameters.')
        if len(args) > 1:
            kw['varlist'] = tuple(SCons.Util.flatten(args[1:])) + kw['varlist']
    if kw.get('strfunction', _null) is not _null \
                      and kw.get('cmdstr', _null) is not _null:
        raise SCons.Errors.UserError(
            'Cannot have both strfunction and cmdstr args to Action()')


def _do_create_action(act, kw):
    """The internal implementation for the Action factory method.

    This handles the fact that passing lists to :func:`Action` itself has
    different semantics than passing lists as elements of lists.
    The former will create a :class:`ListAction`, the latter will create a
    :class:`CommandAction` by converting the inner list elements to strings.
    """
    if isinstance(act, ActionBase):
        return act

    if is_String(act):
        var = SCons.Util.get_environment_var(act)
        if var:
            # This looks like a string that is purely an Environment
            # variable reference, like "$FOO" or "${FOO}".  We do
            # something special here...we lazily evaluate the contents
            # of that Environment variable, so a user could put something
            # like a function or a CommandGenerator in that variable
            # instead of a string.
            return LazyAction(var, kw)
        commands = str(act).split('\n')
        if len(commands) == 1:
            return CommandAction(commands[0], **kw)
        # The list of string commands may include a LazyAction, so we
        # reprocess them via _do_create_list_action.
        return _do_create_list_action(commands, kw)

    if is_List(act):
        return CommandAction(act, **kw)

    if callable(act):
        gen = kw.pop('generator', False)
        if gen:
            action_type = CommandGeneratorAction
        else:
            action_type = FunctionAction
        return action_type(act, kw)

    # Catch a common error case with a nice message:
    if isinstance(act, (int, float)):
        raise TypeError("Don't know how to create an Action from a number (%s)"%act)
    # Else fail silently (???)
    return None


def _do_create_list_action(act, kw) -> ListAction:
    """A factory for list actions.

    Convert the input list *act* into Actions and then wrap them in a
    :class:`ListAction`. If *act* has only a single member, return that
    member, not a *ListAction*. This is intended to allow a contained
    list to specify a command action without being processed into a
    list action.
    """
    acts = []
    for a in act:
        aa = _do_create_action(a, kw)
        if aa is not None:
            acts.append(aa)
    if not acts:
        return ListAction([])
    if len(acts) == 1:
        return acts[0]
    return ListAction(acts)


def Action(act, *args, **kw):
    """A factory for action objects."""
    # Really simple: the _do_create_* routines do the heavy lifting.
    _do_create_keywords(args, kw)
    if is_List(act):
        return _do_create_list_action(act, kw)
    return _do_create_action(act, kw)


class ActionBase(ABC):
    """Base class for all types of action objects that can be held by
    other objects (Builders, Executors, etc.)  This provides the
    common methods for manipulating and combining those actions."""

    @abstractmethod
    def __call__(
        self,
        target,
        source,
        env,
        exitstatfunc=_null,
        presub=_null,
        show=_null,
        execute=_null,
        chdir=_null,
        executor: Executor | None = None,
    ):
        raise NotImplementedError

    def __eq__(self, other):
        return self.__dict__ == other

    def no_batch_key(self, env, target, source):
        return None

    batch_key = no_batch_key

    def genstring(self, target, source, env, executor: Executor | None = None) -> str:
        return str(self)

    @abstractmethod
    def get_presig(self, target, source, env, executor: Executor | None = None):
        raise NotImplementedError

    @abstractmethod
    def get_implicit_deps(self, target, source, env, executor: Executor | None = None):
        raise NotImplementedError

    def get_contents(self, target, source, env):
        result = self.get_presig(target, source, env)

        if not isinstance(result, (bytes, bytearray)):
            result = bytearray(result, 'utf-8')
        else:
            # Make a copy and put in bytearray, without this the contents returned by get_presig
            # can be changed by the logic below, appending with each call and causing very
            # hard to track down issues...
            result = bytearray(result)

        # At this point everything should be a bytearray

        # This should never happen, as the Action() factory should wrap
        # the varlist, but just in case an action is created directly,
        # we duplicate this check here.
        vl = self.get_varlist(target, source, env)
        if is_String(vl): vl = (vl,)
        for v in vl:
            # do the subst this way to ignore $(...$) parts:
            if isinstance(result, bytearray):
                result.extend(SCons.Util.to_bytes(env.subst_target_source('${'+v+'}', SUBST_SIG, target, source)))
            else:
                raise Exception("WE SHOULD NEVER GET HERE result should be bytearray not:%s"%type(result))
                # result.append(SCons.Util.to_bytes(env.subst_target_source('${'+v+'}', SUBST_SIG, target, source)))

        if isinstance(result, (bytes, bytearray)):
            return result

        raise Exception("WE SHOULD NEVER GET HERE - #2 result should be bytearray not:%s" % type(result))

    def __add__(self, other):
        return _actionAppend(self, other)

    def __radd__(self, other):
        return _actionAppend(other, self)

    def presub_lines(self, env):
        # CommandGeneratorAction needs a real environment
        # in order to return the proper string here, since
        # it may call LazyAction, which looks up a key
        # in that env.  So we temporarily remember the env here,
        # and CommandGeneratorAction will use this env
        # when it calls its _generate method.
        self.presub_env = env
        lines = str(self).split('\n')
        self.presub_env = None      # don't need this any more
        return lines

    def get_varlist(self, target, source, env, executor: Executor | None = None):
        return self.varlist

    def get_targets(self, env, executor: Executor | None):
        """
        Returns the type of targets ($TARGETS, $CHANGED_TARGETS) used
        by this action.
        """
        return self.targets


class _ActionAction(ActionBase):
    """Base class for actions that create output objects."""
    def __init__(self, cmdstr=_null, strfunction=_null, varlist=(),
                       presub=_null, chdir=None, exitstatfunc=None,
                       batch_key=None, targets: str='$TARGETS',
                 **kw) -> None:
        self.cmdstr = cmdstr
        if strfunction is not _null:
            if strfunction is None:
                self.cmdstr = None
            else:
                self.strfunction = strfunction
        self.varlist = varlist
        self.presub = presub
        self.chdir = chdir
        if not exitstatfunc:
            exitstatfunc = default_exitstatfunc
        self.exitstatfunc = exitstatfunc

        self.targets = targets

        if batch_key:
            if not callable(batch_key):
                # They have set batch_key, but not to their own
                # callable.  The default behavior here will batch
                # *all* targets+sources using this action, separated
                # for each construction environment.
                def default_batch_key(self, env, target, source):
                    return (id(self), id(env))
                batch_key = default_batch_key
            SCons.Util.AddMethod(self, batch_key, 'batch_key')

    def print_cmd_line(self, s, target, source, env) -> None:
        """
        In python 3, and in some of our tests, sys.stdout is
        a String io object, and it takes unicode strings only
        This code assumes s is a regular string.
        """
        sys.stdout.write(s + "\n")

    def __call__(self, target, source, env,
                               exitstatfunc=_null,
                               presub=_null,
                               show=_null,
                               execute=_null,
                               chdir=_null,
                               executor: Executor | None = None):
        if not is_List(target):
            target = [target]
        if not is_List(source):
            source = [source]

        if presub is _null:
            presub = self.presub
            if presub is _null:
                presub = print_actions_presub
        if exitstatfunc is _null:
            exitstatfunc = self.exitstatfunc
        if show is _null:
            show = print_actions
        if execute is _null:
            execute = execute_actions
        if chdir is _null:
            chdir = self.chdir
        save_cwd = None
        if chdir:
            save_cwd = os.getcwd()
            try:
                chdir = str(chdir.get_abspath())
            except AttributeError:
                if not is_String(chdir):
                    if executor:
                        chdir = str(executor.batches[0].targets[0].dir)
                    else:
                        chdir = str(target[0].dir)
        if presub:
            if executor:
                target = executor.get_all_targets()
                source = executor.get_all_sources()
            t = ' and '.join(map(str, target))
            l = '\n  '.join(self.presub_lines(env))
            out = "Building %s with action:\n  %s\n" % (t, l)
            sys.stdout.write(out)
        cmd = None
        if show and self.strfunction:
            if executor:
                target = executor.get_all_targets()
                source = executor.get_all_sources()
            try:
                cmd = self.strfunction(target, source, env, executor)
            except TypeError:
                cmd = self.strfunction(target, source, env)
            if cmd:
                if chdir:
                    cmd = ('os.chdir(%s)\n' % repr(chdir)) + cmd
                try:
                    get = env.get
                except AttributeError:
                    print_func = self.print_cmd_line
                else:
                    print_func = get('PRINT_CMD_LINE_FUNC')
                    if not print_func:
                        print_func = self.print_cmd_line
                print_func(cmd, target, source, env)
        stat = 0
        if execute:
            if chdir:
                os.chdir(chdir)
            try:
                stat = self.execute(target, source, env, executor=executor)
                if isinstance(stat, SCons.Errors.BuildError):
                    s = exitstatfunc(stat.status)
                    if s:
                        stat.status = s
                    else:
                        stat = s
                else:
                    stat = exitstatfunc(stat)
            finally:
                if save_cwd:
                    os.chdir(save_cwd)
        if cmd and save_cwd:
            print_func('os.chdir(%s)' % repr(save_cwd), target, source, env)

        return stat

    # Stub these two only so _ActionAction can be instantiated. It's really
    # an ABC like parent ActionBase, but things reach in and use it. It's
    # not just unittests or we could fix it up with a concrete subclass there.

    def get_presig(self, target, source, env, executor: Executor | None = None):
        raise NotImplementedError

    def get_implicit_deps(self, target, source, env, executor: Executor | None = None):
        raise NotImplementedError


def _string_from_cmd_list(cmd_list):
    """Takes a list of command line arguments and returns a pretty
    representation for printing."""
    cl = []
    for arg in map(str, cmd_list):
        if ' ' in arg or '\t' in arg:
            arg = '"' + arg + '"'
        cl.append(arg)
    return ' '.join(cl)

default_ENV = None


def get_default_ENV(env):
    """Returns an execution environment.

    If there is one in *env*, just use it, else return the Default
    Environment, insantiated if necessary.

    A fiddlin' little function that has an ``import SCons.Environment``
    which cannot be moved to the top level without creating an import
    loop.  Since this import creates a local variable named ``SCons``,
    it blocks access to the global variable, so we move it here to
    prevent complaints about local variables being used uninitialized.
    """
    global default_ENV

    try:
        return env['ENV']
    except KeyError:
        if not default_ENV:
            import SCons.Environment  # pylint: disable=import-outside-toplevel,redefined-outer-name
            # This is a hideously expensive way to get a default execution
            # environment.  What it really should do is run the platform
            # setup to get the default ENV.  Fortunately, it's incredibly
            # rare for an Environment not to have an execution environment,
            # so we're not going to worry about it overmuch.
            default_ENV = SCons.Environment.Environment()['ENV']
        return default_ENV


def _resolve_shell_env(env, target, source):
    """Returns a resolved execution environment.

    First get the execution environment.  Then if ``SHELL_ENV_GENERATORS``
    is set and is iterable, call each function to allow it to alter the
    created execution environment, passing each the returned execution
    environment from the previous call.

    .. versionadded:: 4.4
    """
    ENV = get_default_ENV(env)
    shell_gen = env.get('SHELL_ENV_GENERATORS')
    if shell_gen:
        try:
            shell_gens = iter(shell_gen)
        except TypeError:
            raise SCons.Errors.UserError("SHELL_ENV_GENERATORS must be iteratable.")

        ENV = ENV.copy()
        for generator in shell_gens:
            ENV = generator(env, target, source, ENV)
            if not isinstance(ENV, dict):
                raise SCons.Errors.UserError(f"SHELL_ENV_GENERATORS function: {generator} must return a dict.")

    return ENV


def scons_subproc_run(scons_env, *args, **kwargs) -> subprocess.CompletedProcess:
    """Run an external command using an SCons execution environment.

    SCons normally runs external build commands using :mod:`subprocess`,
    but does not harvest any output from such commands. This function
    is a thin wrapper around :func:`subprocess.run` allowing running
    a command in an SCons context (i.e. uses an "execution environment"
    rather than the user's existing environment), and provides the ability
    to return any output in a :class:`subprocess.CompletedProcess`
    instance (this must be selected by setting ``stdout`` and/or
    ``stderr`` to ``PIPE``, or setting ``capture_output=True`` - see
    Keyword Arguments). Typical use case is to run a tool's "version"
    option to find out the installed version.

    If supplied, the ``env`` keyword argument provides an execution
    environment to process into appropriate form before it is supplied
    to :mod:`subprocess`; if omitted, *scons_env* is used to derive a
    suitable default.  The other keyword arguments are passed through,
    except that the SCons legacy ``error`` keyword is remapped to the
    subprocess ``check`` keyword; if both are omitted ``check=False``
    will be passed. The caller is responsible for setting up the desired
    arguments  for :func:`subprocess.run`.

    This function retains the legacy behavior of returning something
    vaguely usable even in the face of complete failure, unless
    ``check=True`` (in which case an error is allowed to be raised):
    it synthesizes a :class:`~subprocess.CompletedProcess` instance in
    this case.

    A subset of interesting keyword arguments follows; see the Python
    documentation of :mod:`subprocess` for the complete list.

    Keyword Arguments:
       stdout: (and *stderr*, *stdin*) if set to :const:`subprocess.PIPE`.
          send input to or collect output from the relevant stream in
          the subprocess; the default ``None`` does no redirection
          (i.e. output or errors may go to the console or log file,
          but is not captured); if set to :const:`subprocess.DEVNULL`
          they are explicitly thrown away. ``capture_output=True`` is a
          synonym for setting both ``stdout`` and ``stderr``
          to :const:`~subprocess.PIPE`.
       text: open *stdin*, *stdout*, *stderr* in text mode. Default
          is binary mode. ``universal_newlines`` is a synonym.
       encoding: specifies an encoding. Changes to text mode.
       errors: specified error handling. Changes to text mode.
       input: a byte sequence to be passed to *stdin*, unless text
          mode is enabled, in which case it must be a string.
       shell: if true, the command is executed through the shell.
       check: if true and the subprocess exits with a non-zero exit
          code, raise a :exc:`subprocess.CalledProcessError` exception.
          Otherwise (the default) in case of an :exc:`OSError`, report the
          exit code in the :class:`~Subprocess.CompletedProcess` instance.

    .. versionadded:: 4.6
    """
    # Figure out the execution environment to use
    env = kwargs.get('env', None)
    if env is None:
        env = get_default_ENV(scons_env)
    kwargs['env'] = SCons.Util.sanitize_shell_env(env)

    # Backwards-compat with _subproc: accept 'error', map to 'check',
    # and remove, since subprocess.run does not recognize.
    # 'error' isn't True/False, it takes a string value (see _subproc)
    error = kwargs.get('error')
    if error and 'check' in kwargs:
        raise ValueError('error and check arguments may not both be used.')
    check = kwargs.get('check', False)  # always set a value for 'check'
    if error is not None:
        if error == 'raise':
            check = True
        del kwargs['error']
    kwargs['check'] = check

    # Most SCons tools/tests expect not to fail on things like missing files.
    # check=True (or error="raise") means we're okay to take an exception;
    # else we catch the likely exception and construct a dummy
    # CompletedProcess instance.
    # Note pylint can't see we always include 'check' in kwargs: suppress.
    if check:
        cp = subprocess.run(*args, **kwargs)  # pylint: disable=subprocess-run-check
    else:
        try:
            cp = subprocess.run(*args, **kwargs)  # pylint: disable=subprocess-run-check
        except OSError as exc:
            argline = ' '.join(*args)
            cp = subprocess.CompletedProcess(
                args=argline, returncode=1, stdout="", stderr=""
            )

    return cp


def _subproc(scons_env, cmd, error='ignore', **kw):
    """Wrapper for subprocess.Popen which pulls from construction env.

    Use for calls to subprocess which need to interpolate values from
    an SCons construction environment into the environment passed to
    subprocess.  Adds an an error-handling argument.  Adds ability
    to specify std{in,out,err} with "'devnull'" tag.

    .. deprecated:: 4.6
    """
    # TODO: just uses subprocess.DEVNULL now, we can drop the "devnull"
    # string now - it is a holdover from Py2, which didn't have DEVNULL.
    for stream in 'stdin', 'stdout', 'stderr':
        io = kw.get(stream)
        if is_String(io) and io == 'devnull':
            kw[stream] = DEVNULL

    # Figure out what execution environment to use
    ENV = kw.get('env', None)
    if ENV is None: ENV = get_default_ENV(scons_env)

    kw['env'] = SCons.Util.sanitize_shell_env(ENV)

    try:
        pobj = subprocess.Popen(cmd, **kw)
    except OSError as e:
        if error == 'raise': raise
        # return a dummy Popen instance that only returns error
        class dummyPopen:
            def __init__(self, e) -> None:
                self.exception = e
            # Add the following two to enable using the return value as a context manager
            # for example
            #    with Action._subproc(...) as po:
            #       logic here which uses po

            def __enter__(self):
                return self

            def __exit__(self, *args) -> None:
                pass

            def communicate(self, input=None):
                return ('', '')

            def wait(self):
                return -self.exception.errno

            stdin = None
            class f:
                def read(self) -> str: return ''
                def readline(self) -> str: return ''
                def __iter__(self): return iter(())
            stdout = stderr = f()
        pobj = dummyPopen(e)
    finally:
        # clean up open file handles stored in parent's kw
        for k, v in kw.items():
            if inspect.ismethod(getattr(v, 'close', None)):
                v.close()

    return pobj


class CommandAction(_ActionAction):
    """Class for command-execution actions."""
    def __init__(self, cmd, **kw) -> None:
        # Cmd can actually be a list or a single item; if it's a
        # single item it should be the command string to execute; if a
        # list then it should be the words of the command string to
        # execute.  Only a single command should be executed by this
        # object; lists of commands should be handled by embedding
        # these objects in a ListAction object (which the Action()
        # factory above does).  cmd will be passed to
        # Environment.subst_list() for substituting environment
        # variables.
        if SCons.Debug.track_instances: logInstanceCreation(self, 'Action.CommandAction')

        super().__init__(**kw)
        if is_List(cmd):
            if [c for c in cmd if is_List(c)]:
                raise TypeError("CommandAction should be given only "
                                "a single command")
        self.cmd_list = cmd

    def __str__(self) -> str:
        if is_List(self.cmd_list):
            return ' '.join(map(str, self.cmd_list))
        return str(self.cmd_list)


    def process(self, target, source, env, executor: Executor | None = None, overrides: dict | None = None) -> tuple[list, bool, bool]:
        if executor:
            result = env.subst_list(self.cmd_list, SUBST_CMD, executor=executor, overrides=overrides)
        else:
            result = env.subst_list(self.cmd_list, SUBST_CMD, target, source, overrides=overrides)
        silent = False
        ignore = False
        while True:
            try: c = result[0][0][0]
            except IndexError: c = None
            if c == '@': silent = True
            elif c == '-': ignore = True
            else: break
            result[0][0] = result[0][0][1:]
        try:
            if not result[0][0]:
                result[0] = result[0][1:]
        except IndexError:
            pass
        return result, ignore, silent

    def strfunction(self, target, source, env, executor: Executor | None = None, overrides: dict | None = None) -> str:
        if self.cmdstr is None:
            return None
        if self.cmdstr is not _null:
            if executor:
                c = env.subst(self.cmdstr, SUBST_RAW, executor=executor, overrides=overrides)
            else:
                c = env.subst(self.cmdstr, SUBST_RAW, target, source, overrides=overrides)
            if c:
                return c
        cmd_list, ignore, silent = self.process(target, source, env, executor, overrides=overrides)
        if silent:
            return ''
        return _string_from_cmd_list(cmd_list[0])

    def execute(self, target, source, env, executor: Executor | None = None):
        """Execute a command action.

        This will handle lists of commands as well as individual commands,
        because construction variable substitution may turn a single
        "command" into a list.  This means that this class can actually
        handle lists of commands, even though that's not how we use it
        externally.
        """
        escape_list = SCons.Subst.escape_list
        flatten_sequence = SCons.Util.flatten_sequence

        try:
            shell = env['SHELL']
        except KeyError:
            raise SCons.Errors.UserError('Missing SHELL construction variable.')

        try:
            spawn = env['SPAWN']
        except KeyError:
            raise SCons.Errors.UserError('Missing SPAWN construction variable.')

        if is_String(spawn):
            spawn = env.subst(spawn, raw=1, conv=lambda x: x)

        escape = env.get('ESCAPE', lambda x: x)
        ENV = _resolve_shell_env(env, target, source)

        # Ensure that the ENV values are all strings:
        for key, value in ENV.items():
            if not is_String(value):
                if is_List(value):
                    # If the value is a list, then we assume it is a
                    # path list, because that's a pretty common list-like
                    # value to stick in an environment variable:
                    value = flatten_sequence(value)
                    ENV[key] = os.pathsep.join(map(str, value))
                else:
                    # If it isn't a string or a list, then we just coerce
                    # it to a string, which is the proper way to handle
                    # Dir and File instances and will produce something
                    # reasonable for just about everything else:
                    ENV[key] = str(value)

        if executor:
            target = executor.get_all_targets()
            source = executor.get_all_sources()
        cmd_list, ignore, silent = self.process(target, list(map(rfile, source)), env, executor)

        # Use len() to filter out any "command" that's zero-length.
        for cmd_line in filter(len, cmd_list):
            # Escape the command line for the interpreter we are using.
            cmd_line = escape_list(cmd_line, escape)
            result = spawn(shell, escape, cmd_line[0], cmd_line, ENV)
            if not ignore and result:
                msg = "Error %s" % result
                return SCons.Errors.BuildError(errstr=msg,
                                               status=result,
                                               action=self,
                                               command=cmd_line)
        return 0

    def get_presig(self, target, source, env, executor: Executor | None = None):
        """Return the signature contents of this action's command line.

        This strips $(-$) and everything in between the string,
        since those parts don't affect signatures.
        """
        cmd = self.cmd_list
        if is_List(cmd):
            cmd = ' '.join(map(str, cmd))
        else:
            cmd = str(cmd)
        if executor:
            return env.subst_target_source(cmd, SUBST_SIG, executor=executor)
        return env.subst_target_source(cmd, SUBST_SIG, target, source)

    def get_implicit_deps(self, target, source, env, executor: Executor | None = None):
        """Return the implicit dependencies of this action's command line."""
        icd = env.get('IMPLICIT_COMMAND_DEPENDENCIES', True)
        if is_String(icd) and icd[:1] == '$':
            icd = env.subst(icd)

        if not icd or str(icd).lower() in ('0', 'none', 'false', 'no', 'off'):
            return []

        try:
            icd_int = int(icd)
        except ValueError:
            icd_int = None

        if (icd_int and icd_int > 1) or str(icd).lower() == 'all':
            # An integer value greater than 1 specifies the number of entries
            # to scan. "all" means to scan all.
            return self._get_implicit_deps_heavyweight(target, source, env, executor, icd_int)
        # Everything else (usually 1 or True) means that we want
        # lightweight dependency scanning.
        return self._get_implicit_deps_lightweight(target, source, env, executor)

    def _get_implicit_deps_lightweight(self, target, source, env, executor: Executor | None):
        """
        Lightweight dependency scanning involves only scanning the first entry
        in an action string, even if it contains &&.
        """
        if executor:
            cmd_list = env.subst_list(self.cmd_list, SUBST_SIG, executor=executor)
        else:
            cmd_list = env.subst_list(self.cmd_list, SUBST_SIG, target, source)
        res = []
        for cmd_line in cmd_list:
            if cmd_line:
                d = str(cmd_line[0])
                m = strip_quotes.match(d)
                if m:
                    d = m.group(1)
                d = env.WhereIs(d)
                if d:
                    res.append(env.fs.File(d))
        return res

    def _get_implicit_deps_heavyweight(self, target, source, env, executor: Executor | None,
                                       icd_int):
        """
        Heavyweight dependency scanning involves scanning more than just the
        first entry in an action string. The exact behavior depends on the
        value of icd_int. Only files are taken as implicit dependencies;
        directories are ignored.

        If icd_int is an integer value, it specifies the number of entries to
        scan for implicit dependencies. Action strings are also scanned after
        a &&. So for example, if icd_int=2 and the action string is
        "cd <some_dir> && $PYTHON $SCRIPT_PATH <another_path>", the implicit
        dependencies would be the path to the python binary and the path to the
        script.

        If icd_int is None, all entries are scanned for implicit dependencies.
        """

        # Avoid circular and duplicate dependencies by not providing source,
        # target, or executor to subst_list. This causes references to
        # $SOURCES, $TARGETS, and all related variables to disappear.
        cmd_list = env.subst_list(self.cmd_list, SUBST_SIG, conv=lambda x: x)
        res = []

        for cmd_line in cmd_list:
            if cmd_line:
                entry_count = 0
                for entry in cmd_line:
                    d = str(entry)
                    if ((icd_int is None or entry_count < icd_int) and
                            not d.startswith(('&', '-', '/') if os.name == 'nt'
                                             else ('&', '-'))):
                        m = strip_quotes.match(d)
                        if m:
                            d = m.group(1)

                        if d:
                            # Resolve the first entry in the command string using
                            # PATH, which env.WhereIs() looks in.
                            # For now, only match files, not directories.
                            p = os.path.abspath(d) if os.path.isfile(d) else None
                            if not p and entry_count == 0:
                                p = env.WhereIs(d)

                            if p:
                                res.append(env.fs.File(p))

                        entry_count = entry_count + 1
                    else:
                        entry_count = 0 if d == '&&' else entry_count + 1

        # Despite not providing source and target to env.subst() above, we
        # can still end up with sources in this list. For example, files in
        # LIBS will still resolve in env.subst(). This won't result in
        # circular dependencies, but it causes problems with cache signatures
        # changing between full and incremental builds.
        return [r for r in res if r not in target and r not in source]


class CommandGeneratorAction(ActionBase):
    """Class for command-generator actions."""
    def __init__(self, generator, kw) -> None:
        if SCons.Debug.track_instances: logInstanceCreation(self, 'Action.CommandGeneratorAction')
        self.generator = generator
        self.gen_kw = kw
        self.varlist = kw.get('varlist', ())
        self.targets = kw.get('targets', '$TARGETS')

    def _generate(self, target, source, env, for_signature, executor: Executor | None = None):
        # ensure that target is a list, to make it easier to write
        # generator functions:
        if not is_List(target):
            target = [target]

        if executor:
            target = executor.get_all_targets()
            source = executor.get_all_sources()
        ret = self.generator(target=target,
                             source=source,
                             env=env,
                             for_signature=for_signature)
        gen_cmd = Action(ret, **self.gen_kw)
        if not gen_cmd:
            raise SCons.Errors.UserError("Object returned from command generator: %s cannot be used to create an Action." % repr(ret))
        return gen_cmd

    def __str__(self) -> str:
        try:
            env = self.presub_env
        except AttributeError:
            env = None
        if env is None:
            env = SCons.Defaults.DefaultEnvironment()
        act = self._generate([], [], env, 1)
        return str(act)

    def batch_key(self, env, target, source):
        return self._generate(target, source, env, 1).batch_key(env, target, source)

    def genstring(self, target, source, env, executor: Executor | None = None) -> str:
        return self._generate(target, source, env, 1, executor).genstring(target, source, env)

    def __call__(self, target, source, env, exitstatfunc=_null, presub=_null,
                 show=_null, execute=_null, chdir=_null, executor: Executor | None = None):
        act = self._generate(target, source, env, 0, executor)
        if act is None:
            raise SCons.Errors.UserError(
                "While building `%s': "
                "Cannot deduce file extension from source files: %s"
                % (repr(list(map(str, target))), repr(list(map(str, source))))
            )
        return act(
            target, source, env, exitstatfunc, presub, show, execute, chdir, executor
        )

    def get_presig(self, target, source, env, executor: Executor | None = None):
        """Return the signature contents of this action's command line.

        This strips $(-$) and everything in between the string,
        since those parts don't affect signatures.
        """
        return self._generate(target, source, env, 1, executor).get_presig(target, source, env)

    def get_implicit_deps(self, target, source, env, executor: Executor | None = None):
        return self._generate(target, source, env, 1, executor).get_implicit_deps(target, source, env)

    def get_varlist(self, target, source, env, executor: Executor | None = None):
        return self._generate(target, source, env, 1, executor).get_varlist(target, source, env, executor)

    def get_targets(self, env, executor: Executor | None):
        return self._generate(None, None, env, 1, executor).get_targets(env, executor)


class LazyAction(CommandGeneratorAction, CommandAction):
    """
    A LazyAction is a kind of hybrid generator and command action for
    strings of the form "$VAR".  These strings normally expand to other
    strings (think "$CCCOM" to "$CC -c -o $TARGET $SOURCE"), but we also
    want to be able to replace them with functions in the construction
    environment.  Consequently, we want lazy evaluation and creation of
    an Action in the case of the function, but that's overkill in the more
    normal case of expansion to other strings.

    So we do this with a subclass that's both a generator *and*
    a command action.  The overridden methods all do a quick check
    of the construction variable, and if it's a string we just call
    the corresponding CommandAction method to do the heavy lifting.
    If not, then we call the same-named CommandGeneratorAction method.
    The CommandGeneratorAction methods work by using the overridden
    _generate() method, that is, our own way of handling "generation" of
    an action based on what's in the construction variable.
    """

    def __init__(self, var, kw) -> None:
        if SCons.Debug.track_instances: logInstanceCreation(self, 'Action.LazyAction')
        CommandAction.__init__(self, '${'+var+'}', **kw)
        self.var = SCons.Util.to_String(var)
        self.gen_kw = kw

    def get_parent_class(self, env):
        c = env.get(self.var)
        if is_String(c) and '\n' not in c:
            return CommandAction
        return CommandGeneratorAction

    def _generate_cache(self, env):
        if env:
            c = env.get(self.var, '')
        else:
            c = ''
        gen_cmd = Action(c, **self.gen_kw)
        if not gen_cmd:
            raise SCons.Errors.UserError("$%s value %s cannot be used to create an Action." % (self.var, repr(c)))
        return gen_cmd

    def _generate(self, target, source, env, for_signature, executor: Executor | None = None):
        return self._generate_cache(env)

    def __call__(self, target, source, env, *args, **kw):
        c = self.get_parent_class(env)
        return c.__call__(self, target, source, env, *args, **kw)

    def get_presig(self, target, source, env, executor: Executor | None = None):
        c = self.get_parent_class(env)
        return c.get_presig(self, target, source, env)

    def get_implicit_deps(self, target, source, env, executor: Executor | None = None):
        c = self.get_parent_class(env)
        return c.get_implicit_deps(self, target, source, env)

    def get_varlist(self, target, source, env, executor: Executor | None = None):
        c = self.get_parent_class(env)
        return c.get_varlist(self, target, source, env, executor)


class FunctionAction(_ActionAction):
    """Class for Python function actions."""

    def __init__(self, execfunction, kw) -> None:
        if SCons.Debug.track_instances: logInstanceCreation(self, 'Action.FunctionAction')

        self.execfunction = execfunction
        try:
            self.funccontents = _callable_contents(execfunction)
        except AttributeError:
            try:
                # See if execfunction will do the heavy lifting for us.
                self.gc = execfunction.get_contents
            except AttributeError:
                # This is weird, just do the best we can.
                self.funccontents = _object_contents(execfunction)

        super().__init__(**kw)

    def function_name(self):
        try:
            return self.execfunction.__name__
        except AttributeError:
            try:
                return self.execfunction.__class__.__name__
            except AttributeError:
                return "unknown_python_function"

    def strfunction(self, target, source, env, executor: Executor | None = None):
        if self.cmdstr is None:
            return None
        if self.cmdstr is not _null:
            if executor:
                c = env.subst(self.cmdstr, SUBST_RAW, executor=executor)
            else:
                c = env.subst(self.cmdstr, SUBST_RAW, target, source)
            if c:
                return c

        def array(a):
            def quote(s):
                try:
                    str_for_display = s.str_for_display
                except AttributeError:
                    s = repr(s)
                else:
                    s = str_for_display()
                return s
            return '[' + ", ".join(map(quote, a)) + ']'
        try:
            strfunc = self.execfunction.strfunction
        except AttributeError:
            pass
        else:
            if strfunc is None:
                return None
            if callable(strfunc):
                return strfunc(target, source, env)
        name = self.function_name()
        tstr = array(target)
        sstr = array(source)
        return "%s(%s, %s)" % (name, tstr, sstr)

    def __str__(self) -> str:
        name = self.function_name()
        if name == 'ActionCaller':
            return str(self.execfunction)
        return "%s(target, source, env)" % name

    def execute(self, target, source, env, executor: Executor | None = None):
        exc_info = (None,None,None)
        try:
            if executor:
                target = executor.get_all_targets()
                source = executor.get_all_sources()
            rsources = list(map(rfile, source))
            try:
                result = self.execfunction(target=target, source=rsources, env=env)
            except (KeyboardInterrupt, SystemExit):
                raise
            except Exception as e:
                result = e
                exc_info = sys.exc_info()

            if result:
                result = SCons.Errors.convert_to_BuildError(result, exc_info)
                result.node = target
                result.action = self
                try:
                    result.command=self.strfunction(target, source, env, executor)
                except TypeError:
                    result.command=self.strfunction(target, source, env)

            return result
        finally:
            # Break the cycle between the traceback object and this
            # function stack frame. See the sys.exc_info() doc info for
            # more information about this issue.
            del exc_info

    def get_presig(self, target, source, env, executor: Executor | None = None):
        """Return the signature contents of this callable action."""
        try:
            return self.gc(target, source, env)
        except AttributeError:
            return self.funccontents

    def get_implicit_deps(self, target, source, env, executor: Executor | None = None):
        return []

class ListAction(ActionBase):
    """Class for lists of other actions."""
    def __init__(self, actionlist) -> None:
        if SCons.Debug.track_instances: logInstanceCreation(self, 'Action.ListAction')
        def list_of_actions(x):
            if isinstance(x, ActionBase):
                return x
            return Action(x)
        self.list = list(map(list_of_actions, actionlist))
        # our children will have had any varlist
        # applied; we don't need to do it again
        self.varlist = ()
        self.targets = '$TARGETS'

    def genstring(self, target, source, env, executor: Executor | None = None) -> str:
        return '\n'.join([a.genstring(target, source, env) for a in self.list])

    def __str__(self) -> str:
        return '\n'.join(map(str, self.list))

    def presub_lines(self, env):
        return SCons.Util.flatten_sequence(
            [a.presub_lines(env) for a in self.list])

    def get_presig(self, target, source, env, executor: Executor | None = None):
        """Return the signature contents of this action list.

        Simple concatenation of the signatures of the elements.
        """
        return b"".join([bytes(x.get_contents(target, source, env)) for x in self.list])

    def __call__(self, target, source, env, exitstatfunc=_null, presub=_null,
                 show=_null, execute=_null, chdir=_null, executor: Executor | None = None):
        if executor:
            target = executor.get_all_targets()
            source = executor.get_all_sources()
        for act in self.list:
            stat = act(target, source, env, exitstatfunc, presub,
                       show, execute, chdir, executor)
            if stat:
                return stat
        return 0

    def get_implicit_deps(self, target, source, env, executor: Executor | None = None):
        result = []
        for act in self.list:
            result.extend(act.get_implicit_deps(target, source, env))
        return result

    def get_varlist(self, target, source, env, executor: Executor | None = None):
        result = OrderedDict()
        for act in self.list:
            for var in act.get_varlist(target, source, env, executor):
                result[var] = True
        return list(result.keys())


class ActionCaller:
    """A class for delaying calling an Action function with specific
    (positional and keyword) arguments until the Action is actually
    executed.

    This class looks to the rest of the world like a normal Action object,
    but what it's really doing is hanging on to the arguments until we
    have a target, source and env to use for the expansion.
    """
    def __init__(self, parent, args, kw) -> None:
        self.parent = parent
        self.args = args
        self.kw = kw

    def get_contents(self, target, source, env):
        actfunc = self.parent.actfunc
        try:
            # "self.actfunc" is a function.
            contents = actfunc.__code__.co_code
        except AttributeError:
            # "self.actfunc" is a callable object.
            try:
                contents = actfunc.__call__.__func__.__code__.co_code
            except AttributeError:
                # No __call__() method, so it might be a builtin
                # or something like that.  Do the best we can.
                contents = repr(actfunc)

        return contents

    def subst(self, s, target, source, env):
        # If s is a list, recursively apply subst()
        # to every element in the list
        if is_List(s):
            result = []
            for elem in s:
                result.append(self.subst(elem, target, source, env))
            return self.parent.convert(result)

        # Special-case hack:  Let a custom function wrapped in an
        # ActionCaller get at the environment through which the action
        # was called by using this hard-coded value as a special return.
        if s == '$__env__':
            return env
        if is_String(s):
            return env.subst(s, 1, target, source)

        return self.parent.convert(s)

    def subst_args(self, target, source, env):
        return [self.subst(x, target, source, env) for x in self.args]

    def subst_kw(self, target, source, env):
        kw = {}
        for key in list(self.kw.keys()):
            kw[key] = self.subst(self.kw[key], target, source, env)
        return kw

    def __call__(self, target, source, env, executor: Executor | None = None):
        args = self.subst_args(target, source, env)
        kw = self.subst_kw(target, source, env)
        return self.parent.actfunc(*args, **kw)

    def strfunction(self, target, source, env):
        args = self.subst_args(target, source, env)
        kw = self.subst_kw(target, source, env)
        return self.parent.strfunc(*args, **kw)

    def __str__(self) -> str:
        return self.parent.strfunc(*self.args, **self.kw)


class ActionFactory:
    """A factory class that will wrap up an arbitrary function
    as an SCons-executable Action object.

    The real heavy lifting here is done by the ActionCaller class.
    We just collect the (positional and keyword) arguments that we're
    called with and give them to the ActionCaller object we create,
    so it can hang onto them until it needs them.
    """
    def __init__(self, actfunc, strfunc, convert=lambda x: x) -> None:
        self.actfunc = actfunc
        self.strfunc = strfunc
        self.convert = convert

    def __call__(self, *args, **kw):
        ac = ActionCaller(self, args, kw)
        action = Action(ac, strfunction=ac.strfunction)
        return action

# Local Variables:
# tab-width:4
# indent-tabs-mode:nil
# End:
# vim: set expandtab tabstop=4 shiftwidth=4:
