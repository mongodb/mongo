// @file test.cpp

/* unit tests for mongoutils 
*/

#include "str.h"

#include "html.h"

#include <assert.h>

using namespace std;
using namespace mongoutils;

int main() {
    string x = str::after("abcde", 'c');
    assert( x == "de" );
    assert( str::after("abcde", 'x') == "" );
}

