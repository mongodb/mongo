import sys
import os

# adding wtstats tool path to path for import
tool_dir = os.path.split(sys.argv[0])[0]
sys.path.append(tool_dir)

from wtstats import main

def silent_delete_file(filename):
    try:
        os.remove(filename)
    except OSError:
        pass

def test_help_working():
    """ should output the help screen when using --help argument """
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

def test_create_html_file():
    """ should create an html file from the fixture stats """

    outputfile = os.path.join(tool_dir, 'test/', './wtstats_test.html')
    
    # delete existing html file if exists
    silent_delete_file(outputfile)

    sys.argv = ['./wtstats', './WiredTigerStat.fixture', '--output', outputfile]
    try:
        main()
    except SystemExit:
        pass

