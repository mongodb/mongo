
import os
import sys
import shutil
import datetime
import time
import subprocess
import urllib
import urllib2
import json
import pprint

import boto
import simples3

import pymongo

def findSettingsSetup():
    sys.path.append( "./" )
    sys.path.append( "../" )
    sys.path.append( "../../" )
    sys.path.append( "../../../" )

findSettingsSetup()
import settings
import buildscripts.utils as utils
import buildscripts.smoke as smoke

bucket = simples3.S3Bucket( settings.emr_bucket , settings.emr_id , settings.emr_key )

def _get_status():
    
    def gh( cmds ):
        txt = ""
        for cmd in cmds:
            res = utils.execsys( "git " + cmd )
            txt = txt + res[0] + res[1]
        return utils.md5string( txt )

    return "%s-%s" % ( utils.execsys( "git describe" )[0].strip(),  gh( [ "diff" , "status" ] ) )

def _get_most_recent_tgz( prefix ):
    # this is icky, but works for now
    all = []
    for x in os.listdir( "." ):
        if not x.startswith( prefix ) or not x.endswith( ".tgz" ):
            continue
        all.append( ( x , os.stat(x).st_mtime ) )

    if len(all) == 0:
        raise Exception( "can't find file with prefix: " + prefix )

    all.sort( lambda x,y: int(y[1] - x[1]) )

    return all[0][0]

def get_build_info():
    return ( os.environ.get('MONGO_BUILDER_NAME') , os.environ.get('MONGO_BUILD_NUMBER') )

def make_tarball():

    m = _get_most_recent_tgz( "mongodb-" )
    
    c = "test-code-emr.tgz"    
    tar = "tar zcf %s src jstests buildscripts" % c

    log_config = "log_config.py"
    if os.path.exists( log_config ):
        os.unlink( log_config )

    credentials = do_credentials()
    if credentials:

        builder , buildnum = get_build_info()
        
        if builder and buildnum:

            file = open( log_config , "wb" )
            file.write( 'username="%s"\npassword="%s"\n' % credentials )
            file.write( 'name="%s"\nnumber=%s\n'% ( builder , buildnum ) )

            file.close()
            
            tar = tar + " " + log_config

    utils.execsys( tar )
    return ( m , c )

def _put_ine( bucket , local , remote ):
    print( "going to put\n\t%s\n\thttp://%s.s3.amazonaws.com/%s" % ( local , settings.emr_bucket , remote ) )

    for x in bucket.listdir( prefix=remote ):
        print( "\talready existed" )
        return remote

    bucket.put( remote , open( local , "rb" ).read() , acl="public-read" )
    return remote

def build_jar():
    root = "build/emrjar"
    src = "buildscripts/emr"

    if os.path.exists( root ):
        shutil.rmtree( root )
    os.makedirs( root )

    for x in os.listdir( src ):
        if not x.endswith( ".java" ):
            continue
        shutil.copyfile( src + "/" + x , root + "/" + x )
    shutil.copyfile( src + "/MANIFEST.MF" , root + "/MANIFEST.FM" )
    
    classpath = os.listdir( src + "/lib" )
    for x in classpath:
        shutil.copyfile( src + "/lib/" + x , root + "/" + x )
    classpath.append( "." )
    classpath = ":".join(classpath)

    for x in os.listdir( root ):
        if x.endswith( ".java" ):
            if subprocess.call( [ "javac" , "-cp" , classpath , x ] , cwd=root) != 0:
                raise Exception( "compiled failed" )

    args = [ "jar" , "-cfm" , "emr.jar" , "MANIFEST.FM" ]
    for x in os.listdir( root ):
        if x.endswith( ".class" ):
            args.append( x )
    subprocess.call( args , cwd=root )
    
    shutil.copyfile( root + "/emr.jar" , "emr.jar" )

    return "emr.jar"

def push():
    mongo , test_code = make_tarball()
    print( mongo )
    print( test_code )

    root = "emr/%s/%s" % ( datetime.date.today().strftime("%Y-%m-%d") , os.uname()[0].lower() )
    
    def make_long_name(local,hash):
        pcs = local.rpartition( "." )
        h = _get_status()
        if hash:
            h = utils.md5sum( local )
        return "%s/%s-%s.%s" % ( root , pcs[0] , h , pcs[2] )

    mongo = _put_ine( bucket , mongo , make_long_name( mongo , False ) )
    test_code = _put_ine( bucket , test_code , make_long_name( test_code , True ) )
    
    jar = build_jar()
    jar = _put_ine( bucket , jar , make_long_name( jar , False ) )

    setup = "buildscripts/emr/emrnodesetup.sh"
    setup = _put_ine( bucket , setup , make_long_name( setup , True ) )

    return mongo , test_code , jar , setup

def run_tests( things , tests ):
    if len(tests) == 0:
        raise Exception( "no tests" )
    oldNum = len(tests)
    tests = fix_suites( tests )
    print( "tests expanded from %d to %d" % ( oldNum , len(tests) ) )
    
    print( "things:%s\ntests:%s\n" % ( things , tests ) )

    emr = boto.connect_emr( settings.emr_id , settings.emr_key )

    def http(path):
        return "http://%s.s3.amazonaws.com/%s" % ( settings.emr_bucket , path )
    
    run_s3_path = "emr/%s/%s/%s/" % ( os.getenv( "USER" ) , 
                                      os.getenv( "HOST" ) , 
                                      datetime.datetime.today().strftime( "%Y%m%d-%H%M" ) )

    run_s3_root = "s3n://%s/%s/" % ( settings.emr_bucket , run_s3_path )

    out = run_s3_root + "out"
    logs = run_s3_root + "logs"

    jar="s3n://%s/%s" % ( settings.emr_bucket , things[2] )
    step_args=[ http(things[0]) , http(things[1]) , out , ",".join(tests) ]
    
    step = boto.emr.step.JarStep( "emr main" , jar=jar,step_args=step_args )
    print( "jar:%s\nargs:%s" % ( jar , step_args ) )

    setup = boto.emr.BootstrapAction( "setup" , "s3n://%s/%s" % ( settings.emr_bucket , things[3] ) , []  )

    jobid = emr.run_jobflow( name = "Mongo EMR for %s from %s" % ( os.getenv( "USER" ) , os.getenv( "HOST" ) ) ,
                             ec2_keyname = "emr1" , 
                             slave_instance_type = "m1.large" ,
                             ami_version = "latest" ,
                             num_instances=5 ,
                             log_uri = logs ,
                             bootstrap_actions = [ setup ] , 
                             steps = [ step ] )

    
    print( "%s jobid: %s" % ( datetime.datetime.today() , jobid ) )

    while ( True ):
        flow = emr.describe_jobflow( jobid )
        print( "%s status: %s" % ( datetime.datetime.today() , flow.state ) )
        if flow.state == "COMPLETED" or flow.state == "FAILED":
            break
        time.sleep(30)

    syncdir = "build/emrout/" + jobid + "/"
    sync_s3( run_s3_path , syncdir )
    
    final_out = "build/emrout/" + jobid + "/" 
    
    print("output in: " + final_out )
    do_output( final_out )

def sync_s3( remote_dir , local_dir ):
    for x in bucket.listdir( remote_dir ):
        out = local_dir + "/" + x[0]
        
        if os.path.exists( out ) and x[2].find( utils.md5sum( out ) ) >= 0:
            continue

        dir = out.rpartition( "/" )[0]
        if not os.path.exists( dir ):
            os.makedirs( dir )
            
        thing = bucket.get( x[0] )
        open( out , "wb" ).write( thing.read() )

def fix_suites( suites ):
    fixed = []
    for name,x in smoke.expand_suites( suites , False ):
        idx = name.find( "/jstests" )
        if idx >= 0:
            name = name[idx+1:]
        fixed.append( name )
    return fixed

def do_credentials():
    root = "buildbot.tac"
    
    while len(root) < 40 :
        if os.path.exists( root ):
            break
        root = "../" + root
        
    if not os.path.exists( root ):
        return None
    
    credentials = {}
    execfile(root, credentials, credentials)
    
    if "slavename" not in credentials:
        return None

    if "passwd" not in credentials:
        return None
    
    return ( credentials["slavename"] , credentials["passwd"] )


def do_output( dir ):

    def go_down( start ):
        lst = os.listdir(dir)
        if len(lst) != 1:
            raise Exception( "sad: " + start )
        return start + "/" + lst[0]

    while "out" not in os.listdir( dir ):
        dir = go_down( dir )

    dir = dir + "/out"
    
    pieces = os.listdir(dir)
    pieces.sort()

    passed = []
    failed = []
    times = {}

    for x in pieces:
        if not x.startswith( "part" ):
            continue
        full = dir + "/" + x
        
        for line in open( full , "rb" ):
            if line.find( "-passed" ) >= 0:
                passed.append( line.partition( "-passed" )[0] )
                continue
            
            if line.find( "-failed" ) >= 0:
                failed.append( line.partition( "-failed" )[0] )
                continue
                
            if line.find( "-time-seconds" ) >= 0:
                p = line.partition( "-time-seconds" )
                times[p[0]] = p[2].strip()
                continue
            
            print( "\t" + line.strip() )
            
    def print_list(name,lst):
        print( name )
        for x in lst:
            print( "\t%s\t%s" % ( x , times[x] ) )

    print_list( "passed" , passed )
    print_list( "failed" , failed )

    if do_credentials():
        builder , buildnum = get_build_info()
        if builder and buildnum:
            conn = pymongo.Connection( "bbout1.10gen.cc" )
            db = conn.buildlogs
            q = { "builder" : builder , "buildnum" : int(buildnum) } 
            doc = db.builds.find_one( q )
            
            if doc:
                print( "\nhttp://buildlogs.mongodb.org/build/%s" % doc["_id"] )
            

if __name__ == "__main__":
    if len(sys.argv) == 1:
        print( "need an arg" )
        
    elif sys.argv[1] == "tarball":
        make_tarball()
    elif sys.argv[1] == "jar":
        build_jar()
    elif sys.argv[1] == "push":
        print( push() )

    elif sys.argv[1] == "sync":
        sync_s3( sys.argv[2] , sys.argv[3] )

    elif sys.argv[1] == "fix_suites":
        for x in fix_suites( sys.argv[2:] ):
            print(x)

    elif sys.argv[1] == "credentials":
        print( do_credentials() )

    elif sys.argv[1] == "test":
        m , c = make_tarball()
        build_jar()
        cmd = [ "java" , "-cp" , os.environ.get( "CLASSPATH" , "." ) + ":emr.jar" , "emr" ]

        workingDir = "/data/emr/test"
        cmd.append( "--workingDir" )
        cmd.append( workingDir )
        if os.path.exists( workingDir ):
            shutil.rmtree( workingDir )
        
        cmd.append( "file://" + os.getcwd() + "/" + m )
        cmd.append( "file://" + os.getcwd() + "/" + c )
        
        out = "/tmp/emrresults"
        cmd.append( out )
        if os.path.exists( out ):
            shutil.rmtree( out )

        cmd.append( "jstests/basic1.js" )

        subprocess.call( cmd )
        
        for x in os.listdir( out ):
            if x.startswith( "." ):
                continue
            print( x )
            for z in open( out + "/" + x ):
                print( "\t" + z.strip() )

    elif sys.argv[1] == "output":
        do_output( sys.argv[2] )

    elif sys.argv[1] == "full":
        things = push()
        run_tests( things , sys.argv[2:] )

    else:
        things = push()
        run_tests( things , sys.argv[1:] )

