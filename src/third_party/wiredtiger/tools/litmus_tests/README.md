# WiredTiger litmus tests
In order to support lock free algorithms in the WiredTiger codebase, we define a number of litmus test. These test are intended to be run by the herd7 simulator.

For any algorithm which has defined litmus tests they can be found under that algorithm's subdirectory.

To run the litmus tests either install and run herd7, instructions [here](https://github.com/herd/herdtools7/blob/master/INSTALL.md). Or run them from the web interface found [here](http://diy.inria.fr/www/#).

If a litmus test is required for X86 there should be one defined for ARM64 as well. The reverse is not neccesarily true as X86 has a stronger memory model than ARM.

### Litmus test style in WiredTiger
WiredTiger litmus tests must use spaces and not have any tabs in the file. There needs to be a single whitespace between the intial state definition block {}, and the process definition block. There must also be an additional whitespace line before the exists clause.

Test names should be separated with underscores, e.g. wt_gen_drain.

Additionally per WiredTiger's usual style there should be a newline at the end of the file.
