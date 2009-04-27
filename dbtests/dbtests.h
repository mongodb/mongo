// dbtests.h : Test suite generator headers.
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

#include <unittest/UnitTest.hpp>

using namespace mongo;

UnitTest::TestPtr basicTests();
UnitTest::TestPtr btreeTests();
UnitTest::TestPtr jsTests();
UnitTest::TestPtr jsobjTests();
UnitTest::TestPtr jsonTests();
UnitTest::TestPtr matcherTests();
UnitTest::TestPtr namespaceTests();
UnitTest::TestPtr pairingTests();
UnitTest::TestPtr pdfileTests();
UnitTest::TestPtr queryTests();
UnitTest::TestPtr queryOptimizerTests();
UnitTest::TestPtr replTests();
UnitTest::TestPtr sockTests();
UnitTest::TestPtr updateTests();
