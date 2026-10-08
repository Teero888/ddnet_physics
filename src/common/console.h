/* The small part of DDNet's console that is needed to run the server settings
 * embedded in a map. */
#ifndef DDNET_PHYSICS_COMMON_CONSOLE_H
#define DDNET_PHYSICS_COMMON_CONSOLE_H

#include <ddnet_physics/config.h>

#include <stdbool.h>

enum { DDNET_CONSOLE_MAX_ARGS = 4 };

/* Called once per command of a line. args is the writable rest of the command
 * after the name, to be parsed with ddnet_console_parse_args(). */
typedef void (*ddnet_console_command_fn)(void *user, const char *name, char *args);

/* Split a line into its commands (separated by ';', '#' starts a comment). */
void ddnet_console_execute_line(const char *line, ddnet_console_command_fn callback, void *user);

/* Parse arguments according to a format of 'i' (integer), 'f' (float), 's'
 * (string) and '?' (the following are optional), in place. Returns the number
 * of arguments, or -1 if a value is missing or not a valid number. */
int ddnet_console_parse_args(char *args, const char *format, char *argv[DDNET_CONSOLE_MAX_ARGS]);

/* Like ddnet_config_set(), but only for the settings a map is allowed to change. */
bool ddnet_config_set_from_map(ddnet_config_t *config, const char *name, int value);

#endif
