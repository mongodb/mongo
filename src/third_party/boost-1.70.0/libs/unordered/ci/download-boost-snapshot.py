#!/usr/bin/env python

import urllib, os, os.path, sys, json, tarfile, zipfile, tempfile

def download(snapshot):
    if snapshot == 'stable':
        # TODO: Default version/filename if not available?
        downloads = [
            "https://sourceforge.net/projects/boost/files/boost/%s/%s.tar.bz2/download" %
                (os.environ['BOOST_VERSION'], os.environ['BOOST_FILENAME'])]
    else:
        json_response = urllib.urlopen('https://api.bintray.com/packages/boostorg/%s/snapshot/files' % (snapshot))
        x = json.load(json_response)

        extension_priorities = { '.bz2': 2, '.gz': 1, '.zip': 0 }
        file_list = []
        version_dates = {}
        for file in x:
            file_extension = os.path.splitext(file['path'])[1]
            if (file_extension in extension_priorities):
                file['priority'] = extension_priorities[file_extension]
                file_list.append(file)
                if not file['version'] in version_dates or file['created'] < version_dates[file['version']]:
                    version_dates[file['version']] = file['created']
        file_list.sort(key=lambda x: (version_dates[x['version']], x['priority']), reverse=True)
        downloads = ['http://dl.bintray.com/boostorg/%s/%s' % (snapshot, file['path']) for file in file_list]

    filename = ''
    for download_url in downloads:
        try:
            print "Downloading: " + download_url
            (filename, headers) = urllib.urlretrieve(download_url)

            print "Extracting: " + filename
            dir = tempfile.mkdtemp()
            extract(filename, dir)
            os.remove(filename)
            files = os.listdir(dir)
            assert(len(files) == 1)
            os.rename(os.path.join(dir, files[0]), 'boost')
            return
        except IOError:
            print "Error opening URL: " + download_url

def extract(filename, path = '.'):
    if (filename.endswith(".gz")):
        tar = tarfile.open(filename, "r:gz")
        tar.extractall(path)
        tar.close
    elif (filename.endswith(".bz2")):
        tar = tarfile.open(filename, "r:bz2")
        tar.extractall(path)
        tar.close
    elif (filename.endswith(".zip")):
        zip = zipfile.ZipFile(filename, "r")
        zip.extractall(path)
        zip.close
    else:
        assert False

if len(sys.argv) == 1:
    download('stable')
elif len(sys.argv) == 2:
    download(sys.argv[1])
else:
    print "Usage: %s [stable|branch-name]" % (sys.argv[0])
