import json
import sys
import datetime
import argparse
import logging
from tabulate import tabulate

parser = argparse.ArgumentParser(description='Print top n metrics from build metrics json files.')
parser.add_argument('--input', metavar='FILE', type=str, default='build-metrics.json',
                    help='Path to build metrics input json.')
parser.add_argument('--output', metavar='FILE', type=str, default="top_n_metrics.txt",
                    help='Path to output text file.')
parser.add_argument('--num', metavar='N', type=int, default=10,
                    help='Positive integer which represent the top N metrics to report on.')
args = parser.parse_args()

logger = logging.getLogger()
logger.setLevel(logging.INFO)
logger.addHandler(logging.FileHandler(args.output))
log_format = logging.Formatter("%(message)s")
for handler in logger.handlers:
    handler.setFormatter(log_format)

with open(args.input) as f:
    metrics = json.load(f)

    logger.info(f"Time of report: {datetime.datetime.now()}")
    logger.info(f"Task ID: {metrics['evg_id']}")
    logger.info(f"Distro: {metrics['variant']}")
    logger.info(
        f"Peak Memory Used:\n{round(metrics['system_memory']['max'] / 1024.0 / 1024.0, 2)} MBs")
    logger.info(f"SCons Command:\n{metrics['scons_command']}")

    build_tasks_sort = metrics['build_tasks'].copy()
    build_tasks_sort.sort(reverse=True, key=lambda x: x['mem_usage'])
    logger.info(f"\nTop {args.num} Memory tasks:")
    table_data = []
    for i, val in enumerate(build_tasks_sort[:args.num], start=1):
        table_data.append([i, val['mem_usage'] / 1024.0 / 1024.0, val['outputs'][0]])
    logger.info(tabulate(table_data, headers=['Num', 'MBs', 'Output'], floatfmt=".2f"))

    build_tasks_sort = metrics['build_tasks'].copy()
    build_tasks_sort.sort(reverse=True, key=lambda x: x['end_time'] - x['start_time'])
    logger.info(f"\nTop {args.num} duration tasks:")
    table_data = []
    for i, val in enumerate(build_tasks_sort[:args.num], start=1):
        table_data.append([i, (val['end_time'] - val['start_time']) / 10.0**9, val['outputs'][0]])
    logger.info(tabulate(table_data, headers=['Num', 'Secs', 'Output'], floatfmt=".2f"))

    build_tasks_sort = metrics['artifact_metrics']['artifacts'].copy()
    build_tasks_sort.sort(reverse=True, key=lambda x: x['size'])
    logger.info(f"\nTop {args.num} sized artifacts:")
    table_data = []
    for i, val in enumerate(build_tasks_sort[:args.num], start=1):
        table_data.append([i, val['size'] / 1024.0 / 1024.0, val['name']])
    logger.info(tabulate(table_data, headers=['Num', 'MBs', 'Output'], floatfmt=".2f"))

    build_tasks_sort = [
        metric for metric in metrics['artifact_metrics']['artifacts']
        if metric.get('bin_metrics') and metric['bin_metrics'].get('text')
    ]
    build_tasks_sort.sort(reverse=True, key=lambda x: x['bin_metrics']['text']['vmsize'])
    logger.info(f"\nTop {args.num} Text sections:")
    table_data = []
    for i, val in enumerate(build_tasks_sort[:args.num], start=1):
        table_data.append([i, val['bin_metrics']['text']['vmsize'] / 1024.0 / 1024.0, val['name']])
    logger.info(tabulate(table_data, headers=['Num', 'MBs', 'Output'], floatfmt=".2f"))

    build_tasks_sort = [
        metric for metric in metrics['artifact_metrics']['artifacts']
        if metric.get('bin_metrics') and metric['bin_metrics'].get('debug')
    ]
    build_tasks_sort.sort(reverse=True, key=lambda x: x['bin_metrics']['debug']['filesize'])
    logger.info(f"\nTop {args.num} Debug sections:")
    table_data = []
    for i, val in enumerate(build_tasks_sort[:args.num], start=1):
        table_data.append(
            [i, val['bin_metrics']['debug']['filesize'] / 1024.0 / 1024.0, val['name']])
    logger.info(tabulate(table_data, headers=['Num', 'MBs', 'Output'], floatfmt=".2f"))
