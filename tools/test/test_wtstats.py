import os, sys, glob

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


def helper_run_with_fixture(output=None):
    """ run tool with `output` as value for --output """
    
    # set output default value
    if output == None:
        output = '_test_wtstats.html'

    # delete existing html files if exists
    helper_delete_file(output)
    helper_delete_file(output + '.html')

    outputfile = os.path.join(test_dir, output)
    statsfile = os.path.join(test_dir, 'WiredTigerStat.fixture')
    
    # delete existing html file if exists
    helper_delete_file(outputfile)

    sys.argv = ['./wtstats', statsfile, '--output', outputfile]
    try:
        main()
    except SystemExit:
        pass


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

    helper_run_with_fixture('_test_output_file.html')
    assert os.path.exists(os.path.join(test_dir, '_test_output_file.html'))

    helper_run_with_fixture('_test_output_file')
    assert os.path.exists(os.path.join(test_dir, '_test_output_file.html'))

    helper_delete_file(os.path.join(test_dir, '_test_output_file.html'))


def test_replace_data_in_template():
    """ wtstats should replace the placeholder with real data """

    output = '_test_output_file.html'
    helper_run_with_fixture(output)

    templfile = open(os.path.join(tool_dir, 'wtstats.html.template'), 'r')
    htmlfile = open(os.path.join(test_dir, output), 'r')
    
    assert "### INSERT DATA HERE ###" in templfile.read()
    assert "### INSERT DATA HERE ###" not in htmlfile.read()

    templfile.close()
    htmlfile.close()






