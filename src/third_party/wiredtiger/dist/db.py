# A simple python script to build a file that can be bulk-loaded into a
# WiredTiger database for smoke-testing.

import getopt, random, sys

dmin = 7        # Minimum data size
dmax = 837      # Maximum data size

seed = None     # Random number seed
pairs = 100000  # Key/data pairs to output

opts, args = getopt.getopt(sys.argv[1:], "m:n:s:")
for o, a in opts:
    if o == "-m":
        dmax = int(a)
    elif o == "-n":
        pairs = int(a)
    elif o == "-s":
        seed = int(a)

random.seed(seed)
for i in range(pairs):
    fmt = "%010d\ndata: %0" + str(random.randrange(dmin, dmax)) + "d"
    print(fmt % (i, i))
