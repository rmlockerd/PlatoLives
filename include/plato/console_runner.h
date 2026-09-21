#ifndef PLATO_CONSOLE_RUNNER_H
#define PLATO_CONSOLE_RUNNER_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef void (*plato_console_clipboard_cb)(const char *text, size_t len);
void plato_console_set_clipboard_callback(plato_console_clipboard_cb cb);

int plato_console_run(const char *host, int port);

#ifdef __cplusplus
}
#endif

#endif /* PLATO_CONSOLE_RUNNER_H */
