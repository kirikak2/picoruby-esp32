#ifndef PICORUBY_ESP32_H
#define PICORUBY_ESP32_H

#include <stddef.h>
#include <stdbool.h>

/**
 * @brief Run the PicoRuby R2P2 shell (blocking)
 */
void picoruby_esp32(void);

/**
 * @brief Run a Ruby script from a string buffer
 *
 * @param script The Ruby source code to execute
 * @param script_len Length of the script (0 for null-terminated string)
 * @param filename Filename for error reporting (can be NULL)
 * @return true if script executed successfully, false on error
 */
bool picoruby_esp32_run_script(const char *script, size_t script_len, const char *filename);

/**
 * @brief Initialize PicoRuby VM without running a script
 *
 * @return true if initialization successful
 */
bool picoruby_esp32_init(void);

/**
 * @brief Check if PicoRuby VM is initialized
 *
 * @return true if initialized
 */
bool picoruby_esp32_is_initialized(void);

#endif // PICORUBY_ESP32_H
