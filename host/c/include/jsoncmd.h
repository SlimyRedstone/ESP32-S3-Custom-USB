/*
 * Builds the JSON commands the device understands from argv.
 *
 * The shorthand forms exist because cmd.exe and PowerShell both mangle bare
 * JSON on the command line; raw JSON is still accepted for anything the
 * shorthand does not cover.
 */

#ifndef JSONCMD_H
#define JSONCMD_H

#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>

/**
 * Append @p in to @p out as the body of a JSON string, escaping what RFC 8259
 * requires. @p out must already be NUL terminated.
 *
 * Returns false without modifying @p out if the result would not fit.
 */
bool jsoncmd_escape_append(char *out, size_t out_size, const char *in);

/**
 * Turn the argument vector into one JSON command.
 *
 *   led ABCDEF        -> {"set":{"led":"ABCDEF"}}
 *   message a b c     -> {"set":{"message":"a b c"}}
 *   get led           -> {"get":"led"}
 *   {"...":...}       -> passed through verbatim
 *
 * @p argc and @p argv are as given to main, and argc must be at least 2.
 * Prints usage to stderr and returns false on a malformed invocation.
 */
bool jsoncmd_build(int argc, char **argv, char *out, size_t out_size);

/** Print the command-line syntax to @p f. */
void jsoncmd_usage(FILE *f);

#endif /* JSONCMD_H */
