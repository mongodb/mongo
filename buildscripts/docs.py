"""Build the C++ client docs and the MongoDB server docs.
"""

from __future__ import with_statement
import os
import shutil
import socket
import subprocess
import time
import urllib2

import markdown


def clean_dir(dir):
    try:
        shutil.rmtree(dir)
    except:
        pass
    os.makedirs(dir)


def convert_dir(source, dest):
    clean_dir(dest)

    for x in os.listdir(source + "/"):
        if not x.endswith(".md"):
            continue

        with open("%s/%s" % (source, x)) as f:
            raw = f.read()

        html = markdown.markdown(raw)
        print(x)

        with open("%s/%s" % (dest, x.replace(".md", ".html")), 'w') as o:
            o.write(html)


def check_mongo():
    sock = socket.socket()
    sock.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
    sock.settimeout(1)
    sock.connect(("localhost", 31999))
    sock.close()

def did_mongod_start(timeout=20):
    while timeout > 0:
        time.sleep(1)
        try:
            check_mongo()
            return True
        except Exception,e:
            print e
            timeout = timeout - 1
    return False

def stop(proc):
    try:
        proc.terminate()
    except AttributeError:
        os.kill(proc.pid, 15)

def commands_list(out):
    clean_dir("dummy_data_dir")
    with open("/dev/null") as null:
        try:
            p = subprocess.Popen(["./mongod", "--dbpath", "dummy_data_dir",
                                  "--port", "31999", "--rest"], stdout=null, stderr=null)
        except:
            print "No mongod? Skipping..."
            return
        if not did_mongod_start():
            print "Slow mongod? Skipping..."
            stop(p)
            return
        print "Started mongod"

        with open(out, "w") as f:
            f.write("<base href='http://localhost:28017'/>")
            f.write(urllib2.urlopen("http://localhost:32999/_commands").read())

        print "Stopping mongod"
        stop(p)

def gen_cplusplus(dir):
    clean_dir(dir)
    clean_dir("docs/doxygen")

    # Too noisy...
    with open("/dev/null") as null:
        subprocess.call(["doxygen", "doxygenConfig"], stdout=null, stderr=null)

    os.rename("docs/doxygen/html", dir)


def version():
    """Get the server version from doxygenConfig.
    """
    with open("doxygenConfig") as f:
        for line in f.readlines():
            if line.startswith("PROJECT_NUMBER"):
                return line.split("=")[1].strip()


def main():
    v = version()
    print("Generating server docs in docs/html/internal/%s" % v)
    convert_dir("docs", "docs/html/internal/%s" % v)
    print("Generating commands list")
    commands_list("docs/html/internal/%s/commands.html" % v)
    shutil.rmtree("dummy_data_dir")
    print("Generating C++ docs in docs/html/cplusplus/%s" % v)
    gen_cplusplus("docs/html/cplusplus/%s" % v)


if __name__ == "__main__":
    main()


