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
 * For the text *inside* a JSON string only. Passing punctuation such as a
 * closing quote through here escapes it, which corrupts the document.
 *
 * Returns false without modifying @p out if the result would not fit.
 */
bool jsoncmd_escape_append(char *out, size_t out_size, const char *in);

/**
 * Append @p raw to @p out verbatim, for the structural parts of a document:
 * quotes, braces and separators that must not be escaped.
 */
bool jsoncmd_append(char *out, size_t out_size, const char *raw);

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

/*
 * Minimal readers for the replies this device sends. They scan rather than
 * parse, which is enough for output we generate ourselves and avoids pulling a
 * JSON library into the client.
 *
 * Both return a pointer to a static buffer, valid until the next call, and NULL
 * if the key is absent or the value is not of the expected kind. Not reentrant.
 */

/** Value of a string member, unescaped only for the common escapes. */
const char *jsoncmd_find_string(const unsigned char *json, int len, const char *key);

/** Raw text of an object member, braces included. */
const char *jsoncmd_find_object(const unsigned char *json, int len, const char *key);

#endif /* JSONCMD_H */
