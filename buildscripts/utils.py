
import codecs
import re
import socket
import time
import os
import os.path
import itertools
import subprocess
import sys
import hashlib

# various utilities that are handy

def getAllSourceFiles( arr=None , prefix="." ):
    if arr is None:
        arr = []
    
    if not os.path.isdir( prefix ):
        # assume a file
        arr.append( prefix )
        return arr
        
    for x in os.listdir( prefix ):
        if ( x.startswith( "." )
          or x.startswith( "pcre-" )
          or x.startswith( "32bit" )
          or x.startswith( "mongodb-" )
          or x.startswith( "debian" )
          or x.startswith( "mongo-cxx-driver" )
          or 'gotools' in x ):
            continue

        # XXX: Avoid conflict between v8, v8-3.25 and mozjs source files in
        #      src/mongo/scripting
        #      Remove after v8-3.25 migration.

        if x.find("v8-3.25") != -1 or x.find("mozjs") != -1:
            continue

        full = prefix + "/" + x
        if os.path.isdir( full ) and not os.path.islink( full ):
            getAllSourceFiles( arr , full )
        else:
            if full.endswith( ".cpp" ) or full.endswith( ".h" ) or full.endswith( ".c" ):
                full = full.replace( "//" , "/" )
                arr.append( full )

    return arr


def getGitBranch():
    if not os.path.exists( ".git" ) or not os.path.isdir(".git"):
        return None

    version = open( ".git/HEAD" ,'r' ).read().strip()
    if not version.startswith( "ref: " ):
        return version
    version = version.split( "/" )
    version = version[len(version)-1]
    return version

def getGitBranchString( prefix="" , postfix="" ):
    t = re.compile( '[/\\\]' ).split( os.getcwd() )
    if len(t) > 2 and t[len(t)-1] == "mongo":
        par = t[len(t)-2]
        m = re.compile( ".*_([vV]\d+\.\d+)$" ).match( par )
        if m is not None:
            return prefix + m.group(1).lower() + postfix
        if par.find("Nightly") > 0:
            return ""


    b = getGitBranch()
    if b == None or b == "master":
        return ""
    return prefix + b + postfix

def getGitVersion():
    if not os.path.exists( ".git" ) or not os.path.isdir(".git"):
        return "nogitversion"

    version = open( ".git/HEAD" ,'r' ).read().strip()
    if not version.startswith( "ref: " ):
        return version
    version = version[5:]
    f = ".git/" + version
    if not os.path.exists( f ):
        return version
    return open( f , 'r' ).read().strip()

def getGitDescribe():
    with open(os.devnull, "r+") as devnull:
        proc = subprocess.Popen("git describe",
            stdout=subprocess.PIPE,
            stderr=devnull,
            stdin=devnull,
            shell=True)
        return proc.communicate()[0].strip()

def execsys( args ):
    import subprocess
    if isinstance( args , str ):
        r = re.compile( "\s+" )
        args = r.split( args )
    p = subprocess.Popen( args , stdout=subprocess.PIPE , stderr=subprocess.PIPE )
    r = p.communicate()
    return r;

def getprocesslist():
    raw = ""
    try:
        raw = execsys( "/bin/ps axww" )[0]
    except Exception,e:
        print( "can't get processlist: " + str( e ) )

    r = re.compile( "[\r\n]+" )
    return r.split( raw )

def removeIfInList( lst , thing ):
    if thing in lst:
        lst.remove( thing )

def findVersion( root , choices ):
    for c in choices:
        if ( os.path.exists( root + c ) ):
            return root + c
    raise "can't find a version of [" + root + "] choices: " + choices

def choosePathExist( choices , default=None):
    for c in choices:
        if c != None and os.path.exists( c ):
            return c
    return default

def filterExists(paths):
    return filter(os.path.exists, paths)

def ensureDir( name ):
    d = os.path.dirname( name )
    if not os.path.exists( d ):
        print( "Creating dir: " + name );
        os.makedirs( d )
        if not os.path.exists( d ):
            raise "Failed to create dir: " + name


def distinctAsString( arr ):
    s = set()
    for x in arr:
        s.add( str(x) )
    return list(s)

def checkMongoPort( port=27017 ):
    sock = socket.socket()
    sock.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
    sock.settimeout(1)
    sock.connect(("localhost", port))
    sock.close()


def didMongodStart( port=27017 , timeout=20 ):
    while timeout > 0:
        time.sleep( 1 )
        try:
            checkMongoPort( port )
            return True
        except Exception,e:
            print( e )
            timeout = timeout - 1
    return False

def which(executable):
    if sys.platform == 'win32':
        paths = os.environ.get('Path', '').split(';')
    else:
        paths = os.environ.get('PATH', '').split(':')

    for path in paths:
        path = os.path.expandvars(path)
        path = os.path.expanduser(path)
        path = os.path.abspath(path)
        executable_path = os.path.join(path, executable)
        if os.path.exists(executable_path):
            return executable_path

    return executable

def md5sum( file ):
    #TODO error handling, etc..
    return execsys( "md5sum " + file )[0].partition(" ")[0]

def md5string( a_string ):
    return hashlib.md5(a_string).hexdigest()

def find_python(min_version=(2, 5)):
    try:
        if sys.version_info >= min_version:
            return sys.executable
    except AttributeError:
        # In case the version of Python is somehow missing sys.version_info or sys.executable.
        pass

    version = re.compile(r'[Pp]ython ([\d\.]+)', re.MULTILINE)
    binaries = ('python27', 'python2.7', 'python26', 'python2.6', 'python25', 'python2.5', 'python')
    for binary in binaries:
        try:
            out, err = subprocess.Popen([binary, '-V'], stdout=subprocess.PIPE, stderr=subprocess.PIPE).communicate()
            for stream in (out, err):
                match = version.search(stream)
                if match:
                    versiontuple = tuple(map(int, match.group(1).split('.')))
                    if versiontuple >= min_version:
                        return which(binary)
        except:
            pass

    raise Exception('could not find suitable Python (version >= %s)' % '.'.join(str(v) for v in min_version))

def smoke_command(*args):
    # return a list of arguments that comprises a complete
    # invocation of smoke.py
    here = os.path.dirname(__file__)
    smoke_py = os.path.abspath(os.path.join(here, 'smoke.py'))
    # the --with-cleanbb argument causes smoke.py to run
    # buildscripts/cleanbb.py before each test phase; this
    # prevents us from running out of disk space on slaves
    return [find_python(), smoke_py, '--with-cleanbb'] + list(args)

def run_smoke_command(*args):
    # to run a command line script from a scons Alias (or any
    # Action), the command sequence must be enclosed in a list,
    # otherwise SCons treats it as a list of dependencies.
    return [smoke_command(*args)]

# unicode is a pain. some strings cannot be unicode()'d
# but we want to just preserve the bytes in a human-readable
# fashion. this codec error handler will substitute the
# repr() of the offending bytes into the decoded string
# at the position they occurred
def replace_with_repr(unicode_error):
    offender = unicode_error.object[unicode_error.start:unicode_error.end]
    return (unicode(repr(offender).strip("'").strip('"')), unicode_error.end)

codecs.register_error('repr', replace_with_repr)

def unicode_dammit(string, encoding='utf8'):
    # convert a string to a unicode, using the Python
    # representation of non-ascii bytes when necessary
    #
    # name inpsired by BeautifulSoup's "UnicodeDammit"
    return string.decode(encoding, 'repr')

