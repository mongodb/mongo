#!/usr/bin/python

'''Helper script for constructing an archive (zip or tar) from a list of files.

The output format (tar, tgz, zip) is determined from the file name, unless the user specifies
--format on the command line.

This script simplifies the specification of filename transformations, so that, e.g.,
src/mongo/foo.cpp and build/linux2/normal/buildinfo.cpp can get put into the same
directory in the archive, perhaps mongodb-2.0.2/src/mongo.

Usage:

make_archive.py -o <output-file> [--format (tar|tgz|zip)] \
    [--transform match1=replacement1 [--transform match2=replacement2 [...]]] \
    <input file 1> [...]

If the input file names start with "@", the file is expected to contain a list of
whitespace-separated file names to include in the archive.  This helps get around the Windows
command line length limit.

Transformations are processed in command-line order and are short-circuiting.  So, if a file matches
match1, it is never compared against match2 or later.  Matches are just python startswith()
comparisons.

For a detailed usage example, see src/SConscript.client or src/mongo/SConscript.
'''

import optparse
import os
import sys
import shutil
import zipfile
from subprocess import (Popen,
                        PIPE,
                        STDOUT)

def main(argv):
    opts = parse_options(argv[1:])
    if opts.archive_format in ('tar', 'tgz'):
        make_tar_archive(opts)
    elif opts.archive_format in ('zip'):
        make_zip_archive(opts)
    else:
        raise ValueError('Unsupported archive format "%s"' % opts.archive_format)

def make_tar_archive(opts):
    '''Given the parsed options, generates the 'opt.output_filename'
    tarball containing all the files in 'opt.input_filename' renamed
    according to the mappings in 'opts.transformations'.

    e.g. for an input file named "a/mongo/build/DISTSRC", and an
    existing transformation {"a/mongo/build": "release"}, the input
    file will be written to the tarball as "release/DISTSRC"

    All files to be compressed are copied into new directories as
    required by 'opts.transformations'. Once the tarball has been
    created, all temporary directory structures created for the
    purposes of compressing, are removed.
    '''
    tar_options = "cvf"
    if opts.archive_format is 'tgz':
        tar_options += "z"
    created_directories = []
    tar_command = ["tar", tar_options, opts.output_filename]
    for input_filename in opts.input_filenames:
        preferred_filename = get_preferred_filename(input_filename, opts.transformations)
        tar_command.append(preferred_filename)
        print "copying %s => %s" % (input_filename, preferred_filename)
        enclosing_directory = os.path.dirname(preferred_filename)
        if not os.path.exists(enclosing_directory):
            os.makedirs(enclosing_directory)
            created_directories.append(enclosing_directory)
        shutil.copyfile(input_filename, preferred_filename)

    # execute the full tar command
    proc = Popen(tar_command, stdout=PIPE, stderr=STDOUT, bufsize=0)
    proc.wait()

    # clean up temp directories/files created
    for input_filename in opts.input_filenames:
        preferred_filename = get_preferred_filename(input_filename, opts.transformations)
        # don't delete the original file
        if input_filename is preferred_filename:
            continue
        enclosing_directory = os.path.dirname(preferred_filename)
        try:
            # only delete enclosing directory if we created it
            if enclosing_directory in created_directories:
                shutil.rmtree(enclosing_directory)
            else:
                os.remove(preferred_filename)
        except Exception:
            pass

def make_zip_archive(opts):
    '''Given the parsed options, generates the 'opt.output_filename'
    zipfile containing all the files in 'opt.input_filename' renamed
    according to the mappings in 'opts.transformations'.

    All files in 'opt.output_filename' are renamed before being
    written into the zipfile.
    '''
    archive = open_zip_archive_for_write(opts.output_filename)
    try:
        for input_filename in opts.input_filenames:
            archive.add(input_filename, arcname=get_preferred_filename(input_filename,
            opts.transformations))
    finally:
        archive.close()


def parse_options(args):
    parser = optparse.OptionParser()
    parser.add_option('-o', dest='output_filename', default=None,
                      help='Name of the archive to output.', metavar='FILE')
    parser.add_option('--format', dest='archive_format', default=None,
                      choices=('zip', 'tar', 'tgz'),
                      help='Format of archive to create.  '
                      'If omitted, use the suffix of the output filename to decide.')
    parser.add_option('--transform', action='append', dest='transformations', default=[])

    (opts, input_filenames) = parser.parse_args(args)
    opts.input_filenames = []

    for input_filename in input_filenames:
        if input_filename.startswith('@'):
            opts.input_filenames.extend(open(input_filename[1:], 'r').read().split())
        else:
            opts.input_filenames.append(input_filename)

    if opts.output_filename is None:
        parser.error('-o switch is required')

    if opts.archive_format is None:
        if opts.output_filename.endswith('.zip'):
            opts.archive_format = 'zip'
        elif opts.output_filename.endswith('tar.gz') or opts.output_filename.endswith('.tgz'):
            opts.archive_format = 'tgz'
        elif opts.output_filename.endswith('.tar'):
            opts.archive_format = 'tar'
        else:
            parser.error('Could not deduce archive format from output filename "%s"' %
                         opts.output_filename)

    try:
        opts.transformations = [
            xform.replace(os.path.altsep or os.path.sep, os.path.sep).split('=', 1)
            for xform in opts.transformations]
    except Exception, e:
        parser.error(e)

    return opts

def open_zip_archive_for_write(filename):
    '''Open a zip archive for writing and return it.
    '''
    # Infuriatingly, Zipfile calls the "add" method "write", but they're otherwise identical,
    # for our purposes.  WrappedZipFile is a minimal adapter class.
    class WrappedZipFile(zipfile.ZipFile):
        def add(self, filename, arcname):
            return self.write(filename, arcname)
    return WrappedZipFile(filename, 'w', zipfile.ZIP_DEFLATED)

def get_preferred_filename(input_filename, transformations):
    '''Does a prefix subsitution on 'input_filename' for the
    first matching transformation in 'transformations' and
    returns the substituted string
    '''
    for match, replace in transformations:
        if input_filename.startswith(match):
            return replace + input_filename[len(match):]
    return input_filename

if __name__ == '__main__':
    main(sys.argv)
    sys.exit(0)
