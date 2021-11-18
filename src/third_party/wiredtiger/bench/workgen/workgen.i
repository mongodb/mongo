/*-
 * Public Domain 2014-present MongoDB, Inc.
 * Public Domain 2008-2014 WiredTiger, Inc.
 *
 * This is free and unencumbered software released into the public domain.
 *
 * Anyone is free to copy, modify, publish, use, compile, sell, or
 * distribute this software, either in source code form or as a compiled
 * binary, for any purpose, commercial or non-commercial, and by any
 * means.
 *
 * In jurisdictions that recognize copyright laws, the author or authors
 * of this software dedicate any and all copyright interest in the
 * software to the public domain. We make this dedication for the benefit
 * of the public at large and to the detriment of our heirs and
 * successors. We intend this dedication to be an overt act of
 * relinquishment in perpetuity of all present and future rights to this
 * software under copyright law.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.
 * IN NO EVENT SHALL THE AUTHORS BE LIABLE FOR ANY CLAIM, DAMAGES OR
 * OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE,
 * ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR
 * OTHER DEALINGS IN THE SOFTWARE.
 */

/*
 * workgen.i
 *	The SWIG interface file defining the workgen python API.
 */

%include "typemaps.i"
%include "std_vector.i"
%include "std_string.i"
%include "stdint.i"
%include "attribute.i"
%include "carrays.i"

/* We only need to reference WiredTiger types. */
%import "wiredtiger.h"

%{
#include <ostream>
#include <sstream>
#include <signal.h>
#include "wiredtiger.h"
#include "workgen.h"
#include "workgen_int.h"
%}

%pythoncode %{
    import argparse,numbers,os,shutil,wiredtiger
%}

%exception {
	try {
		$action
	}
	catch (workgen::WorkgenException &wge) {
		SWIG_exception_fail(SWIG_RuntimeError, wge._str.c_str());
	}
}

/*
 * Some functions are long running, turn off signal handling that was enabled
 * by the Python interpreter.  This means that a signal handler coded in Python
 * won't work when spanning a call to one of these long running functions, but
 * it's doubtful our test scripts need signals at all.  This could be made to
 * work, it's just not worth the trouble.
 */
%define InterruptableFunction(funcname)
%exception funcname {
	try {
		void (*savesig)(int) = signal(SIGINT, SIG_DFL);
		$action
		(void)signal(SIGINT, savesig);
	}
	catch (workgen::WorkgenException &wge) {
		SWIG_exception_fail(SWIG_RuntimeError, wge._str.c_str());
	}
}
%enddef

/*
 * Define a __str__ function for all public workgen classes.
 */
%define WorkgenClass(classname)
%extend workgen::classname {
	const std::string __str__() {
		std::ostringstream out;
		$self->describe(out);
		return out.str();
	}
};
%enddef

/*
 * To forestall errors, make it impossible to add new attributes to certain
 * classes.  This trick relies on the implementation of SWIG providing
 * predictably named functions in the _workgen namespace to set attributes.
 */
%define WorkgenFrozenClass(classname)
%extend workgen::classname {
%pythoncode %{
    def __setattr__(self, attr, val):
        if attr != 'this' and getattr(self, attr) == None:
            raise AttributeError("'" + #classname +
              "' object has no attribute '" + attr + "'")
        object.__setattr__(self, attr, val)
%}
};
%enddef

InterruptableFunction(workgen::execute)
InterruptableFunction(workgen::Workload::run)

%module workgen
/* Parse the header to generate wrappers. */
%include "workgen.h"

%template(OpList) std::vector<workgen::Operation>;
%template(ThreadList) std::vector<workgen::Thread>;
%array_class(uint32_t, uint32Array);
%array_class(long, longArray);

WorkgenClass(Key)
WorkgenClass(Operation)
WorkgenClass(Stats)
WorkgenClass(Table)
WorkgenClass(TableOptions)
WorkgenClass(Thread)
WorkgenClass(ThreadOptions)
WorkgenClass(Transaction)
WorkgenClass(Value)
WorkgenClass(Workload)
WorkgenClass(WorkloadOptions)
WorkgenClass(Context)

WorkgenFrozenClass(TableOptions)
WorkgenFrozenClass(ThreadOptions)
WorkgenFrozenClass(WorkloadOptions)

%extend workgen::Context {
%pythoncode %{
    # This will be the actual __init__ function after we shuffle names below!
    def Xinit(self, parser = None):
        self.__original_init__()
        self._internal_init(parser)

    def _internal_init(self, parser):
        self.default_home = "WT_TEST"
        self.default_config = "create"
        if not parser:
            parser = argparse.ArgumentParser("Execute workgen.")
        parser.add_argument("--home", dest="home", type=str,
          help="home directory for the run (default=%s)" % self.default_home)
        parser.add_argument("--keep", dest="keep", action="store_true",
          help="Run the workload on an existing home directory")
        parser.add_argument("--verbose", dest="verbose", action="store_true",
          help="Run the workload verbosely")
        self.parser = parser
        self._initialized = False

    def parse_arguments(self, parser):
        self.args = parser.parse_args()

    def wiredtiger_open_config(self, config):
        return config

    def wiredtiger_open(self, config = None):
        if config == None:
            config = self.default_config
        self.initialize()
        return wiredtiger.wiredtiger_open(self.args.home, self.wiredtiger_open_config(config))

    def initialize(self):
        if not self._initialized:
            self.parse_arguments(self.parser)
            if self.args.home == None:
               self.args.home = self.default_home
            self._initialized = True
            if not self.args.keep:
                shutil.rmtree(self.args.home, True)
                os.mkdir(self.args.home)
        return self
%}
};

%pythoncode %{
# Shuffle the names of the __init__ function, we want ours (Xinit above), called first.
# This seems to be the most natural way to intercept a C++ constructor, and do
# Python-specific actions as part of the regular constructor.
Context.__original_init__ = Context.__init__
Context.__init__ = Context.Xinit
%}

%extend workgen::Operation {
%pythoncode %{
    def __mul__(self, other):
        if not isinstance(other, numbers.Integral):
            raise Exception('Operation.__mul__ requires an integral number')
        op = Operation()
        op._group = OpList([self])
        op._repeatgroup = other
        return op

    __rmul__ = __mul__

    def __add__(self, other):
        if not isinstance(other, Operation):
            raise Exception('Operation.__sum__ requires an Operation')
        if not self.combinable():
            op = Operation()
            op._group = OpList([self, other])
            op._repeatgroup = 1
            return op
        else:
            self._group.append(other)
            return self
%}
};

%extend workgen::Thread {
%pythoncode %{
    def __mul__(self, other):
        if not isinstance(other, numbers.Integral):
            raise Exception('Thread.__mul__ requires an integral number')
        return ThreadListWrapper(ThreadList([self] * other))

    __rmul__ = __mul__

    def __add__(self, other):
        if type(self) != type(other):
            raise Exception('Thread.__sum__ requires an Thread')
        return ThreadListWrapper(ThreadList([self, other]))
%}
};

%extend workgen::ThreadListWrapper {
%pythoncode %{
    def __mul__(self, other):
        if not isinstance(other, numbers.Integral):
            raise Exception('ThreadList.__mul__ requires an integral number')
        tlw = ThreadListWrapper(self)
        tlw.multiply(other)
        return tlw

    __rmul__ = __mul__

    def __add__(self, other):
        tlw = ThreadListWrapper(self)
        if isinstance(other, ThreadListWrapper):
            tlw.extend(other)
        elif isinstance(other, Thread):
            tlw.append(other)
        else:
            raise Exception('ThreadList.__sum__ requires an Thread or ThreadList')
        return tlw
%}
};

%extend workgen::Track {
%pythoncode %{
    def __longarray(self, size):
        result = longArray(size)
        result.__len__ = lambda: size
        return result

    def us(self):
        result = self.__longarray(1000)
        self._get_us(result)
        return result

    def ms(self):
        result = self.__longarray(1000)
        self._get_ms(result)
        return result

    def sec(self):
        result = self.__longarray(100)
        self._get_sec(result)
        return result
%}
};
