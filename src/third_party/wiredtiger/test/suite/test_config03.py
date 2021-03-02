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

import wiredtiger, wtscenario, wttest
import test_base03

# test_config03.py
#    More configuration strings for wiredtiger_open, combined probabilistically.
class test_config03(test_base03.test_base03):
    K = 1024
    M = 1024 * K
    G = 1024 * M

    cache_size_scenarios = wtscenario.quick_scenarios('s_cache_size',
        [1*M,20*M,100*M,1*G,None], [0.6,0.6,0.6,0.6,0.6])
    create_scenarios = wtscenario.quick_scenarios('s_create',
        [True,False,None], [1.0,0.2,0.3])
    error_prefix_scenarios = wtscenario.quick_scenarios('s_error_prefix',
        [None,"errpfx:"], [1.0,0.2])
    # eviction_target < eviction_trigger -- checked later
    eviction_target_scenarios = wtscenario.quick_scenarios('s_eviction_target',
        [10, 40, 85, 98], None)
    eviction_trigger_scenarios = wtscenario.quick_scenarios(
        's_eviction_trigger',
        [50, 90, 95, 99], None)
    multiprocess_scenarios = wtscenario.quick_scenarios('s_multiprocess',
        [True,False], [1.0,1.0])
    session_max_scenarios = wtscenario.quick_scenarios('s_session_max',
        [3, 30, 300], None)
    transactional_scenarios = wtscenario.quick_scenarios('s_transactional',
        [True,False], [0.2,1.0])

    # Note: we are not using any truly verbose scenarios until we have
    # a way to redirect verbose output to a file in Python.
    #
    #verbose_scenarios = wtscenario.quick_scenarios('s_verbose',
    #    ['block', 'evict,evictserver', 'fileops,hazard,mutex',
    #     'read,readserver,reconcile,salvage','verify,write',''], None)
    verbose_scenarios = wtscenario.quick_scenarios('s_verbose', [None], None)

    config_vars = [ 'cache_size', 'create', 'error_prefix', 'eviction_target',
                    'eviction_trigger', 'multiprocess', 'session_max',
                    'verbose' ]

    scenarios = wtscenario.make_scenarios(
        cache_size_scenarios, create_scenarios, error_prefix_scenarios,
        eviction_target_scenarios, eviction_trigger_scenarios,
        multiprocess_scenarios, session_max_scenarios,
        transactional_scenarios, verbose_scenarios, prune=100, prunelong=1000)

    #wttest.WiredTigerTestCase.printVerbose(2, 'test_config03: running ' + \
    #                      str(len(scenarios)) + ' of ' + \
    #                      str(len(all_scenarios)) + ' possible scenarios')

    def setUpConnectionOpen(self, dir):
        args = ''
        # add names to args, e.g. args += ',session_max=30'
        for var in self.config_vars:
            value = getattr(self, 's_' + var)
            if value != None:
                if var == 'verbose':
                    value = '[' + str(value) + ']'
                if value == True:
                    value = 'true'
                if value == False:
                    value = 'false'
                args += ',' + var + '=' + str(value)
        args += ','
        self.pr('wiredtiger_open with args: ' + args)

        expect_fail = False
        successargs = args
        if self.s_create == False:
            successargs = successargs.replace(',create=false,',',create,')
            expect_fail = True
            fail_msg = '/(No such file or directory|The system cannot find the file specified)/'
        elif self.s_create == None:
            successargs = successargs + 'create=true,'
            expect_fail = True
            fail_msg = '/(No such file or directory|The system cannot find the file specified)/'

        if self.s_eviction_target >= self.s_eviction_trigger:
            # construct args that guarantee that target < trigger
            # we know that trigger >= 1
            repfrom = ',eviction_target=' + str(self.s_eviction_target)
            repto = ',eviction_target=' + str(self.s_eviction_trigger - 1)
            successargs = successargs.replace(repfrom, repto)
            if not expect_fail:
                expect_fail = True
                fail_msg = \
                    '/eviction target must be lower than the eviction trigger/'

        if expect_fail:
            self.verbose(3, 'wiredtiger_open (should fail) with args: ' + args)
            self.assertRaisesWithMessage(wiredtiger.WiredTigerError,
                lambda: wiredtiger.wiredtiger_open(dir, args), fail_msg)
            args = successargs

        self.verbose(3, 'wiredtiger_open with args: ' + args)
        conn = self.wiredtiger_open(dir, args)
        self.pr(repr(conn))
        return conn

if __name__ == '__main__':
    wttest.run()
