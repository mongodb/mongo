# Xsum library

The xsum library is imported from https://gitlab.com/radfordneal/xsum manually
the directory contains a reduced version of the xsum library as required for
the implementation of the
[Math.sumPrecise proposal](https://tc39.es/proposal-math-sum/#sec-math.sumprecise)
since the library isn't expected to have frequent updates, the manual vendoring
process was followed. In addition to importing select code a few modifications
were made to the code:

- The files were updated from .c files (as found in the original repository)
to c++ files, .cpp.
- The code was updated to not use C specific syntax specifically the orginal
code extensively used the ["restrict type qualifier"](https://en.cppreference.com/w/c/language/restrict)
whose usage has been removed to comply with C++ syntax.
