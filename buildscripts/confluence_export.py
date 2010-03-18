#! /usr/bin/env python

# Export the contents on confluence
#
# Dependencies:
#   - suds
#
# User: soap, Password: soap
from __future__ import with_statement
import cookielib
import datetime
import os
import shutil
import subprocess
import sys
import urllib2

from suds.client import Client

SOAP_URI = "http://mongodb.onconfluence.com/rpc/soap-axis/confluenceservice-v1?wsdl"
USERNAME = "soap"
PASSWORD = "soap"
AUTH_URI = "http://www.mongodb.org/login.action?os_authType=basic"
TMP_DIR = "confluence-tmp"
TMP_FILE = "confluence-tmp.zip"


def export_and_get_uri():
    client = Client(SOAP_URI)
    auth = client.service.login(USERNAME, PASSWORD)
    return client.service.exportSpace(auth, "DOCS", "TYPE_HTML")


def login_and_download(docs):
    cookie_jar = cookielib.CookieJar()
    cookie_handler = urllib2.HTTPCookieProcessor(cookie_jar)
    password_manager = urllib2.HTTPPasswordMgrWithDefaultRealm()
    password_manager.add_password(None, AUTH_URI, USERNAME, PASSWORD)
    auth_handler = urllib2.HTTPBasicAuthHandler(password_manager)
    urllib2.build_opener(cookie_handler, auth_handler).open(AUTH_URI)
    return urllib2.build_opener(cookie_handler).open(docs)


def extract_to_dir(data, dir):
    with open(TMP_FILE, "w") as f:
        f.write(data.read())
    data.close()
    # This is all really annoying but zipfile doesn't do extraction on 2.5
    subprocess.call(["unzip", "-d", dir, TMP_FILE])
    os.unlink(TMP_FILE)


def rmdir(dir):
    try:
        shutil.rmtree(dir)
    except:
        pass


def overwrite(src, dest):
    target = "%s/DOCS-%s/" % (dest, datetime.date.today())
    current = "%s/current" % dest
    rmdir(target)
    shutil.copytree(src, target)
    try:
        os.unlink(current)
    except:
        pass
    os.symlink(os.path.abspath(target), os.path.abspath(current))


def main(dir):
    rmdir(TMP_DIR)
    extract_to_dir(login_and_download(export_and_get_uri()), TMP_DIR)
    overwrite("%s/DOCS/" % TMP_DIR, dir)


if __name__ == "__main__":
    try:
        main(sys.argv[1])
    except IndexError:
        print "pass outdir as first arg"
