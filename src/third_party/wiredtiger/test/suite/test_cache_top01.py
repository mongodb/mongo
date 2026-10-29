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

import re, time
import wiredtiger
import wttest

# The rankings name the data handle behind a table, and which handle that is depends on the running
# configuration: a tiered table is named by its tiered URI, and a layered table has separate ingest
# and stable constituents. Accept any of the handles a table can legitimately be reported under,
# including whichever backing file the running hook says a table starts life in.
def table_names(testcase, base):
    names = ['file:%s.wt' % base, 'file:%s.wt_stable' % base, 'file:%s.wt_ingest' % base,
        'tiered:%s' % base]
    initial = testcase.initialFileName('table:' + base)
    if initial is not None:
        names.append('file:' + initial)
    return names

# Eviction is deliberately given nothing to do. A table's resident bytes are only a stable thing to
# assert on when eviction is not free to take them away underneath the test.
NO_EVICTION = ('eviction_dirty_target=75,eviction_dirty_trigger=90,'
    'eviction_updates_target=70,eviction_updates_trigger=85')

# One report format, one parser, one set of workload helpers, shared by every class below. Only the
# connection configuration separates the classes.
class cache_top_base(wttest.WiredTigerTestCase):
    # The number of slots each ranking has, which bounds how many tables it can hold.
    slots = 32

    # How many entries a ranking prints below DEBUG_2, where the whole ranking is more than an
    # operator watching a log wants to read.
    verbose_entries = 5

    # Every ranking the report is expected to produce, in the order it reports them.
    rankings = ['dirty leaf bytes', 'recent bytes evicted', 'total cache bytes',
        'recent bytes read', 'update bytes']

    # Rankings of a level, which can also report a connection-wide total. The rest track a decayed
    # flow, which has no connection-wide equivalent.
    level_rankings = ['dirty leaf bytes', 'total cache bytes', 'update bytes']

    value = 'v' * 4096

    header_re = re.compile(r'cache top (?P<ranking>.+?): (?P<count>\d+) tables above '
        r'(?P<threshold>\d+)B hold (?P<listed>\d+)B'
        r'(?: of (?P<total>\d+)B in use, (?P<configured>\d+)B configured)?$')
    # A verbose report prefixes every line with a timestamp and category, so an entry is found by
    # the report's own indentation rather than at the start of the line.
    entry_re = re.compile(r'\s{4,}(?P<value>\d+)B (?P<name>\S+)$')

    def populate(self, uri, rows, start = 0, config = 'key_format=S,value_format=S',
            checkpoint = False):
        self.session.create(uri, config)
        c = self.session.open_cursor(uri)
        recno = 'key_format=r' in config
        for i in range(start, start + rows):
            c[i + 1 if recno else 'k%08d' % i] = self.value
        c.close()
        # Checkpointing leaves the pages clean. Dirty eviction runs against a target that is a
        # fraction of the cache, so a table left dirty can lose its resident bytes at any moment;
        # clean pages are only evicted under cache pressure, which these tests stay clear of.
        if checkpoint:
            self.session.checkpoint()

    def read_all(self, uri):
        c = self.session.open_cursor(uri)
        for _ in c:
            pass
        c.close()

    def read_rows(self, uri, lo, hi):
        c = self.session.open_cursor(uri)
        for i in range(lo, hi):
            c.set_key('k%08d' % i)
            c.search()
        c.close()

    # The text of a report asked for directly.
    def report_text(self):
        self.cleanStdout()
        self.conn.debug_info('cache_top')
        out = self.readStdout(200000)
        self.cleanStdout()
        return out

    # Parse report text into {ranking: {count, threshold, listed, total, entries}}, where entries
    # is a list of (bytes, name) in the order reported.
    def parse_report(self, text):
        report = {}
        ranking = None
        for line in text.splitlines():
            header = self.header_re.search(line)
            if header is not None:
                ranking = header.group('ranking')
                report[ranking] = {
                    'count': int(header.group('count')),
                    'threshold': int(header.group('threshold')),
                    'listed': int(header.group('listed')),
                    'total': None if header.group('total') is None
                        else int(header.group('total')),
                    'configured': None if header.group('configured') is None
                        else int(header.group('configured')),
                    'entries': [],
                }
                continue
            # A capture can begin midway through a report, so entries arriving before any
            # header belong to a ranking this text does not have.
            entry = self.entry_re.search(line)
            if entry is not None and ranking is not None:
                report[ranking]['entries'].append(
                    (int(entry.group('value')), entry.group('name')))
        return report

    def report(self):
        return self.parse_report(self.report_text())

    def names(self, report, ranking):
        return [name for _, name in report[ranking]['entries']]

    # One ranking of a fresh report, as (threshold, entries).
    def ranking(self, name):
        r = self.report()[name]
        return r['threshold'], r['entries']

    def assertTableIn(self, base, names):
        self.assertTrue(any(n in names for n in table_names(self, base)),
            '%s not found in %s' % (base, names))

    def assertTableNotIn(self, base, names):
        for n in table_names(self, base):
            self.assertNotIn(n, names)

    # A connection names its history store one of two ways.
    internal_names = ['WiredTiger.wt', 'WiredTigerHS.wt', 'WiredTigerSharedHS.wt_stable']

    # The tables WiredTiger keeps for itself are the connection statistics' business, not the
    # operator's, and are never named.
    def assertInternalTablesNotIn(self, report):
        for ranking in self.rankings:
            for name in self.names(report, ranking):
                for internal in self.internal_names:
                    self.assertNotIn(internal, name)

    # Every report is expected to hold together internally, whatever the workload. A report that
    # printed only the first few entries of each ranking cannot be checked against the totals on
    # its header lines, which cover the whole ranking.
    def check_report_consistent(self, report, truncated = False):
        self.assertInternalTablesNotIn(report)

        for ranking in self.rankings:
            self.assertIn(ranking, report, 'ranking missing from the report: ' + ranking)
            r = report[ranking]

            # A tree occupies at most one slot of a ranking, so a table is named at most once.
            names = self.names(report, ranking)
            self.assertEqual(len(names), len(set(names)),
                'ranking "%s" names a table more than once: %s' % (ranking, names))

            # A ranking can hold no more than it has slots for, and prints no more than it is
            # allowed to.
            self.assertLessEqual(r['count'], self.slots)
            self.assertLessEqual(len(r['entries']),
                self.verbose_entries if truncated else self.slots)

            if not truncated:
                # The count on the header line is the number of entries that follow, and the
                # listed bytes are their sum.
                self.assertEqual(r['count'], len(r['entries']))
                self.assertEqual(r['listed'], sum(value for value, _ in r['entries']))

            # Entries are ordered largest first. Which named table leads is deliberately not
            # asserted anywhere: how many bytes a table has resident at any moment depends on when
            # eviction last ran, so only the ordering of the reported values is a guarantee.
            values = [value for value, _ in r['entries']]
            self.assertEqual(values, sorted(values, reverse = True))

            # Every entry is at or above the threshold, and every name is a data handle.
            for value, name in r['entries']:
                self.assertGreaterEqual(value, r['threshold'])
                self.assertTrue(name.startswith('file:') or name.startswith('tiered:'),
                    'unexpected name: ' + name)

            # A ranking of a level reports what the connection holds alongside what it listed; a
            # flow has no connection-wide equivalent and reports neither that nor a cache size.
            if ranking in self.level_rankings:
                self.assertIsNotNone(r['total'])
            else:
                self.assertIsNone(r['total'])

    # Asking for a ranking nothing qualifies for lowers its bar. This alone admits nothing: a
    # table that was too small when it was written stays out until it is written to again.
    def lower_threshold(self, ranking, reports = 8):
        for _ in range(reports):
            threshold, _ = self.ranking(ranking)
        return threshold

    # A verbose report is emitted by a background server, so poll for it with a deadline rather
    # than sleeping for one. Accumulates output until a whole report has arrived, since the server
    # can be caught midway through printing one.
    def wait_for_report(self, msg, timeout = 60):
        deadline = time.time() + timeout
        text = ''
        while True:
            text += self.readStdout(200000)
            self.cleanStdout()
            if all(r in self.parse_report(text) for r in self.rankings):
                return text
            self.assertLess(time.time(), deadline, msg)
            time.sleep(0.1)

# The rankings of the tables consuming the most cache, as reported by WT_CONNECTION::debug_info.
class test_cache_top01(cache_top_base):
    conn_config = 'create,cache_size=100MB,statistics=(all),' + NO_EVICTION

    # A report against an untouched connection produces every ranking, all empty.
    def test_report_empty(self):
        report = self.report()
        self.check_report_consistent(report)
        for ranking in self.rankings:
            self.assertEqual(report[ranking]['count'], 0)

    # A table large enough to matter is named; one holding almost nothing is not. A column store is
    # ranked the same way a row store is, the rankings being counted in bytes rather than keys.
    def test_tables_reported_by_size(self):
        self.populate('table:rows', 2500, checkpoint = True)
        self.populate('table:columns', 2500, config = 'key_format=r,value_format=S',
            checkpoint = True)
        self.populate('table:small', 10, checkpoint = True)

        report = self.report()
        self.check_report_consistent(report)

        resident = self.names(report, 'total cache bytes')
        self.assertTableIn('rows', resident)
        self.assertTableIn('columns', resident)
        self.assertTableNotIn('small', resident)

    # Successive committed versions of every key push the older ones into the history store, then
    # reading at an old timestamp brings them back out, so it takes read and eviction traffic as
    # well as the writes.
    def push_versions(self, uri, rows, timestamps):
        for ts in timestamps:
            c = self.session.open_cursor(uri)
            for i in range(rows):
                self.session.begin_transaction()
                c['k%08d' % i] = self.value
                self.session.commit_transaction('commit_timestamp=' + self.timestamp_str(ts))
            c.close()
        self.session.checkpoint()

        for ts in timestamps:
            self.session.begin_transaction('read_timestamp=' + self.timestamp_str(ts))
            self.read_all(uri)
            self.session.rollback_transaction()

    # The history store and the metadata stay out of the rankings even when the history store is
    # the hottest tree in the cache.
    def test_internal_tables_excluded_under_load(self):
        uri = 'table:hsload'
        self.session.create(uri, 'key_format=S,value_format=S')
        self.conn.set_timestamp('oldest_timestamp=' + self.timestamp_str(1) +
            ',stable_timestamp=' + self.timestamp_str(1))

        # How much of the cache the history store holds moves with eviction, so check a report
        # after each round rather than only once at the end.
        ts = 2
        for _ in range(3):
            self.push_versions(uri, 400, range(ts, ts + 8))
            ts += 8
            self.check_report_consistent(self.report())

        # The checks above are only meaningful if the workload used the history store.
        self.assertGreater(self.get_stat(wiredtiger.stat.conn.cache_hs_insert), 0)

    # A table dropped while it is being reported leaves the ranking without taking the connection
    # with it.
    def test_drop_while_reported(self):
        self.populate('table:doomed', 2500, checkpoint = True)
        self.assertTableIn('doomed', self.names(self.report(), 'total cache bytes'))

        self.session.drop('table:doomed')

        report = self.report()
        self.check_report_consistent(report)
        for ranking in self.rankings:
            self.assertTableNotIn('doomed', self.names(report, ranking))

    # Altering a table closes and reopens it underneath the rankings. A table must still be named
    # at most once afterwards.
    @wttest.skip_for_hook("disagg", "session.alter is not supported for layered tables")
    def test_no_duplicates_across_handle_reopen(self):
        self.populate('table:churn', 2500, checkpoint = True)

        # The check below is only meaningful if the table is large enough to be ranked at all.
        self.assertTableIn('churn', self.names(self.report(), 'total cache bytes'))

        self.session.alter('table:churn', 'access_pattern_hint=random')
        self.populate('table:churn', 2500, start = 50000, checkpoint = True)

        self.check_report_consistent(self.report())

    # More tables hold cache than a ranking has slots for. The report stays bounded, and the slots
    # go to the tables that hold the cache rather than to whichever called in first.
    def test_many_tables_ranks_largest(self):
        bulk, tiny = 34, 6

        # A ranking opens with a bar no table here would clear, and only lowers it when asked for
        # a report. Lower it before these tables exist, so every one of them is weighed against
        # the lowered bar the first time it holds anything.
        self.populate('table:primer', 200)
        self.assertEqual(self.report()['total cache bytes']['count'], 0)
        self.lower_threshold('total cache bytes')

        # More tables now hold cache than the ranking has slots, so it has to choose between them.
        # Each has to grow by a minimum step before it is reconsidered.
        for i in range(bulk):
            self.populate('table:bulk%02d' % i, 400)
        for i in range(tiny):
            self.populate('table:tiny%d' % i, 5)
        self.session.checkpoint()

        report = self.report()
        self.check_report_consistent(report)
        resident = self.names(report, 'total cache bytes')

        # It filled the ranking, and filled it with the tables that hold the cache: a table
        # holding almost nothing never displaces one that does. How full it stays is deliberately
        # left loose.
        self.assertGreaterEqual(len(resident), self.slots // 2)
        for name in resident:
            self.assertTrue(any(name in table_names(self, 'bulk%02d' % i) for i in range(bulk)),
                'a table holding almost nothing took a slot: %s' % name)

    # A threshold nothing reaches is lowered until it says something, and never below its floor.
    def test_threshold_lowers_when_nothing_qualifies(self):
        self.populate('table:modest', 200)

        thresholds = [self.ranking('total cache bytes')[0] for _ in range(12)]

        self.assertEqual(thresholds, sorted(thresholds, reverse = True),
            'threshold rose while nothing qualified: %s' % thresholds)
        self.assertLess(thresholds[-1], thresholds[0])
        # The threshold has a small nonzero floor, so it can fall only so far, not to zero.
        self.assertGreaterEqual(thresholds[-1], 4 * 1024)

    # Reading a table back into an empty cache puts it in the read ranking.
    def test_read_ranking(self):
        self.populate('table:reread', 2500, checkpoint = True)

        # Reopening leaves the cache empty, so the scan below has to read every page.
        self.reopen_conn()
        self.read_all('table:reread')

        report = self.report()
        self.check_report_consistent(report)
        self.assertTableIn('reread', self.names(report, 'recent bytes read'))

    # A table being evicted from appears in the eviction ranking.
    def test_evict_ranking(self):
        # A cache this small cannot hold the data, so eviction has to run.
        self.conn.reconfigure('cache_size=10MB')
        self.populate('table:evicted', 4000)

        report = self.report()
        self.check_report_consistent(report)
        self.assertTableIn('evicted', self.names(report, 'recent bytes evicted'))

    # Update memory is attributed to the table that holds it.
    def test_update_ranking(self):
        self.populate('table:updates', 2500)

        report = self.report()
        self.check_report_consistent(report)
        self.assertTableIn('updates', self.names(report, 'update bytes'))

    # The report survives the cache being resized underneath it, which is where the threshold
    # comes from, and names the size it was last told about.
    def test_cache_resize(self):
        self.populate('table:resized', 2000)

        for mb in [500, 20]:
            self.conn.reconfigure('cache_size=%dMB' % mb)
            report = self.report()
            self.check_report_consistent(report)
            self.assertEqual(report['total cache bytes']['configured'], mb * 1024 * 1024)

    # The rankings coexist with the rest of what debug_info prints.
    def test_combined_with_other_categories(self):
        self.populate('table:combined', 2500, checkpoint = True)

        self.cleanStdout()
        self.conn.debug_info('cache_top=true,handles=true')
        out = self.readStdout(200000)
        self.cleanStdout()
        self.assertIn('cache top ', out)
        self.assertIn('Data handle dump', out)

    # The rankings belong to a connection and start empty on the next one.
    def test_reset_on_reopen(self):
        self.populate('table:transient', 2500, checkpoint = True)
        self.assertTableIn('transient', self.names(self.report(), 'total cache bytes'))

        self.reopen_conn()

        report = self.report()
        self.check_report_consistent(report)
        for ranking in self.rankings:
            self.assertTableNotIn('transient', self.names(report, ranking))

# The same rankings, delivered through the verbose category rather than on request.
class test_cache_top02(cache_top_base):
    # A short sweep interval so the server that emits the report comes around promptly. The
    # category is left off here: each test below turns it on the way it means to test.
    conn_config = ('create,cache_size=100MB,statistics=(all),'
        'file_manager=(close_scan_interval=1),' + NO_EVICTION)

    # The server can emit a report at any point once the category is on, including while the
    # connection is closing, which is after the last chance a test has to consume it. Turning the
    # category off is not a barrier either. Ignore the asynchronous reports for the whole test
    # instead: only they carry the category tag, so a report asked for directly is still checked.
    def setUp(self):
        super().setUp()
        self.ignoreStdoutPattern('WT_VERB_CACHE_TOP')

    # The category named in the configuration the connection was opened with.
    def test_verbose_from_connection_config(self):
        self.reopen_conn(config = self.conn_config + ',verbose=[cache_top]')
        self.populate('table:verbose', 2000, checkpoint = True)

        self.wait_for_report('no cache_top verbose report within the deadline')
        self.conn.reconfigure('verbose=[]')

    # Turning the category on and off on a running connection, which is how it is reached in the
    # field.
    def test_verbose_at_runtime(self):
        self.populate('table:runtime', 2000, checkpoint = True)

        # Nothing has asked for the rankings yet.
        self.cleanStdout()
        self.conn.reconfigure('verbose=[cache_top]')
        text = self.wait_for_report('no report after enabling the category at runtime')

        # A report the server emitted holds together the same way a requested one does, except
        # that it prints only the first few entries of each ranking.
        self.check_report_consistent(self.parse_report(text), truncated = True)

        # Turning it back off is accepted, and asking directly still works.
        self.conn.reconfigure('verbose=[]')
        self.assertIn('cache top ', self.report_text())

# The rankings on a connection that has no disk behind it.
class test_cache_top03(cache_top_base):
    conn_config = 'create,cache_size=100MB,in_memory=true'

    def test_in_memory(self):
        self.populate('table:inmemory', 2000)

        report = self.report()
        self.check_report_consistent(report)
        self.assertTableIn('inmemory', self.names(report, 'total cache bytes'))

# A ranking's threshold can fall far below where it stood when a tree last called in. A tree only
# returns to the ranking if its recheck value comes down with the threshold, which happens on a
# different path for a decayed flow than for a level read from the tree's counters.
class test_cache_top04(cache_top_base):
    # A large cache, so a ranking opens with a threshold well above anything these tests drive. The
    # threshold is derived once, on first use, and only ever adjusted downwards from there.
    conn_config = 'create,cache_size=1GB,statistics=(all)'

    # A tree is admitted to the ranking only once its value came down with the threshold, not
    # because it grew past the threshold the ranking opened with.
    def check_admitted_below(self, entries, opening, base):
        names = [name for _, name in entries]
        self.assertTableIn(base, names)
        for value, name in entries:
            if name in table_names(self, base):
                self.assertLess(value, opening)

    # Bytes read accumulate and decay, and the recheck value comes down as the tree reads more.
    def test_read_ranking_after_threshold_falls(self):
        uri = 'table:cooling'
        self.populate(uri, 400, checkpoint = True)

        # Reopen so the reads below come from disk rather than out of cache.
        self.reopen_conn()

        # Read well under the ranking's opening threshold, so the table cannot qualify yet.
        self.read_rows(uri, 0, 100)
        opening, entries = self.ranking('recent bytes read')
        self.assertEqual(entries, [],
            'the table qualified before the threshold fell, so this proves nothing: %s' % entries)

        self.assertLess(self.lower_threshold('recent bytes read'), opening)

        # Reading more must now put the table in the ranking.
        self.read_rows(uri, 200, 300)
        self.check_admitted_below(self.ranking('recent bytes read')[1], opening, 'cooling')

    # Resident bytes are read straight from the tree's counters, and eviction on the tree is what
    # brings its recheck value down.
    def test_resident_ranking_after_threshold_falls(self):
        uri = 'table:cooling'
        # Small enough to stay under the ranking's opening threshold.
        self.populate(uri, 300, checkpoint = True)

        opening, entries = self.ranking('total cache bytes')
        self.assertTableNotIn('cooling', [name for _, name in entries])

        self.assertLess(self.lower_threshold('total cache bytes'), opening)

        # Shrink the cache so eviction runs on the tree, which is what lowers its recheck value.
        self.conn.reconfigure('cache_size=1MB')

        # Eviction is a background thread, so poll: reading pages back in both accounts for them
        # and gives the tree a chance to be admitted.
        deadline = time.time() + 60
        while True:
            self.read_all(uri)
            _, entries = self.ranking('total cache bytes')
            if any(n in [name for _, name in entries] for n in table_names(self, 'cooling')):
                break
            self.assertLess(time.time(), deadline,
                'table never returned to the resident ranking after the threshold fell')
            time.sleep(0.5)

        self.check_admitted_below(entries, opening, 'cooling')

# The same rankings as a connection statistic, which is reachable without verbose logging or an
# explicit request. Recomputed at the end of every checkpoint.
class test_cache_top05(cache_top_base):
    cache_size_mb = 100
    conn_config = 'create,cache_size=%dMB,statistics=(all),' % cache_size_mb + NO_EVICTION

    # Each ranking that publishes a share of the cache, as (whole ranking, largest few).
    pct_stats = [
        ('cache_top_inuse_pct', 'cache_top5_inuse_pct'),
        ('cache_top_updates_pct', 'cache_top5_updates_pct'),
        ('cache_top_dirty_pct', 'cache_top5_dirty_pct'),
    ]

    # The share of the configured cache the ranked tables hold is published, and agrees with the
    # report built from the same rankings.
    def test_concentration_published(self):
        self.populate('table:hog', 4000, checkpoint = True)

        pcts = {name: self.get_stat(getattr(wiredtiger.stat.conn, name))
            for pair in self.pct_stats for name in pair}

        # A share of the cache cannot exceed the cache.
        for name, pct in pcts.items():
            self.assertLessEqual(pct, 100, name)

        # The largest few are a subset of the whole ranking, so they can never account for more.
        for whole, top5 in self.pct_stats:
            self.assertLessEqual(pcts[top5], pcts[whole], top5)

        # One table holds what is in the cache here, so the ranked tables have to account for a
        # real share of it. A bound rather than a value: the share is integer-divided, so what
        # this rules out is the ranking having nothing in it at all.
        self.assertGreater(pcts['cache_top_inuse_pct'],
            0, 'no cache attributed to the ranked tables')

        # The statistic and the report are separate observations of a moving cache, so compare
        # them loosely: both have to agree on how much of the cache the same ranking holds, and
        # both have to measure it against the configured cache size rather than against the bytes
        # the cache happens to hold.
        cache_bytes = self.cache_size_mb * 1024 * 1024
        r = self.report()['total cache bytes']
        self.assertEqual(r['configured'], cache_bytes,
            'the report does not name the configured cache size')
        from_report = r['listed'] * 100 // r['configured']
        self.assertLess(abs(pcts['cache_top_inuse_pct'] - from_report), 10,
            'statistic and report disagree: %d vs %d' % (pcts['cache_top_inuse_pct'], from_report))
