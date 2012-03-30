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

def main(argv):
    opts = parse_options(argv[1:])
    archive = open_archive_for_write(opts.output_filename, opts.archive_format)
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

def open_archive_for_write(filename, archive_format):
    '''Open a tar or zip archive for write, with the given format, and return it.

    The type of archive is determined by the "archive_format" parameter, which should be
    "tar", "tgz" (for gzipped tar) or "zip".
    '''

    if archive_format in ('tar', 'tgz'):
        import tarfile
        mode = 'w'
        if archive_format is 'tgz':
            mode += '|gz'
        return tarfile.open(filename, mode)
    if archive_format is 'zip':
        import zipfile
        # Infuriatingly, Zipfile calls the "add" method "write", but they're otherwise identical,
        # for our purposes.  WrappedZipFile is a minimal adapter class.
        class WrappedZipFile(zipfile.ZipFile):
            def add(self, filename, arcname):
                return self.write(filename, arcname)
        return WrappedZipFile(filename, 'w', zipfile.ZIP_DEFLATED)
    raise ValueError('Unsupported archive format "%s"' % archive_format)

def get_preferred_filename(input_filename, transformations):
    for match, replace in transformations:
        if input_filename.startswith(match):
            return replace + input_filename[len(match):]
    return input_filename

if __name__ == '__main__':
    main(sys.argv)
    sys.exit(0)
