#!/usr/bin/env python
#
# Public Domain 2014-present MongoDB, Inc.
# Public Domain 2008-2014 WiredTiger, Inc.
#
# This is free and unencumbered software released into the public domain.
#
# Anyone is free to copy, modify, publish, use, compile, sell, or
# distribute this software, either in source code form or as a compiled
# binary, for any purpose, commercial or non-commercial, and by any
# means.
#
# In jurisdictions that recognize copyright laws, the author or authors
# of this software dedicate any and all copyright interest in the
# software to the public domain. We make this dedication for the benefit
# of the public at large and to the detriment of our heirs and
# successors. We intend this dedication to be an overt act of
# relinquishment in perpetuity of all present and future rights to this
# software under copyright law.
#
# THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
# EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
# MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.
# IN NO EVENT SHALL THE AUTHORS BE LIABLE FOR ANY CLAIM, DAMAGES OR
# OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE,
# ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR
# OTHER DEALINGS IN THE SOFTWARE.

import wiredtiger, wttest
from wtscenario import make_scenarios

# test_join10.py
#    Test modeled on the C example for join.
class test_join10(wttest.WiredTigerTestCase):

    # We need statistics for these tests.
    conn_config = 'statistics=(all)'

    def check_stats(self, jc, expect):
        statcursor = self.session.open_cursor('statistics:join', jc, None)
        for id, desc, valstr, val in statcursor:
            if desc in expect:
                expect_val = expect.pop(desc)
                self.assertEqual(val, expect_val)
        self.assertTrue(len(expect) == 0,
                        'missing expected values in stats: ' + str(expect))
        statcursor.close()

    def test_country(self):
        session = self.session
        pop_data = [["AU", 1900, 4000000],
                    ["AU", 1950, 8267337],
                    ["AU", 2000, 19053186],
                    ["CAN", 1900, 5500000],
                    ["CAN", 1950, 14011422],
                    ["CAN", 2000, 31099561],
                    ["UK", 1900, 369000000],
                    ["UK", 1950, 50127000],
                    ["UK", 2000, 59522468],
                    ["USA", 1900, 76212168],
                    ["USA", 1950, 150697361],
                    ["USA", 2000, 301279593]]

        session.create("table:poptable", \
                       "key_format=r,value_format=SHQ," +
                       "columns=(id,country,year,population),colgroups=(main,population)")

        session.create("colgroup:poptable:main", "columns=(country,year,population)")
        session.create("colgroup:poptable:population", "columns=(population)")

        session.create("index:poptable:country", "columns=(country)")
        session.create("index:poptable:immutable_year", "columns=(year),immutable")

        cursor = session.open_cursor("table:poptable", None, "append")
        for p in pop_data:
            cursor.set_value(p[0], p[1], p[2])
            cursor.insert()
        cursor.close()

        join_cursor = session.open_cursor("join:table:poptable")
        country_cursor = session.open_cursor("index:poptable:country")
        year_cursor = session.open_cursor("index:poptable:immutable_year")

        # select values WHERE country == "AU" AND year > 1900
        country_cursor.set_key("AU")
        self.assertEqual(country_cursor.search(), 0)
        session.join(join_cursor, country_cursor, "compare=eq,count=10")
        year_cursor.set_key(1900)
        self.assertEqual(year_cursor.search(), 0)
        session.join(join_cursor, year_cursor, "compare=gt,count=10,strategy=bloom")

        # Check results
        expect = [[c,y,p] for c,y,p in pop_data if c == "AU" and y > 1900]
        got = []
        for recno, country, year, population in join_cursor:
            got.append([country, year, population])
        self.assertEqual(expect, got)

        # Check statistics
        # It may seem odd to encode specific values to check against, but each of these
        # statistics represent significant and predictable events in the code, and we
        # want to know if anything changes.
        expect_stat = dict()
        pfxc = 'join: index:poptable:country: '
        pfxy = 'join: index:poptable:immutable_year: '

        expect_stat[pfxc + 'accesses to the main table'] = 2
        expect_stat[pfxc + 'bloom filter false positives'] = 0
        expect_stat[pfxc + 'checks that conditions of membership are satisfied'] = 4
        expect_stat[pfxc + 'items inserted into a bloom filter'] = 0
        expect_stat[pfxc + 'items iterated'] = 4

        # We're using a bloom filter on this one, but we don't check for bloom filter
        # false positives, it's a bit tied to implementation details.
        expect_stat[pfxy + 'accesses to the main table'] = 2
        expect_stat[pfxy + 'checks that conditions of membership are satisfied'] = 3
        expect_stat[pfxy + 'items inserted into a bloom filter'] = 8
        expect_stat[pfxy + 'items iterated'] = 12

        self.check_stats(join_cursor, expect_stat)

        join_cursor.close()
        year_cursor.close()
        country_cursor.close()

        # Complex join cursors
        join_cursor = session.open_cursor("join:table:poptable")
        subjoin_cursor = session.open_cursor("join:table:poptable")

        country_cursor = session.open_cursor("index:poptable:country")
        country_cursor2 = session.open_cursor("index:poptable:country")
        year_cursor = session.open_cursor("index:poptable:immutable_year")

        # select values WHERE (country == "AU" OR country == "UK")
        #                     AND year > 1900

        # First, set up the join representing the country clause.
        country_cursor.set_key("AU")
        self.assertEqual(country_cursor.search(), 0)
        session.join(subjoin_cursor, country_cursor, "operation=or,compare=eq,count=10")

        country_cursor2.set_key("UK")
        self.assertEqual(country_cursor2.search(), 0)
        session.join(subjoin_cursor, country_cursor2, "operation=or,compare=eq,count=10")

        # Join that to the top join, and add the year clause
        session.join(join_cursor, subjoin_cursor)
        year_cursor.set_key(1900)
        self.assertEqual(year_cursor.search(), 0)

        session.join(join_cursor, year_cursor, "compare=gt,count=10,strategy=bloom")

        # Check results
        expect = [[c,y,p] for c,y,p in pop_data if (c == "AU" or c == "UK") and y > 1900]
        got = []
        for recno, country, year, population in join_cursor:
            got.append([country, year, population])
        self.assertEqual(expect, got)

        expect_stat = dict()

        # Note: the stats collected for the clause (country == "AU" OR country == "UK")
        # are in a join entry that is a "subjoin".  Due to a quirk in the implementation
        # of join statistics, subjoin statistics are returned using the main table prefix.
        pfxm = 'join: table:poptable: '
        pfxy = 'join: index:poptable:immutable_year: '

        expect_stat[pfxm + 'accesses to the main table'] = 4
        expect_stat[pfxm + 'bloom filter false positives'] = 0
        expect_stat[pfxm + 'checks that conditions of membership are satisfied'] = 12
        expect_stat[pfxm + 'items inserted into a bloom filter'] = 0
        expect_stat[pfxm + 'items iterated'] = 0

        expect_stat[pfxy + 'accesses to the main table'] = 6
        expect_stat[pfxy + 'bloom filter false positives'] = 0
        expect_stat[pfxy + 'checks that conditions of membership are satisfied'] = 6
        expect_stat[pfxy + 'items inserted into a bloom filter'] = 0
        expect_stat[pfxy + 'items iterated'] = 0

        self.check_stats(join_cursor, expect_stat)

        join_cursor.close()
        subjoin_cursor.close()
        country_cursor.close()
        country_cursor2.close()
        year_cursor.close()

if __name__ == '__main__':
    wttest.run()
