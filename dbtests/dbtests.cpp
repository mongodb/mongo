// dbtests.cpp : Runs db unit tests.
//

/**
*    Copyright (C) 2008 10gen Inc.
*  
*    This program is free software: you can redistribute it and/or  modify
*    it under the terms of the GNU Affero General Public License, version 3,
*    as published by the Free Software Foundation.
*  
*    This program is distributed in the hope that it will be useful,
*    but WITHOUT ANY WARRANTY; without even the implied warranty of
*    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
*    GNU Affero General Public License for more details.
*  
*    You should have received a copy of the GNU Affero General Public License
*    along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/

#include "dbtests.h"

#include <unittest/Registry.hpp>

using namespace std;

extern const char* dbpath;

// Maybe unit tests deserve a separate binary?
int main( int argc, char** argv ) {
  dbpath = "/tmp/unittest/";

  UnitTest::Registry tests;

  tests.add( btreeTests(), "btree" );

  return tests.run( argc, argv );
}

