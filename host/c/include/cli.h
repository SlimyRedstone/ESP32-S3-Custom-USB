/*
 * Console mode: everything the client does without opening a window.
 *
 * Sends one command and prints the reply, or streams events until interrupted.
 */

#ifndef CLI_H
#define CLI_H

/**
 * Install the interrupt handlers so a blocked transfer unwinds on ctrl-c.
 *
 * Also used by the graphical mode, which shares the same shutdown path.
 */
void cli_install_signals(void);

/**
 * Run the console client.
 *
 * @param argc As given to main; must be at least 2.
 * @param argv As given to main. argv[1] is either "listen" or a command for
 *             jsoncmd_build().
 * @return Process exit status.
 */
int cli_run(int argc, char **argv);

#endif /* CLI_H */
