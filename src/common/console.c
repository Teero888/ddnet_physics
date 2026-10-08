#include "console.h"

#include <ctype.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>

/* str_skip_whitespaces / str_skip_to_whitespace */
static char *skip_whitespaces(char *str) {
  while (*str && (*str == ' ' || *str == '\t' || *str == '\n' || *str == '\r'))
    str++;
  return str;
}

static char *skip_to_whitespace(char *str) {
  while (*str && *str != ' ' && *str != '\t' && *str != '\n' && *str != '\r')
    str++;
  return str;
}

/* CConsole::ExecuteLineStroked */
void ddnet_console_execute_line(const char *line, ddnet_console_command_fn callback, void *user) {
  const char *str = line;
  while (str && *str) {
    const char *end = str;
    const char *next_part = NULL;
    bool in_string = false;
    bool is_escaping = false;

    while (*end) {
      if (is_escaping)
        is_escaping = false;
      else if (*end == '"')
        in_string = !in_string;
      else if (in_string && *end == '\\')
        is_escaping = true;

      if (!in_string) {
        if (*end == ';') {
          next_part = end + 1;
          break;
        } else if (*end == '#') {
          break;
        }
      }
      end++;
    }

    size_t length = (size_t)(end - str);
    char *command = malloc(length + 1);
    if (!command)
      return;
    memcpy(command, str, length);
    command[length] = '\0';

    /* CConsole::ParseStart */
    char *name = skip_whitespaces(command);
    char *args = skip_to_whitespace(name);
    if (*args) {
      *args = '\0';
      args++;
    }
    if (*name)
      callback(user, name, args);

    free(command);
    str = next_part;
  }
}

/* CConsole::ParseArgs */
int ddnet_console_parse_args(char *args, const char *format, char *argv[DDNET_CONSOLE_MAX_ARGS]) {
  char *str = args;
  bool optional = false;
  int argc = 0;

  for (; *format; format++) {
    char command = *format;
    if (command == ' ')
      continue;
    if (command == '?') {
      optional = true;
      continue;
    }

    str = skip_whitespaces(str);
    if (*str == '\0')
      return optional ? argc : -1;
    if (argc == DDNET_CONSOLE_MAX_ARGS)
      return -1;

    if (*str == '"') {
      str++;
      argv[argc++] = str;
      char *dst = str; /* escapes are resolved in place */
      while (str[0] != '"') {
        if (str[0] == '\\') {
          if (str[1] == '\\')
            str++;
          else if (str[1] == '"')
            str++;
        } else if (str[0] == '\0') {
          return -1;
        }
        *dst = *str;
        dst++;
        str++;
      }
      *dst = '\0';
      str++;
    } else {
      argv[argc++] = str;
      str = skip_to_whitespace(str);
      if (str[0] != '\0') {
        str[0] = '\0';
        str++;
      }
    }

    char *number_end;
    if (command == 'i') {
      long value = strtol(argv[argc - 1], &number_end, 10);
      if (*number_end != '\0' || (int)value == INT_MAX || (int)value == INT_MIN)
        return -1;
    } else if (command == 'f') {
      float value = strtod(argv[argc - 1], &number_end);
      (void)value;
      if (*number_end != '\0')
        return -1;
    }
  }
  return argc;
}
