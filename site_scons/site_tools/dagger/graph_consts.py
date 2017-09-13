"""Constants for use in graph.py and dagger.py"""

"""Relationship edge types"""
LIB_LIB = 1
LIB_FIL = 2
FIL_LIB = 3
FIL_FIL = 4
FIL_SYM = 5
LIB_SYM = 6
IMP_LIB_LIB = 7
EXE_LIB = 8


"""NodeTypes"""
NODE_LIB = 1
NODE_SYM = 2
NODE_FILE = 3
NODE_EXE = 4

RELATIONSHIP_TYPES = range(1, 9)
NODE_TYPES = range(1, 5)


"""Error/query codes"""
NODE_NOT_FOUND = 1

