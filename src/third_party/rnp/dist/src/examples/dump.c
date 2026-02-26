/*
 * Copyright (c) 2019, [Ribose Inc](https://www.ribose.com).
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without modification,
 * are permitted provided that the following conditions are met:
 *
 * 1.  Redistributions of source code must retain the above copyright notice,
 *     this list of conditions and the following disclaimer.
 *
 * 2.  Redistributions in binary form must reproduce the above copyright notice,
 *     this list of conditions and the following disclaimer in the documentation
 *     and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
 * WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
 * DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
 * SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER
 * CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY,
 * OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF
 * THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#include <stdio.h>
#ifdef _MSC_VER
#include "uniwin.h"
#else
#include <unistd.h> /* getopt() */
#include <libgen.h> /* basename() */
#endif
#include <rnp/rnp.h>

#define PFX "dump: "

static void
print_usage(char *program_name)
{
    const char *program_basename;
#ifdef _MSC_VER
    char fname[_MAX_FNAME];
    program_basename = fname;
    _splitpath_s(program_name, NULL, 0, NULL, 0, fname, _MAX_FNAME, NULL, 0);
#else
    program_basename = basename(program_name);
#endif

    fprintf(stderr,
            "dump: " PFX "Program dumps PGP packets. \n\nUsage:\n"
            "\t%s [-d|-h] [input.pgp]\n"
            "\t  -d : indicates whether to print packet content. Data is represented as hex\n"
            "\t  -m : dump mpi values\n"
            "\t  -g : dump key fingerprints and grips\n"
            "\t  -j : JSON output\n"
            "\t  -h : prints help and exists\n",
            program_basename);
}

static bool
stdin_reader(void *app_ctx, void *buf, size_t len, size_t *readres)
{
    ssize_t res = read(STDIN_FILENO, buf, len);
    if (res < 0) {
        return false;
    }
    *readres = res;
    return true;
}

static bool
stdout_writer(void *app_ctx, const void *buf, size_t len)
{
    ssize_t wlen = write(STDOUT_FILENO, buf, len);
    return (wlen >= 0) && (size_t) wlen == len;
}

int
main(int argc, char *const argv[])
{
    char *   input_file = NULL;
    uint32_t flags = 0;
    uint32_t jflags = 0;
    bool     json = false;

    /* Parse command line options:
        -i input_file [mandatory]: specifies name of the file with PGP packets
        -d : indicates whether to dump whole packet content
        -m : dump mpi contents
        -g : dump key grips and fingerprints
        -j : JSON output
        -h : prints help and exists
    */
    int opt = 0;
    while ((opt = getopt(argc, argv, "dmgjh")) != -1) {
        switch (opt) {
        case 'd':
            flags |= RNP_DUMP_RAW;
            jflags |= RNP_JSON_DUMP_RAW;
            break;
        case 'm':
            flags |= RNP_DUMP_MPI;
            jflags |= RNP_JSON_DUMP_MPI;
            break;
        case 'g':
            flags |= RNP_DUMP_GRIP;
            jflags |= RNP_JSON_DUMP_GRIP;
            break;
        case 'j':
            json = true;
            break;
        default:
            print_usage(argv[0]);
            return 1;
        }
    }

    /*  Check whether we have input file */
    if (optind < argc) {
        input_file = argv[optind];
    }

    rnp_input_t  input = NULL;
    rnp_result_t ret = 0;
    if (input_file) {
        ret = rnp_input_from_path(&input, input_file);
    } else {
        ret = rnp_input_from_callback(&input, stdin_reader, NULL, NULL);
    }
    if (ret) {
        fprintf(stderr, "failed to open source: error 0x%x\n", (int) ret);
        return 1;
    }

    if (!json) {
        rnp_output_t output = NULL;
        ret = rnp_output_to_callback(&output, stdout_writer, NULL, NULL);
        if (ret) {
            fprintf(stderr, "failed to open stdout: error 0x%x\n", (int) ret);
            rnp_input_destroy(input);
            return 1;
        }
        ret = rnp_dump_packets_to_output(input, output, flags);
        rnp_output_destroy(output);
    } else {
        char *json = NULL;
        ret = rnp_dump_packets_to_json(input, jflags, &json);
        if (!ret) {
            fprintf(stdout, "%s\n", json);
        }
        rnp_buffer_destroy(json);
    }
    rnp_input_destroy(input);

    /* Inform in case of error occurred during parsing */
    if (ret) {
        fprintf(stderr, "Operation failed [error code: 0x%X]\n", (int) ret);
        return 1;
    }

    return 0;
}
