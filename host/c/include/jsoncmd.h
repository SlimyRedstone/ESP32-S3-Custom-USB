/*
 * Builds the JSON commands the device understands from argv.
 *
 * The shorthand forms exist because cmd.exe and PowerShell both mangle bare
 * JSON on the command line; raw JSON is still accepted for anything the
 * shorthand does not cover.
 *
 * Documents are assembled with cJSON, so escaping is its problem rather than
 * this file's.
 */

#ifndef JSONCMD_H
#define JSONCMD_H

#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>

/**
 * Turn the argument vector into one JSON command.
 *
 *   led ABCDEF        -> {"set":{"led":"ABCDEF"}}
 *   message a b c     -> {"set":{"message":"a b c"}}
 *   get led           -> {"get":"led"}
 *   {"...":...}       -> parsed, then re-emitted compactly
 *
 * @p argc and @p argv are as given to main, and argc must be at least 2.
 * Prints usage to stderr and returns false on a malformed invocation.
 */
bool jsoncmd_build(int argc, char **argv, char *out, size_t out_size);

void jsoncmd_usage(FILE *f);

#endif /* JSONCMD_H */
