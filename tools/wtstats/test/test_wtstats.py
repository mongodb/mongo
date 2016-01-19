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

""" 
To run this test suite, install nose with `pip install nose` and then
run `nosetests -v` from the `./tools` directory.
"""

import os, sys, glob
import re
import json
import types
from nose import with_setup

# adding wtstats tool path to path for import
test_dir = os.path.realpath(os.path.dirname(__file__))
tool_dir = os.path.join(os.path.realpath(os.path.dirname(__file__)), '..')
sys.path.append(tool_dir)

from wtstats import main

def helper_delete_file(filename):
    """ silently delete file without throwing errors """
    try:
        os.remove(filename)
    except OSError:
        pass

def helper_cleanup():
    """ delete all html files in test directory """
    patterns = ('*.json', '*.html', '*.pyc')
    matches = []
    for p in patterns:
        matches.extend(glob.glob(os.path.join(test_dir, p)))
    for f in matches: 
        helper_delete_file(f)


def helper_run_with_fixture(kwargs=None):
    """ run tool with `output` as value for --output """
    
    if kwargs == None:
        kwargs = {}

    # set output default value
    if (not '--output' in kwargs) or (kwargs['--output'] == None):
        kwargs['--output'] = '_test_wtstats.html'

    # path replacement
    kwargs['--output'] = os.path.join(test_dir, kwargs['--output'])

    statsfile = os.path.join(test_dir, kwargs['files'] if 'files' in kwargs else 'WiredTigerStat.fixture')
    print "ST", statsfile

    arglist = ['./wtstats', statsfile]
    for item in kwargs.items():
        arglist.append(item[0]) 
        if item[1]: 
            arglist.append(item[1])

    sys.argv = arglist
    try:
        main()
    except SystemExit:
        pass


def helper_get_json_from_file(outfile):
    """ extracts the chart definition from the html file and returns the class initialization """

    with open(os.path.join(test_dir, outfile), 'r') as htmlfile:
        html = htmlfile.read()
    
    json_str = re.search(r'var data=(.*}),\w=window\.app', html).group(1)
    data = json.loads(json_str)

    return data


def tearDown():
    """ tear down fixture, removing all html files """
    helper_cleanup()


def setUp():
    """ set up fixture, removing all html files """
    helper_cleanup()


@with_setup(setUp, tearDown)
def test_helper_runner():
    """ helper runner should work as expected """
    helper_run_with_fixture()
    assert os.path.exists(os.path.join(tool_dir, 'test', '_test_wtstats.html'))


def test_help_working():
    """ wtstast should output the help screen when using --help argument """
    sys.argv = ['./wtstats.py', '--help']
    try:
        main()
    except SystemExit:
        pass

    # capture stdout
    output = sys.stdout.getvalue()

    assert output.startswith('usage:')
    assert 'positional arguments' in output
    assert 'optional arguments' in output
    assert 'show this help message and exit' in output


@with_setup(setUp, tearDown)
def test_create_html_file_basic():
    """ wtstats should create an html file from the fixture stats """

    outfile = os.path.join(test_dir, 'wtstats_test.html')
    statsfile = os.path.join(test_dir, 'WiredTigerStat.fixture')
    
    sys.argv = ['./wtstats', statsfile, '--output', outfile]
    try:
        main()
    except SystemExit:
        pass

    assert os.path.exists(outfile)


@with_setup(setUp, tearDown)
def test_output_default():
    """ wtstats should choose default output if not specified """

    statsfile = os.path.join(test_dir, 'WiredTigerStat.fixture')
    
    sys.argv = ['./wtstats', statsfile]
    try:
        main()
    except SystemExit:
        pass

    assert os.path.exists(os.path.join('./wtstats.html'))
    helper_delete_file('./wtstats.html')


@with_setup(setUp, tearDown)
def test_output_option():
    """ wtstats should use the specified filename with --output """

    outfile = '_foo_bar_baz.html'
    helper_run_with_fixture({'--output': outfile})
    assert os.path.exists(os.path.join(test_dir, outfile))

@with_setup(setUp, tearDown)
def test_monitor_stats_start_with_wtperf():
    """ wtstats should be able to parse wtperf monitor files """

    outfile = '_foo_bar_baz.html'
    helper_run_with_fixture({'files': 'monitor.fixture', '--output': outfile})
    data = helper_get_json_from_file(outfile)

    series_keys = map(lambda x: x['key'], data['series'])
    for key in series_keys:
        assert key.startswith('wtperf:')

    assert os.path.exists(os.path.join(test_dir, outfile))


@with_setup(setUp, tearDown)
def test_monitor_stats_convert_us_to_ms():
    """ wtstats should convert monitor stats us to ms """

    outfile = '_foo_bar_baz.html'
    helper_run_with_fixture({'files': 'monitor.fixture', '--output': outfile})
    data = helper_get_json_from_file(outfile)

    series_keys = map(lambda x: x['key'], data['series'])
    for key in series_keys:
        assert '(uS)' not in key

    values = (item['values'] for item in data['series'] if item['key'] == 'wtperf: insert maximum latency (ms)').next().values()
    assert max(values) == 103687 / 1000.



@with_setup(setUp, tearDown)
def test_directory_with_wtstats_and_wtperf():
    """ wtstats should be able to parse directories containing both types """

    outfile = '_test_output_file.html'
    helper_run_with_fixture({'files': '.', '--output': outfile})
    data = helper_get_json_from_file(outfile)

    series_keys = map(lambda x: x['key'], data['series'])
    assert any(map(lambda title: 'block-manager' in title, series_keys))
    assert any(map(lambda title: 'wtperf' in title, series_keys))


@with_setup(setUp, tearDown)
def test_add_ext_if_missing():
    """ wtstats should only add ".html" extension if it's missing in the --output value """

    helper_run_with_fixture({'--output': '_test_output_file.html'})
    assert os.path.exists(os.path.join(test_dir, '_test_output_file.html'))

    helper_run_with_fixture({'--output': '_test_output_file'})
    assert os.path.exists(os.path.join(test_dir, '_test_output_file.html'))

    helper_delete_file(os.path.join(test_dir, '_test_output_file.html'))


@with_setup(setUp, tearDown)
def test_replace_data_in_template():
    """ wtstats should replace the placeholder with real data """

    outfile = '_test_output_file.html'
    helper_run_with_fixture({'--output': outfile})

    templfile = open(os.path.join(tool_dir, 'wtstats.html.template'), 'r')
    htmlfile = open(os.path.join(test_dir, outfile), 'r')
    
    assert "### INSERT DATA HERE ###" in templfile.read()
    assert "### INSERT DATA HERE ###" not in htmlfile.read()

    templfile.close()
    htmlfile.close()


@with_setup(setUp, tearDown)
def test_data_with_options():
    """ wtstats should output the data as expected to the html file """

    outfile = '_test_output_file.html'
    helper_run_with_fixture({'--output': outfile})

    data = helper_get_json_from_file(outfile)
    assert 'series' in data

    serie = data['series'][0]
    assert 'values' in serie
    assert 'key' in serie


@with_setup(setUp, tearDown)
def test_include_option():
    """ wtstats should only parse the matched stats with --include """

    outfile = '_test_output_file.html'
    helper_run_with_fixture({'--output': outfile, '--include': 'bytes'})
    data = helper_get_json_from_file(outfile)

    series_keys = map(lambda x: x['key'], data['series'])
    for key in series_keys:
        print key
        assert 'bytes' in key


@with_setup(setUp, tearDown)
def test_include_skip_prefix():
    """ wtstats should remove the common prefix from titles """ 

    outfile = '_test_output_file.html'
    helper_run_with_fixture({'--output': outfile, '--include': 'cache'})
    data = helper_get_json_from_file(outfile)

    series_keys = map(lambda x: x['key'], data['series'])
    for key in series_keys:
        assert not key.startswith('cache:')


@with_setup(setUp, tearDown)
def test_list_option():
    """ wtstats should only output list of series titles with --list """

    outfile = '_test_output_file.html'
    helper_run_with_fixture({'--output': outfile, '--list': None})

    # no html file created
    assert not os.path.exists(os.path.join(test_dir, outfile))

    # find one expected output line
    output = sys.stdout.getvalue().splitlines()
    assert next((l for l in output if 'log: total log buffer size' in l), None) != None


@with_setup(setUp, tearDown)
def test_json_option():
    """ wtstats should additionally output json file with --json """

    outfile = '_test_output_file.html'
    helper_run_with_fixture({'--output': outfile, '--json': None})
    data_html = helper_get_json_from_file(outfile)
    with open(os.path.join(test_dir, '_test_output_file.json'), 'r') as jsonfile:
        data_json = json.load(jsonfile)
    
    assert data_html == data_json


@with_setup(setUp, tearDown)
def test_all_option():
    """ wtstats should create grouped html files with --all """

    outfile = 'mystats.html'
    helper_run_with_fixture({'--output': outfile, '--all': None})

    files = glob.glob(os.path.join(test_dir, '*.html'))
    
    # test some expected files
    assert len(files) > 1
    assert os.path.join(test_dir, 'mystats.transaction.html') in files
    assert os.path.join(test_dir, 'mystats.group.system.html') in files
    assert os.path.join(test_dir, 'mystats.html') in files


    data = helper_get_json_from_file('mystats.transaction.html')
    series_keys = map(lambda x: x['key'], data['series'])
    for key in series_keys:
        assert key.startswith('transaction:')
