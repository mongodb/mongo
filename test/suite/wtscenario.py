#!/usr/bin/env python
#
# Public Domain 2014-2016 MongoDB, Inc.
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

import testscenarios
import suite_random

# wtscenarios.py
#    Support scenarios based testing
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

def check_scenarios(scenes):
    """
    Make sure all scenarios have unique names
    """
    assert len(scenes) == len(dict(scenes))
    return scenes

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
    return check_scenarios(result)

def prune_sorter_key(scene):
    """
    Used by prune_scenerios to extract key for sorting.
    The key is the saved random value multiplied by
    the probability of choosing.
    """
    p = 1.0
    if 'P' in scene[1]:
        p = scene[1]['P']
    return p * scene[1]['_rand']

def prune_resort_key(scene):
    """
    Used by prune_scenerios to extract the original ordering key for sorting.
    """
    return scene[1]['_order']

def set_long_run(islong):
    global _is_long_run
    _is_long_run = islong

def prune_scenarios(scenes, default_count = -1, long_count = -1):
    """
    Use listed probabilities for pruning the list of scenarios.
    That is, the highest probability (value of P in the scendario)
    are chosen more often.  With just one argument, only scenarios
    with P > .5 are returned half the time, etc. A second argument
    limits the number of scenarios. When a third argument is present,
    it is a separate limit for a long run.
    """
    global _is_long_run
    r = suite_random.suite_random()
    result = []
    if default_count == -1:
        # Missing second arg - return those with P == .3 at
        # 30% probability, for example.
        for scene in scenes:
            if 'P' in scene[1]:
                p = scene[1]['P']
                if p < r.rand_float():
                    continue
            result.append(scene)
        return result
    else:
        # With at least a second arg present, we'll want a specific count
        # of items returned.  So we'll sort them all and choose
        # the top number.  Not the most efficient solution,
        # but it's easy.
        if _is_long_run and long_count != -1:
            count = long_count
        else:
            count = default_count

        l = len(scenes)
        if l <= count:
            return scenes
        if count == 0:
            return []
        order = 0
        for scene in scenes:
            scene[1]['_rand'] = r.rand_float()
            scene[1]['_order'] = order
            order += 1
        scenes = sorted(scenes, key=prune_sorter_key) # random sort driven by P
        scenes = scenes[l-count:l]                    # truncate to get best
        scenes = sorted(scenes, key=prune_resort_key) # original order
        for scene in scenes:
            del scene[1]['_rand']
            del scene[1]['_order']
        return check_scenarios(scenes)

def number_scenarios(scenes):
    """
    Add a 'scenario_number' and 'scenario_name' variable to each scenario.
    The hash table for each scenario is altered!
    """
    count = 0
    for scene in scenes:
        scene[1]['scenario_name'] = scene[0]
        scene[1]['scenario_number'] = count
        count += 1
    return check_scenarios(scenes)

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
            if self.imax < 4*1024:
                res += ',allocation_size=512'
        if hasattr(self, 'loverflow'):
            res += ',leaf_item_max=' + str(self.loverflow)
        if hasattr(self, 'lmax'):
            res += ',leaf_page_max=' + str(self.lmax)
            if self.lmax < 4*1024:
                res += ',allocation_size=512'
        return res
