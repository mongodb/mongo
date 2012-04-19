// Copyright 2012.  10gen, Inc.

/**
 * This is a minimal utility program that you can use to smoke the printStackTrace
 * function, until such time as it is properly unit tested.
 */

#include "mongo/util/stacktrace.h"

int main(int argc, char **argv) {
    mongo::printStackTrace();
}
