#!/usr/bin/env python3
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

# A python script to run the WT tool verify command.
# This script wraps the wt util tool and takes in arguments for the tool,
# handles the output and provides additional processing options on the output
# such as pretty printed output and visualization options.

# This script only supports Row store and Variable Length Column Store (VLCS).
# Fixed Length Column Store (FLCS) is not supported.

import argparse
from operator import itemgetter
import os
import subprocess
import json
import sys
import re
import numpy as np
import pandas as pd
import matplotlib
matplotlib.use('WebAgg')
import matplotlib.pyplot as plt
import mpld3
from mpld3._server import serve

SEPARATOR = "=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=\n"
WT_OUTPUT_FILE = "wt_output_file.txt"
HISTOGRAM_CHOICES = ["page_mem_size", "dsk_mem_size", "entries"]
PIE_CHART_CHOICES = ["page_type"]
ALL_VISUALIZATION_CHOICES = HISTOGRAM_CHOICES + PIE_CHART_CHOICES

PLOT_COLORS = {
    "dsk_mem_size": {"internal": "#ff69b4", "leaf": "#ffb3ba"},
    "page_mem_size": {"internal": "#4169e1", "leaf": "#bae1ff"},
    "entries": {"internal": "#40e0d0", "leaf": "#baffc9"},
    "page_type": ["#ff7f50", "#ffdfba"],
}
TITLE_SIZE = 20

FIELD_TITLES = {
    "dsk_mem_size": "on disk size",
    "page_mem_size": "in memory size",
    "entries": "entries",
    "page_type": "page type ratio"
}

def output_pretty(output):
    """
    Print output for parsing script
    """
    str = "{"
    for i, checkpoint in enumerate(output):
        if i:
            str += ","
        str += "\n\t" + checkpoint + ": {"
        for j, key in enumerate(output[checkpoint]):
            value = output[checkpoint][key]
            if j:
                str += ","
            str += "\n\t\t" + key + ": {\n\t\t\t" + json.dumps(value)
            str += "\n\t\t}"
        str += "\n\t}"
    str += "\n}"
    return str


def is_int(string):
    """
    Check if string can be converted to an integer
    """
    try :
        return int(string)
    except (TypeError, ValueError):
        return string


def parse_metadata(line):
    """
    Parses the metadata 
    """
    dict = {}
    if line.startswith("\t> "): 
        line = line[3:-1]
        for x in line.split(" | "):
            [key, value] = x.split(": ", 1)
            if value[0] == "[" and value[-1] == "]":
                value = value[1:-1].split(", ")
                value = list(map(lambda n: is_int(n), value))
            if key == "addr": 
                temp = value[0].split(": ")
                dict.update({"object_id": is_int(temp[0]), "offset_range": temp[1], "size": is_int(value[1]), 
                            "checksum": is_int(value[2])})
            else:
                dict[key] = is_int(value)
    return dict


def parse_node(f, line, output, chkpt_info, root_addr, is_root_node):
    node = {}
    line = line[2:-1] # remove new node symbol
    page_type = line.split()[-1]
    expected_page_type = ["internal", "leaf"]
    if not page_type in expected_page_type:
        raise Exception(f"page_type expected to be one of {expected_page_type} but found '{page_type}'")
    node_id = line.split(": ")[0]
    line = f.readline()
    while line and line != SEPARATOR and not line.startswith("- "):
        node.update(parse_metadata(line)) # remove metadata symbol
        line = f.readline()
    if is_root_node:
        node.update(root_addr)
    output[chkpt_info][node_id] = node 
    return line


def parse_chkpt_info(f):
    """
    Parse the checkpoint(s) name and root info
    """
    line = f.readline() 
    chkpt_info = ""
    if m := re.search("\s*ckpt_name: (\S+)\s*", line):
        chkpt_info = m.group(1)
    else:
        raise RuntimeError("Could not find checkpoint name")
    line = f.readline()
    if not line.startswith("Root:"):
        if not line.strip():
            raise Exception("Root not found, there is no data to parse")
        raise Exception(f"Expected the line starts with 'Root:' but found '{line}'")
    line = f.readline()
    root_addr = parse_metadata(line[3:-1]) # remove metadata symbol
    line = f.readline()
    return [root_addr, line, chkpt_info]


def parse_dump_blocks(input_file: str):
    """
    Parse the output file of dump_blocks.
    Return a dictionary that has checkpoint names as keys. Each checkpoint has a set of keys that
    corresonds to each page type. Each page type has a list of tuples (offset, size) sorted by
    offset.
    """

    with open(input_file, "r") as f:
        lines = f.readlines()

    checkpoint_name = None
    is_root, has_root = False, False
    data = {}

    for line in lines:
        line = line.strip()

        # Check for root info.
        # (i.e > addr: [0: 4096-8192, 4096, 1421166157])
        if is_root:
            x = re.search(r"^> addr: \[\d+: (\d+)-\d+, (\d+), \d+]$", line)
            if not x:
                raise Exception(f"Root information expected in '{line}'")
            addr_start = int(x.group(1))
            size = int(x.group(2))

            data[checkpoint_name]['root'] = [(addr_start, size)]
            is_root = False
            continue

        # Check for a new checkpoint.
        # (i.e file:test_hs01.wt, ckpt_name: WiredTigerCheckpoint.5)
        if x := re.search(r"^file:.*.wt, ckpt_name: (.*)", line):
            checkpoint_name = x.group(1)
            data[checkpoint_name] = {}
            continue

        # Check for the root info.
        if x := re.search(r"^Root:$", line):
            has_root = is_root = True
            continue

        # Check for any other addr info.
        # (i.e <page_type>: ... addr: [0: 1171456-1200128, 28672, 3808881546])
        if x := re.search(r"^([A-Za-z_]*):.*addr: \[\d+: (\d+)-\d+, (\d+), \d+]$", line):
            page_type = x.group(1)
            addr_start = int(x.group(2))
            size = int(x.group(3))

            # If a cell type has an address, get its type.
            if page_type == 'cell_type':
                x = re.search(r"^cell_type: (\w+).*", line)
                if not x:
                    raise Exception(f"Cell type not found in {line}")
                page_type = x.group(1)

            if page_type not in data[checkpoint_name]:
                data[checkpoint_name][page_type] = []

            data[checkpoint_name][page_type] += [(addr_start, size)]
            continue
    if not has_root:
        raise Exception("Root not found, there is no data to parse")
    # Sort by offset.
    for checkpoint in data:
        for page_type in data[checkpoint]:
            data[checkpoint][page_type].sort(key=itemgetter(0))

    return data

def parse_dump_pages(input_file: str):
    """
    Parse the output file of dump_pages.
    """
    with open(input_file, "r") as f:
        output = {}
        line = f.readline()
        while line:
            if line != SEPARATOR:
                raise Exception(f"Expected '{SEPARATOR}' but found '{line}'")
            [root_addr, line, chkpt_info] = parse_chkpt_info(f)
            output[chkpt_info] = {}
            is_root_node = True
            while line and line != SEPARATOR:
                if not line.startswith("- "):
                    raise Exception(f"Expected the line starts with '- ' but found '{line}'")
                line = parse_node(f, line, output, chkpt_info, root_addr, is_root_node)
                is_root_node = False
    return output


def show_block_distribution_broken_barh(filename, checkpoints, bar_width=1):
    """
    Make a "broken" horizontal bar plot to show the on disk information contained in a file.
    checkpoints: dictionary generated by parse_dump_blocks() that contains information related to
    different checkpoints.
    """
    bar_width = 1
    colors = ['blue', 'orange', 'green', 'red', 'purple', 'brown', 'pink', 'gray', 'olive', 'cyan']

    # Remember the max offset seen and the associated data size.
    max_addr = 0
    max_addr_size = 0

    color_indx = 0
    page_types = []
    fig, ax = plt.subplots(1, figsize=(15, 10))

    for checkpoint in checkpoints:

        for page_type in checkpoints[checkpoint]:

            # Save the number of different page types we have seen for the plot.
            if page_type not in page_types:
                page_types.append(page_type)

            # As we may have multiple checkpoints, make sure we show the same page type on the same
            # y value.
            page_type_index = page_types.index(page_type)

            ax.broken_barh(checkpoints[checkpoint][page_type], (page_type_index, bar_width),
                           facecolors=f'tab:{colors[color_indx]}',
                           label=f"{page_type} ({checkpoint})")

            # Save the max offset seen for the plot assuming the offsets are sorted.
            if checkpoints[checkpoint][page_type][-1][0] > max_addr:
                max_addr = checkpoints[checkpoint][page_type][-1][0]
                max_addr_size = checkpoints[checkpoint][page_type][-1][1]

            # Change color for every page type and every checkpoint.
            color_indx += 1

    # Plot settings.
    plt.yticks(np.arange(0, len(page_types) + 1, 1))
    xlimit = max_addr + max_addr_size
    plt.xlim(left=0,right=xlimit)
    plt.xlabel("Offset")
    plt.ylabel("Page type")
    plt.legend()
    plt.title(f"Distribution of each page type for {filename}")
    plt.close()

    # Generate the image.
    img = mpld3.fig_to_html(fig)

    return img

def show_block_distribution_hist(filename, data, bins=100):
    """
    data: dictionary generated by parse_dump_blocks() that contains information related to different
    checkpoints.
    This generates two histograms: one for the distribution of the blocks in the file and another
    one for the distribution of block sizes.
    """
    for checkpoint in data:
        all_addr = []

        # Concatenate the lists for each page type:
        for page_type in data[checkpoint]:
            all_addr += data[checkpoint][page_type]

    # Plot settings for the distribution of the blocks in the file.
    fig, ax = plt.subplots(1, figsize=(15, 10))
    # Use the first component, which is the offset.
    ax.hist(list(map(itemgetter(0), all_addr)), bins=bins)
    plt.title(f"Distribution of blocks for {filename}")
    plt.xlabel(f"Offset (bins={bins})")
    plt.ylabel("Frequency (blocks)")
    plt.close()

    # Generate the image.
    imgs = ""
    imgs += mpld3.fig_to_html(fig)

    # Plot settings for the distribution of block sizes.
    fig, ax = plt.subplots(1, figsize=(15, 10))
    # Use the second component, which is the size.
    ax.hist(list(map(itemgetter(1), all_addr)), bins=bins)
    plt.title(f"Distribution of block sizes for {filename}")
    plt.xlabel(f"Offset (bins={bins})")
    plt.ylabel("Frequency (blocks)")
    plt.close()

    # Generate the image.
    imgs += mpld3.fig_to_html(fig)

    return imgs


def show_free_block_distribution(filename, data, max_gap_size=0):
    """
    data: dictionary generated by parse_dump_blocks() that contains information related to different
    checkpoints.
    max_gap_size: display free blocks up to the specified size in bytes
    """
    all_addr = []
    for checkpoint in data:

        # Concatenate the lists for each page type:
        for page_type in data[checkpoint]:
            all_addr += data[checkpoint][page_type]

        # Sort them by the addr_start which is the first element:
        all_addr.sort(key=itemgetter(0))

    # Now count all the gaps:
    buckets = {}
    for i, current_tuple in enumerate(all_addr):
        if i > 0:
            prev_tuple = all_addr[i - 1]
            prev_tuple_end = prev_tuple[0] + prev_tuple[1]
            gap = current_tuple[0] - prev_tuple_end
            if gap < 0:
                raise Exception("Data is not sorted correctly")
            if gap == 0:
                continue
            # If the size of the free block is large enough, we may not have interest in it.
            if max_gap_size and gap > max_gap_size:
                continue
            if gap in buckets:
                buckets[gap] += 1
            else:
                buckets[gap] = 1
        else:
            gap = current_tuple[0]
            # Nothing to do if the first block is written at offset 0.
            if gap > 0:
                buckets[gap] = 1

    # Sort buckets by size.
    buckets = dict(sorted(buckets.items()))

    # Plot settings.
    fig, ax = plt.subplots(1, figsize=(15, 10))
    labels = [str(x) for x in buckets.keys()]
    ax.bar(labels, list(buckets.values()))
    # The rotation does not work as expected.
    ax.set_xticklabels(labels, rotation=45)
    ax.set_xticks(range(len(buckets)))
    plt.xlabel("Bucket size (B)")
    plt.ylabel("Number of blocks")
    plt.title(f"Free blocks for {filename}")
    plt.close()

    # Generate the image.
    img = mpld3.fig_to_html(fig)

    return img


def histogram(field, chkpt, chkpt_name):
    """
    Rendering histogram in HTML for the specified field for leaf and internal pages 
    """
    internal = []
    leaf = []
    for metadata in chkpt.values():
        if field in metadata:
            if "internal" in metadata["page_type"]:
                internal.append(metadata[field])
            elif "leaf" in metadata["page_type"]:
                leaf.append(metadata[field])

    # Using pandas to calculate stats.
    df_internal = pd.DataFrame(internal, columns=[field])
    df_leaf = pd.DataFrame(leaf, columns=[field])

    columns = ['mean', '50%', 'min', 'max']
    stats_internal = df_internal.describe().loc[columns].transpose()
    stats_internal.columns = columns
    stats_leaf = df_leaf.describe().loc[columns].transpose()
    stats_leaf.columns = columns

    stats = pd.DataFrame({
        'Internal': stats_internal.iloc[0],
        'Leaf': stats_leaf.iloc[0]
    }).transpose()
    stats = stats.round(2)

    # plot the histograms
    fig, ax = plt.subplots(2, figsize=(15, 10))
    ax[0].hist(internal, bins=50, color=PLOT_COLORS[field]["internal"], label="internal")
    ax[1].hist(leaf, bins=50, color=PLOT_COLORS[field]["leaf"], label="leaf")
    ax[0].set_title(chkpt_name + " - Internal and leaf page " + FIELD_TITLES[field], fontsize=TITLE_SIZE)

    for subplot in ax:
        subplot.legend()
        if field == "entries":
            subplot.set_xlabel(field)
        else:
            subplot.set_xlabel(field + ' (bytes)')
        subplot.set_ylabel('Number of pages')

    plt.close()

    imgs = mpld3.fig_to_html(fig) 
    table_stats_html = f"<div style='padding-left: 150px;'> <h2>{field}</h2> {stats.to_html()}</div>"
    return imgs + table_stats_html


def pie_chart(field, chkpt, chkpt_name):
    """
    Rendering pie chart in HTML for the specified field for leaf and internal pages 
    """
    num_internal = 0
    num_leaf = 0
    for metadata in chkpt.values():
        if field in metadata:
            if "internal" in metadata["page_type"]:
                num_internal += 1
            elif "leaf" in metadata["page_type"]:
                num_leaf += 1
    labels = ["internal - " + str(num_internal), "leaf - " + str(num_leaf)]

    fig, ax = plt.subplots(figsize=(10, 10))
    plt.title(chkpt_name + " - " + FIELD_TITLES[field], fontsize=TITLE_SIZE)
    ax.pie([num_internal, num_leaf], labels=labels, autopct='%1.1f%%', colors=PLOT_COLORS[field])
    imgs = mpld3.fig_to_html(fig)
    plt.close()
    return imgs


def visualize_chkpt(tree_data, field):
    """
    Visualize a specified field for every existing checkpoint
    """
    imgs = ""
    for chkpt_name, chkpt in tree_data.items():
        if field in HISTOGRAM_CHOICES:
            imgs += histogram(field, chkpt, chkpt_name)
        elif field in PIE_CHART_CHOICES:
            imgs += pie_chart(field, chkpt, chkpt_name)
    return imgs


def visualize(tree_data, fields):
    """
    Visualize all specified fields for all checkpoints
    """
    imgs = ""
    for field in fields:
        imgs += visualize_chkpt(tree_data, field)
    serve(imgs)


def execute_command(command: str, output_file: str) -> None:
    """
    Execute a given command and writes its output in a file.
    """
    try:
        result = subprocess.run(command, shell=True, check=True,
                                stdout=subprocess.PIPE,
                                universal_newlines=True)
    except subprocess.CalledProcessError as e:
        print(f"Error executing command: {e}", file=sys.stderr)
        sys.exit(1)
    
    try:
        with open(output_file, 'w') as file:
            file.write(result.stdout)
        print(f"WT tool output written to {output_file}")
    except IOError as e:
        print(f"Failed to write output to file: {e}", file=sys.stderr)
        sys.exit(1)


def find_wt_exec_path():
    """
    Find the path of the WT tool executable. 
    
    We expect to find exactly one wt binary. Otherwise exit and prompt the user to provide an explicit path to the binary
    """
    wiredtiger_root_dir = f"{os.path.dirname(os.path.abspath(__file__))}/../../"

    try:
        result = subprocess.run(['find', wiredtiger_root_dir, '-maxdepth', '2', '-name', 'wt'],
                                stdout=subprocess.PIPE, text=True)
    except subprocess.CalledProcessError as e:
        print(f"Error executing command: {e}", file=sys.stderr)
        sys.exit(1)

    list_of_paths = result.stdout.split('\n')
    # Sanity check to remove empty new lines from find output
    result = [path for path in list_of_paths if path != '']

    if len(result) > 1:
        print("Error: multiple wt executables found. Please provide wt executable path using the -wt flag:")
        for path in result:
            print(path)
        exit(1)
    if len(result) == 0:
        print("Error: wt executable not found. Please provide path using the -wt flag.")
        exit(1)

    return result[0]


def construct_command(args) -> str:
    """
    Construct the WiredTiger verify command based on provided arguments.
    """
    if args.wt_exec_path:
        command = f"{args.wt_exec_path}"
    else:
        command = f"{find_wt_exec_path()}"

    command += f" -h {args.home_dir} verify -t"
    if args.dump:
        command += f" -d {args.dump}"
    if args.filename:
        command += f" \"{args.filename}\""
    return command


def main():
    parser = argparse.ArgumentParser(description="Script to run the WiredTiger verify command with specified options.")
    parser.add_argument('-d', '--dump', required=True, choices=['dump_blocks','dump_pages'], help='wt verify configuration options.')
    parser.add_argument('-f', '--filename', help='Name of the WiredTiger file to verify (such as file:foo.wt).')
    parser.add_argument('-hd', '--home_dir', default='.', help='Path to the WiredTiger database home directory (default is current directory).')
    parser.add_argument('-i', '--input_file', help='Input file (output of a wt verify command)')
    parser.add_argument('-o', '--output_file', help='Optionally save output to the provided output file.')
    parser.add_argument('-p', '--print_output', action='store_true', default=False, help='Print the output to stdout (default is off)')
    parser.add_argument('-v', '--visualize', choices=ALL_VISUALIZATION_CHOICES, nargs='*',
                        help='Type of visualization (multiple options allowed). If no options are provided, all available data is visualized.')
    parser.add_argument('-wt', '--wt_exec_path', help='Path of the WT tool executable.')

    args = parser.parse_args()

    # If the user has given an input file, don't generate anything.
    input_file = args.input_file
    if not input_file:
        if not args.filename:
            raise Exception("Argument -f/--filename is required")
        command = construct_command(args)
        try:
            execute_command(command, WT_OUTPUT_FILE)
        except (RuntimeError, ValueError, TypeError) as e:
            print(str(e), file=sys.stderr)
            sys.exit(1)
        # Use the generated output file as the input file for the next steps.
        input_file = WT_OUTPUT_FILE

    parsed_data = None

    if "dump_pages" in args.dump:
        parsed_data = parse_dump_pages(input_file)
    else:
        if not "dump_blocks" in args.dump:
            raise Exception(f"dump_blocks not found in {args.dump}")
        parsed_data = parse_dump_blocks(input_file)

    # If we don't have data, nothing to do.
    if not parsed_data:
        print("No data has been generated!")
        return

    if args.output_file or args.print_output:
        pretty_output = output_pretty(parsed_data)

    if args.output_file:
        try:
            with open(args.output_file, 'w') as file:
                file.write(pretty_output)
            print(f"Parsed output written to {args.output_file}")
        except IOError as e:
            print(f"Failed to write output to file: {e}", file=sys.stderr)
            sys.exit(1)

    if args.print_output:
        print(pretty_output)

    if "dump_blocks" in args.dump:
        imgs = show_block_distribution_broken_barh(args.filename, parsed_data)
        imgs += show_block_distribution_hist(args.filename, parsed_data)
        imgs += show_free_block_distribution(args.filename, parsed_data)
        serve(imgs)
    else:
        if not args.visualize:
            args.visualize = ALL_VISUALIZATION_CHOICES
        visualize(parsed_data, args.visualize)

if __name__ == "__main__":
    main()
