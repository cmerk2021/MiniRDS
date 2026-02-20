/*
 * getopt - POSIX-compatible command line option parsing for Windows
 *
 * This is a public domain implementation of getopt and getopt_long
 * for use on platforms that lack them (primarily Windows/MSVC).
 *
 * Based on the public domain implementation by Gregory Pietsch,
 * with modifications for getopt_long support.
 */

#ifndef GETOPT_H
#define GETOPT_H

#ifdef __cplusplus
extern "C" {
#endif

extern char *optarg;
extern int optind, opterr, optopt;

/* Argument requirement flags */
#define no_argument        0
#define required_argument  1
#define optional_argument  2

struct option {
    const char *name;
    int has_arg;
    int *flag;
    int val;
};

int getopt(int argc, char *const argv[], const char *optstring);
int getopt_long(int argc, char *const argv[], const char *optstring,
                const struct option *longopts, int *longindex);

#ifdef __cplusplus
}
#endif

#endif /* GETOPT_H */
