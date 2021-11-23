#!/usr/bin/env python
#
# Public Domain 2014-present MongoDB, Inc.
# Public Domain 2008-2014 WiredTiger, Inc.
#
# This is free and unencumbered software released into the public domain.
#
# Anyone is free to copy, modify, publish, use, compile, sell, or
# distribute this software, either in source code form or as a compiled
# binary, for any purpose, commercial or non-commercial, and by any
# means.
#
# In jurisdictions that recognize copyright laws, the author or authors
# of this software dedicate any and all copyright interest in the
# software to the public domain. We make this dedication for the benefit
# of the public at large and to the detriment of our heirs and
# successors. We intend this dedication to be an overt act of
# relinquishment in perpetuity of all present and future rights to this
# software under copyright law.
#
# THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
# EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
# MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.
# IN NO EVENT SHALL THE AUTHORS BE LIABLE FOR ANY CLAIM, DAMAGES OR
# OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE,
# ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR
# OTHER DEALINGS IN THE SOFTWARE.

# Usage: python test_conf_dump.py <optional-wtperf-config>
#
# This script tests if the config file dumped in the test directory corresponds
# correctly to the wtperf config file used. Command line options to wtperf are
# also taken into account.
#
# Following expectations are checked for:
# 1. If provided through multiple sources, "conn_config" and "table_config"
#    configuration options are appended to each other. All other options get
#    replaced by a higher precedent source.
# 2. The precedence order for the options in an increasing order is as follows:
#    default option,
#    provided through config file,
#    provided through option -o
#    provided through option -C (for conn_config) or -T (for table_config)
#
# Test fails if any config option is missing or has a wrong value. Test also
# fails if the value for the option is not replaced/appended in the correct
# order of precedence as stated above.

import os, re, subprocess, sys

OP_FILE = "WT_TEST/CONFIG.wtperf"
TMP_CONF = "__tmp.wtperf"
WTPERF_BIN = "./wtperf"

env_wtperfdir = os.getenv('WTPERF_DIR')
if env_wtperfdir:
    WTPERF_DIR = env_wtperfdir
else:
    WTPERF_DIR = "../../build_posix/bench/wtperf/"

CONF_NOT_PROVIDED = -2

# Generate a wtperf conf file to use
def generate_conf_file(file_name):
    f = open(file_name, 'w')
    f.write(
'''conn_config="cache_size=16GB,eviction=(threads_max=4),log=(enabled=false),session_max=33"
table_config="leaf_page_max=32k,internal_page_max=16k,allocation_size=4k,split_pct=90,type=file"
close_conn=false
icount=1500
create=true
compression="snappy"
checkpoint_interval=5
checkpoint_threads=1
populate_threads=1
report_interval=5
session_count_idle=50
session_count_idle=60
session_count_idle=70
session_count_idle=80
run_time=5
sample_interval=5
sample_rate=1
table_count=2
threads=((count=6,updates=1))
value_sz=1000
warmup=2
''')
    f.close()

# Build a command from the given options and execute wtperf
def execute_wtperf(conf_file, option_C = "", option_T = "", option_o = ""):
    # Generate the command to run, execute wtperf
    cmd = WTPERF_BIN + " -O " + conf_file
    if option_C:
        cmd += " -C " + option_C
    if option_T:
        cmd += " -T " + option_T
    if option_o:
        # Any quotes in option_o need to be escaped before providing it as part
        # of the command
        option_o_cmd_str = option_o.replace('"', '\\"')
        cmd += " -o " + option_o_cmd_str

    print("Running:  " + cmd)
    subprocess.check_call(cmd, shell=True)
    print("=========================\n")

# Build a dictionary of config key and it's value from the given config file.
# Optionally take -C, -T and -o and overwrite/append values as per correct
# precedence
def build_dict_from_conf(
        conf_file, option_C = "", option_T = "", option_o = ""):
    # Open given conf file and make a dictionary of passed arguments and values
    with open(conf_file) as f:
        lines = f.read().splitlines()

        # Maintain precedence order of config file, -o, -C/-T
        # Build a dict of config options, appending values for table_config and
        # conn_config, if specified multiple times. Replace with the latest in
        # case of all other configuration keys.
        key_val_dict = {}
        for line in lines:
            if re.match('^\s*#', line) is None:
                key_val_pair = line.split('=', 1)
                if ((key_val_pair[0] == 'table_config' or
                     key_val_pair[0] == 'conn_config') and
                     key_val_pair[0] in key_val_dict):
                    tmp_val = key_val_dict[key_val_pair[0]][:-1]
                    tmp_val += ","
                    tmp_val += key_val_pair[1][1:]
                    key_val_dict[key_val_pair[0]] = tmp_val
                else:
                    key_val_dict[key_val_pair[0]] = key_val_pair[1]

        # If provided, put option o in the dict
        if option_o:
            opt_o_key_val_list = option_o.split(',')
            for op_o_key_val in opt_o_key_val_list:
                key_val_pair = op_o_key_val.split('=', 1)
                if ((key_val_pair[0] == 'table_config' or
                     key_val_pair[0] == 'conn_config') and
                     key_val_pair[0] in key_val_dict):
                    tmp_val = key_val_dict[key_val_pair[0]][:-1]
                    tmp_val += ","
                    tmp_val += key_val_pair[1][1:]
                    key_val_dict[key_val_pair[0]] = tmp_val
                else:
                    key_val_dict[key_val_pair[0]] = key_val_pair[1]

        # If provided, put option C in the dict
        if option_C:
            tmp_val = key_val_dict["conn_config"][:-1]
            tmp_val += ","
            tmp_val += option_C[1:]
            key_val_dict["conn_config"] = tmp_val

        # If provided, put option T in the dict
        if option_T:
            tmp_val = key_val_dict["table_config"][:-1]
            tmp_val += ","
            tmp_val += option_T[1:]
            key_val_dict["table_config"] = tmp_val

        return key_val_dict

# Extract configuration value for the given key from the given config file
def extract_config_from_file(conf_file, key):
    ret_val = ""
    with open(conf_file) as f:
        lines = f.read().splitlines()
        for line in lines:
            if re.match('^\s*#', line) is None:
                key_val_pair = line.split('=', 1)
                if key_val_pair[0] == key:
                    ret_val = key_val_pair[1]
    return ret_val

# Extract configuration value for the given key from the given "-o" string
def extract_config_from_opt_o(option_o, key):
    ret_val = ""
    opt_o_key_val_list = option_o.split(',')
    for op_o_key_val in opt_o_key_val_list:
        key_val_pair = op_o_key_val.split('=', 1)
        if key_val_pair[0] == key:
            ret_val = key_val_pair[1]
    return ret_val

# Execute test:
# Run wtperf with given config and check if the dumped config file matches the
# given inputs
def run_test(conf_file, option_C = "", option_T = "", option_o = ""):
    # Run wtperf
    execute_wtperf(conf_file, option_C, option_T, option_o)

    key_val_dict_ip = build_dict_from_conf(
        conf_file, option_C, option_T, option_o)
    key_val_dict_op = build_dict_from_conf(OP_FILE)

    conn_config_from_file = extract_config_from_file(conf_file, "conn_config")
    table_config_from_file = extract_config_from_file(conf_file, "table_config")
    conn_config_from_opt_o = ""
    table_config_from_opt_o = ""
    if option_o:
        conn_config_from_opt_o = extract_config_from_opt_o(
            option_o, "conn_config")
        table_config_from_opt_o = extract_config_from_opt_o(
            option_o, "table_config")

    # Check if dumped output conf matches with input file and options
    match = True
    for key in key_val_dict_ip:
        match_itr = True

        # Check if we see this config key in the dumped file
        if not key in key_val_dict_op:
            print("Key '" + key + "' not found in dumped file " + OP_FILE)
            match = match_itr = False
            continue

        # Check if values from all sources of conn_config are presented in the
        # conn_config in dumped file. Also check of their relative ordering as
        # per precedence rules defined.
        if (key == 'conn_config' and
            (conn_config_from_file or conn_config_from_opt_o or option_C)):
            # Should find these config in order: file < option o < option C
            file_loc = CONF_NOT_PROVIDED
            option_o_loc = CONF_NOT_PROVIDED
            option_C_loc = CONF_NOT_PROVIDED
            op_conn_config = key_val_dict_op['conn_config']

            if conn_config_from_file:
                file_loc = op_conn_config.find(conn_config_from_file[1:-1])
            if conn_config_from_opt_o:
                option_o_loc = op_conn_config.find(conn_config_from_opt_o[1:-1])
            if option_C:
                option_C_loc = op_conn_config.find(option_C[1:-1])

            # Check if value from any of the sources is missing
            if ((conn_config_from_file and file_loc == -1) or
                (conn_config_from_opt_o and option_o_loc == -1) or
                (option_C and option_C_loc == -1)):
                print("Part of conn_config missing in dumped file " + OP_FILE)
                match_itr = False

            # Check if the values got appended in the correct order
            if match_itr:
                if ((option_o_loc != CONF_NOT_PROVIDED and
                     option_o_loc < file_loc) or
                    (option_C_loc != CONF_NOT_PROVIDED and
                     (option_C_loc < file_loc or option_C_loc < option_o_loc))):
                    print("Detected incorrect config append order:")
                    match_itr = False

        # Check if values from all sources of table_config are presented in the
        # table_config in dumped file. Also check of their relative ordering as
        # per precedence rules defined.
        if (key == 'table_config' and
            (table_config_from_file or table_config_from_opt_o or option_T)):
            # Should find these config in order: file < option o < option T
            file_loc = CONF_NOT_PROVIDED
            option_o_loc = CONF_NOT_PROVIDED
            option_T_loc = CONF_NOT_PROVIDED
            op_table_config = key_val_dict_op['table_config']

            if table_config_from_file:
                file_loc = op_table_config.find(table_config_from_file[1:-1])
            if table_config_from_opt_o:
                option_o_loc = op_table_config.find(
                    table_config_from_opt_o[1:-1])
            if option_T:
                option_T_loc = op_table_config.find(option_T[1:-1])

            # Check if value from any of the sources is missing
            if ((table_config_from_file and file_loc == -1) or
                (table_config_from_opt_o and option_o_loc == -1) or
                (option_T and option_T_loc == -1)):
                print("Part of table_config missing in dumped file " + OP_FILE)
                match_itr = False

            # Check if the values got appended in the correct order
            if match_itr:
                if ((option_o_loc != CONF_NOT_PROVIDED and
                     option_o_loc < file_loc) or
                    (option_T_loc != CONF_NOT_PROVIDED and
                     (option_T_loc < file_loc or option_T_loc < option_o_loc))):
                    print("Detected incorrect config append order:")
                    match_itr = False

        if (key != 'table_config' and key != 'conn_config' and
            key_val_dict_ip[key] != key_val_dict_op[key]):
            print("Config mismatch between:")
            match_itr = False

        if match_itr is False:
            print("Input Config:" + key + '=' + key_val_dict_ip[key])
            print("Dumped Config:" + key + '=' + key_val_dict_op[key])
            print("\n")

        match = match and match_itr

    return match

# ----------------- Execute Test --------------
# If a wtperf conf file is provided use it, else generate a temp conf file
os.chdir(WTPERF_DIR)
if len(sys.argv) == 2:
    conf_file = sys.argv[1]
else:
    conf_file = TMP_CONF
    generate_conf_file(conf_file)

# Run a test with no options
if not run_test(conf_file):
    exit(-1)

# Run a test with -C, -T, -o provided
option_o = "verbose=2,conn_config=\"session_max=135\",table_config=\"type=lsm\",sample_interval=2,run_time=0,sample_rate=2,readonly=false"
option_C = "\"cache_size=10GB,session_max=115\""
option_T = "\"allocation_size=8k,split_pct=92\""
if not run_test(conf_file, option_C, option_T, option_o):
    exit(-1)

# Cleanup generated temp files
subprocess.check_call("rm -rf WT_TEST/", shell=True)
if len(sys.argv) == 1 and conf_file == TMP_CONF:
    subprocess.check_call("rm " + TMP_CONF, shell=True)

print("All tests succeeded")
