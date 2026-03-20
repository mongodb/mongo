/*
 * Copyright (c) 2017-2023, [Ribose Inc](https://www.ribose.com).
 * Copyright (c) 2009 The NetBSD Foundation, Inc.
 * All rights reserved.
 *
 * This code is originally derived from software contributed to
 * The NetBSD Foundation by Alistair Crooks (agc@netbsd.org), and
 * carried further by Ribose Inc (https://www.ribose.com).
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * ``AS IS'' AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED
 * TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
 * PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDERS OR CONTRIBUTORS
 * BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 */
/* Command line program to perform rnp operations */
#ifdef _MSC_VER
#include "uniwin.h"
#else
#include <getopt.h>
#endif
#include <stdio.h>
#include <string.h>
#include "rnpkeys.h"

extern struct option options[];
extern const char *  usage;

static optdefs_t
get_short_cmd(int ch)
{
    switch (ch) {
    case 'V':
        return CMD_VERSION;
    case 'g':
        return CMD_GENERATE_KEY;
    case 'l':
        return CMD_LIST_KEYS;
    case 'h':
        FALLTHROUGH_STATEMENT;
    default:
        return CMD_HELP;
    }
}

#ifndef RNP_RUN_TESTS
int
main(int argc, char **argv)
#else
int rnpkeys_main(int argc, char **argv);
int
rnpkeys_main(int argc, char **argv)
#endif
{
    if (argc < 2) {
        print_usage(usage);
        return EXIT_FAILURE;
    }

    cli_rnp_t rnp = {};
#if !defined(RNP_RUN_TESTS) && defined(_WIN32)
    try {
        rnp.substitute_args(&argc, &argv);
    } catch (std::exception &ex) {
        RNP_LOG("Error converting arguments ('%s')", ex.what());
        return EXIT_FAILURE;
    }
#endif

    rnp_cfg   cfg;
    optdefs_t cmd = CMD_NONE;
    int       optindex = 0;
    int       ch;

    while ((ch = getopt_long(argc, argv, "Vglh", options, &optindex)) != -1) {
        /* Check for unsupported command/option */
        if (ch == '?') {
            print_usage(usage);
            return EXIT_FAILURE;
        }

        optdefs_t newcmd = cmd;
        if (ch >= CMD_LIST_KEYS) {
            if (!setoption(cfg, &newcmd, options[optindex].val, optarg)) {
                ERR_MSG("Failed to process argument --%s", options[optindex].name);
                return EXIT_FAILURE;
            }
        } else {
            newcmd = get_short_cmd(ch);
        }

        if (cmd && newcmd != cmd) {
            ERR_MSG("Conflicting commands!");
            return EXIT_FAILURE;
        }
        cmd = newcmd;
    }

    /* No initialization required for these two commands. */
    if (cmd == CMD_HELP || cmd == CMD_VERSION) {
        return cli_rnp_t::ret_code(rnp_cmd(&rnp, cmd, NULL));
    }

    if (!rnpkeys_init(rnp, cfg)) {
        return EXIT_FAILURE;
    }

    if (!cli_rnp_setup(&rnp)) {
        return EXIT_FAILURE;
    }

    /* now do the required action for each of the command line args */
    if (optind == argc) {
        return cli_rnp_t::ret_code(rnp_cmd(&rnp, cmd, NULL));
    }
    bool success = true;
    for (int i = optind; i < argc; i++) {
        success = success && rnp_cmd(&rnp, cmd, argv[i]);
    }
    return cli_rnp_t::ret_code(success);
}
