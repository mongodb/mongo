#!/usr/bin/env python
#
# Copyright (c) 2018 Stefan Seefeld
# All rights reserved.
#
# This file is part of Boost.uBLAS. It is made available under the
# Boost Software License, Version 1.0.
# (Consult LICENSE or http://www.boost.org/LICENSE_1_0.txt)

import argparse
import matplotlib.pyplot as plt
import numpy as np


class plot(object):

    def __init__(self, label, data):
        self.label = label
        self.data = data


def load_file(filename):

    lines = open(filename, 'r').readlines()
    label = lines[0][1:-1].strip()
    lines = [l.strip() for l in lines]
    lines = [l.split('#', 1)[0] for l in lines]
    lines = [l for l in lines if l]
    data = [l.split() for l in lines]
    return plot(label, list(zip(*data)))


def main(argv):

    parser = argparse.ArgumentParser(prog=argv[0], description='benchmark plotter')
    parser.add_argument('data', nargs='+', help='benchmark data to plot')
    parser.add_argument('--log', choices=['no', 'all', 'x', 'y'], help='use a logarithmic scale')
    args = parser.parse_args(argv[1:])
    runs = [load_file(d) for d in args.data]
    plt.title('Benchmark plot')
    plt.xlabel('size')
    plt.ylabel('time (s)')
    if args.log == 'all':
        plot = plt.loglog
    elif args.log == 'x':
        plot = plt.semilogx
    elif args.log == 'y':
        plot = plt.semilogy
    else:
        plot = plt.plot
    plots = [plot(r.data[0], r.data[1], label=r.label) for r in runs]
    plt.legend()
    plt.show()
    return True

    
if __name__ == '__main__':

    import sys
    sys.exit(0 if main(sys.argv) else 1)
