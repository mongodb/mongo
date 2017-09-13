#!/usr/bin/python

#
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at http://mozilla.org/MPL/2.0/.

"""
Runs the static rooting analysis
"""

from subprocess import Popen
import subprocess
import os
import argparse
import sys
import re

def env(config):
    e = dict(os.environ)
    e['PATH'] = ':'.join(p for p in (config.get('gcc_bin'), config.get('sixgill_bin'), e['PATH']) if p)
    e['XDB'] = '%(sixgill_bin)s/xdb.so' % config
    e['SOURCE'] = config['source']
    e['ANALYZED_OBJDIR'] = config['objdir']
    return e

def fill(command, config):
    try:
        return tuple(s % config for s in command)
    except:
        print("Substitution failed:")
        problems = []
        for fragment in command:
            try:
                fragment % config
            except:
                problems.append(fragment)
        raise Exception("\n".join(["Substitution failed:"] + [ "  %s" % s for s in problems ]))

def print_command(command, outfile=None, env=None):
    output = ' '.join(command)
    if outfile:
        output += ' > ' + outfile
    if env:
        changed = {}
        e = os.environ
        for key,value in env.items():
            if (key not in e) or (e[key] != value):
                changed[key] = value
        if changed:
            outputs = []
            for key, value in changed.items():
                if key in e and e[key] in value:
                    start = value.index(e[key])
                    end = start + len(e[key])
                    outputs.append('%s="%s${%s}%s"' % (key,
                                                       value[:start],
                                                       key,
                                                       value[end:]))
                else:
                    outputs.append("%s='%s'" % (key, value))
            output = ' '.join(outputs) + " " + output

    print output

def generate_hazards(config, outfilename):
    jobs = []
    for i in range(int(config['jobs'])):
        command = fill(('%(js)s',
                        '%(analysis_scriptdir)s/analyzeRoots.js',
                        '%(gcFunctions_list)s',
                        '%(gcEdges)s',
                        '%(suppressedFunctions_list)s',
                        '%(gcTypes)s',
                        str(i+1), '%(jobs)s',
                        'tmp.%s' % (i+1,)),
                       config)
        outfile = 'rootingHazards.%s' % (i+1,)
        output = open(outfile, 'w')
        print_command(command, outfile=outfile, env=env(config))
        jobs.append((command, Popen(command, stdout=output, env=env(config))))

    final_status = 0
    while jobs:
        pid, status = os.wait()
        jobs = [ job for job in jobs if job[1].pid != pid ]
        final_status = final_status or status

    if final_status:
        raise subprocess.CalledProcessError(final_status, 'analyzeRoots.js')

    with open(outfilename, 'w') as output:
        command = ['cat'] + [ 'rootingHazards.%s' % (i+1,) for i in range(int(config['jobs'])) ]
        print_command(command, outfile=outfilename)
        subprocess.call(command, stdout=output)

JOBS = { 'dbs':
             (('%(ANALYSIS_SCRIPTDIR)s/run_complete',
               '--foreground',
               '--no-logs',
               '--build-root=%(objdir)s',
               '--wrap-dir=%(sixgill)s/scripts/wrap_gcc',
               '--work-dir=work',
               '-b', '%(sixgill_bin)s',
               '--buildcommand=%(buildcommand)s',
               '.'),
              ()),

         'callgraph':
             (('%(js)s', '%(analysis_scriptdir)s/computeCallgraph.js'),
              'callgraph.txt'),

         'gcFunctions':
             (('%(js)s', '%(analysis_scriptdir)s/computeGCFunctions.js', '%(callgraph)s',
               '[gcFunctions]', '[gcFunctions_list]', '[gcEdges]', '[suppressedFunctions_list]'),
              ('gcFunctions.txt', 'gcFunctions.lst', 'gcEdges.txt', 'suppressedFunctions.lst')),

         'gcTypes':
             (('%(js)s', '%(analysis_scriptdir)s/computeGCTypes.js',),
              'gcTypes.txt'),

         'allFunctions':
             (('%(sixgill_bin)s/xdbkeys', 'src_body.xdb',),
              'allFunctions.txt'),

         'hazards':
             (generate_hazards, 'rootingHazards.txt'),

         'explain':
             (('python', '%(analysis_scriptdir)s/explain.py',
               '%(hazards)s', '%(gcFunctions)s',
               '[explained_hazards]', '[unnecessary]', '[refs]'),
              ('hazards.txt', 'unnecessary.txt', 'refs.txt'))
         }

def out_indexes(command):
    for i in range(len(command)):
        m = re.match(r'^\[(.*)\]$', command[i])
        if m:
            yield (i, m.group(1))

def run_job(name, config):
    cmdspec, outfiles = JOBS[name]
    print("Running " + name + " to generate " + str(outfiles))
    if hasattr(cmdspec, '__call__'):
        cmdspec(config, outfiles)
    else:
        temp_map = {}
        cmdspec = fill(cmdspec, config)
        if isinstance(outfiles, basestring):
            stdout_filename = '%s.tmp' % name
            temp_map[stdout_filename] = outfiles
            print_command(cmdspec, outfile=outfiles, env=env(config))
        else:
            stdout_filename = None
            pc = list(cmdspec)
            outfile = 0
            for (i, name) in out_indexes(cmdspec):
                pc[i] = outfiles[outfile]
                outfile += 1
            print_command(pc, env=env(config))

        command = list(cmdspec)
        outfile = 0
        for (i, name) in out_indexes(cmdspec):
            command[i] = '%s.tmp' % name
            temp_map[command[i]] = outfiles[outfile]
            outfile += 1

        sys.stdout.flush()
        if stdout_filename is None:
            subprocess.check_call(command, env=env(config))
        else:
            with open(stdout_filename, 'w') as output:
                subprocess.check_call(command, stdout=output, env=env(config))
        for (temp, final) in temp_map.items():
            try:
                os.rename(temp, final)
            except OSError:
                print("Error renaming %s -> %s" % (temp, final))
                raise

config = { 'ANALYSIS_SCRIPTDIR': os.path.dirname(__file__) }

defaults = [ '%s/defaults.py' % config['ANALYSIS_SCRIPTDIR'],
             '%s/defaults.py' % os.getcwd() ]

for default in defaults:
    try:
        execfile(default, config)
        print("Loaded %s" % default)
    except:
        pass

data = config.copy()

parser = argparse.ArgumentParser(description='Statically analyze build tree for rooting hazards.')
parser.add_argument('step', metavar='STEP', type=str, nargs='?',
                    help='run starting from this step')
parser.add_argument('--source', metavar='SOURCE', type=str, nargs='?',
                    help='source code to analyze')
parser.add_argument('--upto', metavar='UPTO', type=str, nargs='?',
                    help='last step to execute')
parser.add_argument('--jobs', '-j', default=None, metavar='JOBS', type=int,
                    help='number of simultaneous analyzeRoots.js jobs')
parser.add_argument('--list', const=True, nargs='?', type=bool,
                    help='display available steps')
parser.add_argument('--buildcommand', '--build', '-b', type=str, nargs='?',
                    help='command to build the tree being analyzed')
parser.add_argument('--tag', '-t', type=str, nargs='?',
                    help='name of job, also sets build command to "build.<tag>"')
parser.add_argument('--expect-file', type=str, nargs='?',
                    help='deprecated option, temporarily still present for backwards compatibility')

args = parser.parse_args()
for k,v in vars(args).items():
    if v is not None:
        data[k] = v

if args.tag and not args.buildcommand:
    args.buildcommand="build.%s" % args.tag

if args.jobs is not None:
    data['jobs'] = args.jobs
if not data.get('jobs'):
    data['jobs'] = subprocess.check_output(['nproc', '--ignore=1'])

if args.buildcommand:
    data['buildcommand'] = args.buildcommand
elif 'BUILD' in os.environ:
    data['buildcommand'] = os.environ['BUILD']
else:
    data['buildcommand'] = 'make -j4 -s'

if 'ANALYZED_OBJDIR' in os.environ:
    data['objdir'] = os.environ['ANALYZED_OBJDIR']

if 'SOURCE' in os.environ:
    data['source'] = os.environ['SOURCE']
if not data.get('source') and data.get('sixgill_bin'):
    path = subprocess.check_output(['sh', '-c', data['sixgill_bin'] + '/xdbkeys file_source.xdb | grep jsapi.cpp'])
    data['source'] = path.replace("/js/src/jsapi.cpp", "")

steps = [ 'dbs',
          'callgraph',
          'gcTypes',
          'gcFunctions',
          'allFunctions',
          'hazards',
          'explain' ]

if args.list:
    for step in steps:
        command, outfilename = JOBS[step]
        if outfilename:
            print("%s -> %s" % (step, outfilename))
        else:
            print(step)
    sys.exit(0)

for step in steps:
    command, outfiles = JOBS[step]
    if isinstance(outfiles, basestring):
        data[step] = outfiles
    else:
        outfile = 0
        for (i, name) in out_indexes(command):
            data[name] = outfiles[outfile]
            outfile += 1
        assert len(outfiles) == outfile, 'step \'%s\': mismatched number of output files and params' % step

if args.step:
    steps = steps[steps.index(args.step):]

if args.upto:
    steps = steps[:steps.index(args.upto)+1]

for step in steps:
    run_job(step, data)
