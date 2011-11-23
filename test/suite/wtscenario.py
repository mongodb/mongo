#!/usr/bin/env python
#
# See the file LICENSE for redistribution information.
#
# Copyright (c) 2011 WiredTiger, Inc.
#	All rights reserved.
#
# wtscenarios.py
# 	Support scenarios based testing
#

import testscenarios
import suite_random

def powerrange(start, stop, mult):
    """
    Like xrange, generates a range from start to stop.
    Unlike xrange, the range is inclusive of stop,
    each step is multiplicative, and as a special case,
    the stop value is returned as the last item.
    """
    val = start
    while val <= stop:
        yield val
        newval = val * mult
        if val < stop and newval > stop:
            val = stop
        else:
            val = newval

def log2chr(val):
    """
    For the log-base 2 of val, return the numeral or letter
    corresponding to val (which is < 36).  Hence, 1 return '0',
    2 return '1', 2*15 returns 'f', 2*16 returns 'g', etc.
    """
    p = 0
    while val >= 2:
        p += 1
        val /= 2
    if p < 10:
        return chr(ord('0') + p)
    else:
        return chr(ord('a') + p - 10)
    
megabyte = 1024 * 1024

def multiply_scenarios(sep, *args):
    """
    Create the cross product of two lists of scenarios
    """
    result = None
    for scenes in args:
        if result == None:
            result = scenes
        else:
            total = []
            for scena in scenes:
                for scenb in result:
                    # Create a merged scenario with a concatenated name
                    name = scena[0] + sep + scenb[0]
                    tdict = {}
                    tdict.update(scena[1])
                    tdict.update(scenb[1])

                    # If there is a 'P' value, it represents the
                    # probability that we want to use this scenario
                    # If both scenarios list a probability, multiply them.
                    if 'P' in scena[1] and 'P' in scenb[1]:
                        P = scena[1]['P'] * scenb[1]['P']
                        tdict['P'] = P
                    total.append((name, tdict))
            result = total
    return result

def prune_scenarios(scenes):
    """
    Use listed probabilities for pruning the list of scenarios
    """
    r = suite_random.suite_random()
    result = []
    for scene in scenes:
        if 'P' in scene[1]:
            p = scene[1]['P']
            if p < r.rand_float():
                continue
        result.append(scene)
    return result

def quick_scenarios(fieldname, values, probabilities):
    """
    Quickly build common scenarios, like:
       [('foo', dict(somefieldname='foo')),
       ('bar', dict(somefieldname='bar')),
       ('boo', dict(somefieldname='boo'))]
    via a call to:
       quick_scenario('somefieldname', ['foo', 'bar', 'boo'])
    """
    result = []
    if probabilities == None:
        plen = 0
    else:
        plen = len(probabilities)
    ppos = 0
    for value in values:
        if ppos >= plen:
            d = dict([[fieldname, value]])
        else:
            p = probabilities[ppos]
            ppos += 1
            d = dict([[fieldname, value],['P', p]])
        result.append((str(value), d))
    return result

class wtscenario:
    """
    A set of generators for different test scenarios
    """

    @staticmethod
    def session_create_scenario():
        """
        Return a set of scenarios with the name of this method
        'session_create_scenario' as the name of instance
        variable containing a wtscenario object.  The wtscenario
        object can be queried to get a config string.
        Each scenario is named according to the shortName() method.
        """
        s = [
            ('default', dict(session_create_scenario=wtscenario())) ]
        for imin in powerrange(512, 512*megabyte, 1024):
            for imax in powerrange(imin, 512*megabyte, 1024):
                for lmin in powerrange(512, 512*megabyte, 1024):
                    for lmax in powerrange(lmin, 512*megabyte, 1024):
                        for cache in [megabyte, 32*megabyte, 1000*megabyte]:
                            scen = wtscenario()
                            scen.ioverflow = max(imin / 40, 40)
                            scen.imax = imax
                            scen.loverflow = max(lmin / 40, 40)
                            scen.lmax = lmax
                            scen.cache_size = cache
                            s.append((scen.shortName(), dict(session_create_scenario=scen)))
        return s

    def shortName(self):
        """
        Return a name of a scenario, based on the 'log2chr-ed numerals'
        representing the four values for {internal,leaf} {minimum, maximum}
        page size.
        """
        return 'scen_' + log2chr(self.ioverflow) + log2chr(self.imax) + log2chr(self.loverflow) + log2chr(self.lmax) + log2chr(self.cache_size)

    def configString(self):
        """
        Return the associated configuration string
        """
        res = ''
        if hasattr(self, 'ioverflow'):
            res += ',internal_item_max=' + str(self.ioverflow)
        if hasattr(self, 'imax'):
            res += ',internal_page_max=' + str(self.imax)
        if hasattr(self, 'loverflow'):
            res += ',leaf_item_max=' + str(self.loverflow)
        if hasattr(self, 'lmax'):
            res += ',leaf_page_max=' + str(self.lmax)
        return res
