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
#
# test_base02.py
#    Configuration
#

import json
import wiredtiger, wttest
from wtscenario import check_scenarios

# Test configuration strings.
class test_base02(wttest.WiredTigerTestCase):
    name = 'test_base02a'
    extra_config = ''

    scenarios = check_scenarios([
        ('file', dict(uri='file:')),
        ('table', dict(uri='table:')),
        ('lsm', dict(uri='lsm:')),
        ('table-lsm', dict(uri='table:', extra_config=',type=lsm')),
    ])

    def create_and_drop(self, confstr):
        name = self.uri + self.name
        confstr += self.extra_config
        self.pr('create_and_drop: ' + name + ": " + confstr)
        self.session.create(name, confstr)
        self.session.drop(name, None)

    def test_config_combinations(self):
        """
        Spot check various combinations of configuration options.
        """
        conf_confsize = [
            None,
            'allocation_size=1024',
            'internal_page_max=64k,internal_item_max=1k',
            'leaf_page_max=128k,leaf_item_max=512',
            'leaf_page_max=256k,leaf_item_max=256,internal_page_max=8k,internal_item_max=128',
            ]
        conf_col = [
            'columns=(first,second)',
            'columns=(first,   second,,,)',
            'key_format="5S", value_format="Su", columns=(first,second, third)',
            ',,columns=(first=S,second="4u"),,',
            'columns=(/path/key,   /other/path/value,,,)',
            ]
        conf_encoding = [
            None,
            'huffman_key=,huffman_value=english',
            ]
        for size in conf_confsize:
            for col in conf_col:
                for enc in conf_encoding:
                    conflist = [size, col, enc]
                    confstr = ",".join([c for c in conflist if c != None])
                    self.create_and_drop(confstr)

    def test_config_json(self):
        """
        Spot check various combinations of configuration options, using JSON format.
        """
        conf_jsonstr = [
            json.dumps({'columns' : ('key', 'value')}),
            json.dumps({
                "key_format" : "S",
                "value_format" : "5sHQ",
                "columns" : ("id", "country", "year", "population"),
                "colgroups" : ("cyear", "population"),
                    })]
        for confstr in conf_jsonstr:
            self.create_and_drop(confstr)

if __name__ == '__main__':
    wttest.run()
