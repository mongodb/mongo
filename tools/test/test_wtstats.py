import os, sys, glob
import json
import types

# adding wtstats tool path to path for import
test_dir = os.path.realpath(os.path.dirname(__file__))
tool_dir = os.path.join(os.path.realpath(os.path.dirname(__file__)), '..')
sys.path.append(tool_dir)

from wtstats import main

def helper_delete_file(filename):
    try:
        os.remove(filename)
    except OSError:
        pass

def helper_cleanup():
    # delete all html files in test directory
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

    # delete existing html files if exists
    helper_delete_file(kwargs['--output'])
    helper_delete_file(kwargs['--output'] + '.html')

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

def helper_parse_json_data(html):
    substr = 'var data = '
    lines = html.splitlines()

    # find data line and parse json string
    data_line = next(l for l in lines if substr in l)
    data = json.loads(data_line[data_line.find(substr) + len(substr):data_line.rfind(';')])
    
    return data


def tearDown():
    helper_cleanup()

def setUp():
    helper_cleanup()


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


def test_helper_runner():
    """ helper runner should work as expected """
    helper_run_with_fixture()
    assert os.path.exists(os.path.join(tool_dir, 'test', '_test_wtstats.html'))


def test_create_html_file_basic():
    """ wtstats should create an html file from the fixture stats """

    outputfile = os.path.join(test_dir, 'wtstats_test.html')
    statsfile = os.path.join(test_dir, 'WiredTigerStat.fixture')
    
    # delete existing html file if exists
    helper_delete_file(outputfile)

    sys.argv = ['./wtstats', statsfile, '--output', outputfile]
    try:
        main()
    except SystemExit:
        pass

    assert os.path.exists(outputfile)


def test_add_ext_if_missing():
    """ wtstats should only add ".html" extension if it's missing in the --output value """

    helper_run_with_fixture({'--output': '_test_output_file.html'})
    assert os.path.exists(os.path.join(test_dir, '_test_output_file.html'))

    helper_run_with_fixture({'--output': '_test_output_file'})
    assert os.path.exists(os.path.join(test_dir, '_test_output_file.html'))

    helper_delete_file(os.path.join(test_dir, '_test_output_file.html'))


def test_replace_data_in_template():
    """ wtstats should replace the placeholder with real data """

    output = '_test_output_file.html'
    helper_run_with_fixture({'--output': output})

    templfile = open(os.path.join(tool_dir, 'wtstats.html.template'), 'r')
    htmlfile = open(os.path.join(test_dir, output), 'r')
    
    assert "### INSERT DATA HERE ###" in templfile.read()
    assert "### INSERT DATA HERE ###" not in htmlfile.read()

    templfile.close()
    htmlfile.close()


def test_data_with_options():
    """ wtstats outputs the data as expected to the html file """

    output = '_test_output_file.html'
    helper_run_with_fixture({'--output': output})

    with open(os.path.join(test_dir, output), 'r') as htmlfile:
        data = helper_parse_json_data(htmlfile.read())

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


def test_abstime_option():
    """ wtstats exports unix epochs when running with --abstime """

    output = '_test_output_file.html'
    helper_run_with_fixture({'--output': output, '--abstime': None})

    with open(os.path.join(test_dir, output), 'r') as htmlfile:
        data = helper_parse_json_data(htmlfile.read())

    assert data['chart']['extra']['x_axis_format'] == "%H:%M:%S"
    assert data['chart']['x_is_date'] == True
    assert type(data['xdata'][0]) == types.IntType
    assert data['xdata'][0] > 1417700000000




