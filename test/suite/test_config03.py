#!/usr/bin/env python
#
# See the file LICENSE for redistribution information.
#
# Copyright (c) 2008-2012 WiredTiger, Inc.
#	All rights reserved.
#
# test_config03.py
# 	More configuration strings for wiredtiger_open,
#       combined probabilistically.
#

import unittest
from wiredtiger import WiredTigerError
import wiredtiger
import wttest
import test_base03
import wtscenario

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
    exclusive_scenarios = wtscenario.quick_scenarios('s_exclusive',
        [True,False], [0.2,1.0])
    hazard_max_scenarios = wtscenario.quick_scenarios('s_hazard_max',
        [15, 50, 500], [0.4, 0.8, 0.8])
    logging_scenarios = wtscenario.quick_scenarios('s_logging',
        [True,False], [1.0,1.0])
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
                    'eviction_trigger', 'exclusive', 'hazard_max', 'logging',
                    'multiprocess', 'session_max', 'transactional', 'verbose' ]

    all_scenarios = wtscenario.multiply_scenarios('_',
        cache_size_scenarios, create_scenarios, error_prefix_scenarios,
        eviction_target_scenarios, eviction_trigger_scenarios,
        exclusive_scenarios, hazard_max_scenarios, logging_scenarios,
        multiprocess_scenarios, session_max_scenarios,
        transactional_scenarios, verbose_scenarios)

    scenarios = wtscenario.prune_scenarios(all_scenarios, 1000)
    scenarios = wtscenario.number_scenarios(scenarios)

    print 'test_config03: running ' + str(len(scenarios)) + ' of ' + \
        str(len(all_scenarios)) + ' possible scenarios'

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
        if self.s_create == False:
            successargs = args.replace(',create=false,',',create,')
            expect_fail = True
        elif self.s_create == None:
            successargs = args + 'create=true,'
            expect_fail = True

        if expect_fail:
            #print 'wiredtiger_open (expected to fail) with args: ' + args
            self.assertRaises(WiredTigerError, lambda:
                                  wiredtiger.wiredtiger_open(dir, args))
            args = successargs

        #print 'wiredtiger_open with args: ' + args
        conn = wiredtiger.wiredtiger_open(dir, args)
        self.pr(`conn`)
        return conn

if __name__ == '__main__':
    wttest.run()
