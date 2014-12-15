""" To run this test suite, install nose with `pip install nose` and then
    run `nosetests -v` from the `./tools` directory.
"""

import os, sys, glob
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
    for f in glob.glob('./test/*.html'):
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

    statsfile = os.path.join(test_dir, 'WiredTigerStat.fixture')

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


def helper_find_line(outfile, str):
    """ extracts the chart definition from the html file and returns the class initialization """

    with open(os.path.join(test_dir, outfile), 'r') as htmlfile:
        html = htmlfile.readlines()
        line = next((l for l in html if str in l), None)

    return line

def helper_parse_json_data(outfile):
    """ extracts the data definition from the html file and parses the json data as python dict """
    
    substr = 'var data = '
    data_line = helper_find_line(outfile, substr)
    data = json.loads(data_line[data_line.find(substr) + len(substr) : data_line.rfind(';')])
    
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
    helper_run_with_fixture({'--output': outfile, '--abstime': None})
    assert os.path.exists(os.path.join(test_dir, outfile))
    

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

    data = helper_parse_json_data(outfile)

    assert 'series' in data
    assert 'chart' in data
    assert 'xdata' in data

    chart = data['chart']

    assert 'extra' in chart
    assert 'x_is_date' in chart
    assert 'type' in chart
    assert 'height' in chart

    serie = data['series'][0]

    assert 'values' in serie
    assert 'key' in serie
    assert 'yAxis' in serie


@with_setup(setUp, tearDown)
def test_abstime_option():
    """ wtstats should export unix epochs when running with --abstime """

    outfile = '_test_output_file.html'
    helper_run_with_fixture({'--output': outfile, '--abstime': None})
    data = helper_parse_json_data(outfile)

    assert data['chart']['extra']['x_axis_format'] == "%H:%M:%S"
    assert data['chart']['x_is_date'] == True
    assert type(data['xdata'][0]) == types.IntType
    assert data['xdata'][0] > 1417700000000


@with_setup(setUp, tearDown)
def test_focus_option():
    """ wtstats should create a lineWithFocusChart when using --focus """

    outfile = '_test_output_file.html'
    helper_run_with_fixture({'--output': outfile, '--focus': None})
    data = helper_parse_json_data(outfile)

    assert data['chart']['type'] == 'lineWithFocusChart'


@with_setup(setUp, tearDown)
def test_include_option():
    """ wtstats should only parse the matched stats with --include """

    outfile = '_test_output_file.html'
    helper_run_with_fixture({'--output': outfile, '--include': 'bytes'})
    data = helper_parse_json_data(outfile)

    series_keys = map(lambda x: x['key'], data['series'])
    for key in series_keys:
        print key
        assert 'bytes' in key


@with_setup(setUp, tearDown)
def test_include_skip_prefix():
    """ wtstats should remove the common prefix from titles """ 

    outfile = '_test_output_file.html'
    helper_run_with_fixture({'--output': outfile, '--include': 'cache'})
    data = helper_parse_json_data(outfile)

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
def test_right_option():
    """ wtstats should create a multiChart with --right """

    outfile = '_test_output_file.html'
    helper_run_with_fixture({'--output': outfile, '--include': 'cache', '--right': 'conn'})

    data = helper_parse_json_data(outfile)
    assert data['chart']['type'] == 'multiChart'


@with_setup(setUp, tearDown)
def test_all_option():
    """ wtstats should create grouped html files with --all """

    outfile = 'mystats.html'
    helper_run_with_fixture({'--output': outfile, '--all': None})

    files = glob.glob('./test/*.html')
    
    # test some expected files
    assert len(files) > 1
    assert './test/mystats.transaction.html' in files
    assert './test/mystats.group.system.html' in files
    assert './test/mystats.html' in files


    data = helper_parse_json_data('mystats.transaction.html')
    series_keys = map(lambda x: x['key'], data['series'])
    for key in series_keys:
        assert key.startswith('transaction:')

