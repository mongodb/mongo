import argparse
import json


# This function reads a gcovr json summary file into a dict and returns it
def read_coverage_data(coverage_data_path: str):
    with open(coverage_data_path) as json_file:
        data = json.load(json_file)
        return data


# The timing data file contains two lines of text, each line containing a single integer.
# Line one contains the start time in seconds, line two contains the end time in seconds.
# This function reads the timing data and returns the difference, in seconds, between the start and end times.
def read_timing_data(timing_data_path: str):
    with open(timing_data_path) as file:
        line1 = file.readline()
        line2 = file.readline()
        start_time_secs = int(line1)
        end_time_secs = int(line2)
        delta_secs = end_time_secs - start_time_secs
        print("Timing data: {} to {}, delta {}".format(start_time_secs, end_time_secs, delta_secs))
        return delta_secs


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('-c', '--coverage', required=True, help='Path to the gcovr json summary data file')
    parser.add_argument('-t', '--time', required=True, help='Path to the timing data file')
    parser.add_argument('-v', '--verbose', action="store_true", help='be verbose')
    args = parser.parse_args()

    if args.verbose:
        print('Code Coverage Analysis')
        print('======================')
        print('Configuration:')
        print('  Coverage data:  {}'.format(args.coverage))
        print('  Timing data:    {}'.format(args.time))

    coverage_data = read_coverage_data(args.coverage)
    branch_coverage = coverage_data['branch_percent']
    print("Branch coverage = {}%".format(branch_coverage))

    delta_secs = read_timing_data(args.time)
    delta_mins = delta_secs / 60.0
    code_coverage_per_min = branch_coverage / delta_mins
    print("Time taken: {} seconds".format(delta_secs))

    print("Code coverage rate = {:.2f}%/min".format(code_coverage_per_min))


if __name__ == '__main__':
    main()
