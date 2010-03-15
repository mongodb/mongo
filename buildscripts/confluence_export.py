#! /usr/bin/env python

# Export the contents on confluence
#
# Dependencies:
#   - suds
#
# User: soap, Password: soap
import cookielib
import cStringIO as StringIO
import shutil
import sys
import urllib2
import zipfile

from suds.client import Client

SOAP_URI = "http://mongodb.onconfluence.com/rpc/soap-axis/confluenceservice-v1?wsdl"
USERNAME = "soap"
PASSWORD = "soap"
AUTH_URI = "http://www.mongodb.org/login.action?os_authType=basic"
TMP_DIR = "confluence-tmp"


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
    buffer = StringIO.StringIO(data.read())
    data.close()
    zip = zipfile.ZipFile(buffer)
    zip.extractall(dir)


def rmdir(dir):
    try:
        shutil.rmtree(dir)
    except:
        pass


def overwrite(src, dest):
    rmdir(dest)
    shutil.copytree(src, dest)


def main(dir):
    rmdir(TMP_DIR)
    extract_to_dir(login_and_download(export_and_get_uri()), TMP_DIR)
    overwrite("%s/DOCS/" % TMP_DIR, dir)


if __name__ == "__main__":
    try:
        main(sys.argv[1])
    except IndexError:
        print "pass outdir as first arg"
