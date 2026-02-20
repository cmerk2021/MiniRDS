/*
 * getopt - POSIX-compatible command line option parsing for Windows
 *
 * This is a public domain implementation of getopt and getopt_long
 * for use on platforms that lack them (primarily Windows/MSVC).
 */

#include <stdio.h>
#include <string.h>
#include "getopt.h"

char *optarg = NULL;
int optind = 1;
int opterr = 1;
int optopt = '?';

static int optwhere = 1;

static void permute(char *const argv[], int index1, int index2) {
    char *tmp = argv[index1];
    ((char **)argv)[index1] = argv[index2];
    ((char **)argv)[index2] = tmp;
}

int getopt(int argc, char *const argv[], const char *optstring) {
    char ch;
    const char *found;

    optarg = NULL;

    if (optind >= argc || argv[optind] == NULL) {
        return -1;
    }

    /* skip non-option arguments */
    if (argv[optind][0] != '-' || argv[optind][1] == '\0') {
        return -1;
    }

    /* handle "--" */
    if (strcmp(argv[optind], "--") == 0) {
        optind++;
        return -1;
    }

    ch = argv[optind][optwhere];

    /* find the option in optstring */
    found = strchr(optstring, ch);
    if (found == NULL || ch == ':') {
        optopt = ch;
        if (opterr && *optstring != ':') {
            fprintf(stderr, "%s: invalid option -- '%c'\n", argv[0], ch);
        }
        /* move to next character or next argument */
        if (argv[optind][optwhere + 1] == '\0') {
            optind++;
            optwhere = 1;
        } else {
            optwhere++;
        }
        return '?';
    }

    /* check if option requires an argument */
    if (found[1] == ':') {
        /* argument required */
        if (argv[optind][optwhere + 1] != '\0') {
            /* argument is in the same argv element */
            optarg = &argv[optind][optwhere + 1];
            optind++;
            optwhere = 1;
        } else if (found[2] == ':') {
            /* optional argument not present */
            optarg = NULL;
            optind++;
            optwhere = 1;
        } else {
            /* argument is next argv element */
            optind++;
            optwhere = 1;
            if (optind >= argc) {
                optopt = ch;
                if (opterr && *optstring != ':') {
                    fprintf(stderr, "%s: option requires an argument -- '%c'\n",
                            argv[0], ch);
                }
                return (*optstring == ':') ? ':' : '?';
            }
            optarg = argv[optind];
            optind++;
        }
    } else {
        /* no argument */
        if (argv[optind][optwhere + 1] == '\0') {
            optind++;
            optwhere = 1;
        } else {
            optwhere++;
        }
    }

    return ch;
}

int getopt_long(int argc, char *const argv[], const char *optstring,
                const struct option *longopts, int *longindex) {
    int i;
    size_t len;

    optarg = NULL;

    if (optind >= argc || argv[optind] == NULL) {
        return -1;
    }

    /* check for long option */
    if (argv[optind][0] == '-' && argv[optind][1] == '-') {
        const char *arg = &argv[optind][2];
        const char *eq;

        if (*arg == '\0') {
            /* bare "--" */
            optind++;
            return -1;
        }

        /* check for '=' in the argument */
        eq = strchr(arg, '=');
        len = eq ? (size_t)(eq - arg) : strlen(arg);

        for (i = 0; longopts[i].name != NULL; i++) {
            if (strncmp(arg, longopts[i].name, len) == 0 &&
                strlen(longopts[i].name) == len) {
                /* found matching long option */
                if (longindex) *longindex = i;

                optind++;

                if (longopts[i].has_arg == required_argument ||
                    longopts[i].has_arg == optional_argument) {
                    if (eq) {
                        optarg = (char *)(eq + 1);
                    } else if (longopts[i].has_arg == required_argument) {
                        if (optind >= argc) {
                            if (opterr) {
                                fprintf(stderr,
                                    "%s: option '--%s' requires an argument\n",
                                    argv[0], longopts[i].name);
                            }
                            return '?';
                        }
                        optarg = argv[optind++];
                    }
                }

                if (longopts[i].flag != NULL) {
                    *longopts[i].flag = longopts[i].val;
                    return 0;
                }
                return longopts[i].val;
            }
        }

        /* no matching long option found */
        if (opterr) {
            fprintf(stderr, "%s: unrecognized option '--%.*s'\n",
                    argv[0], (int)len, arg);
        }
        optind++;
        return '?';
    }

    /* fall back to short option processing */
    return getopt(argc, argv, optstring);
}
