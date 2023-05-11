import os
import sys

EXPECTED_B_CPP = """// IWYU pragma: no_include "b.h"

#include "a.h" // IWYU pragma: keep

type_b return_b_function() {
    return type_b();
}
"""

with open('b.cpp') as f:
    content = f.read()
    if content != EXPECTED_B_CPP:
        print(f'Actual:\n"""{content}"""')
        print(f'Expected:\n"""{EXPECTED_B_CPP}"""')
        sys.exit(1)
