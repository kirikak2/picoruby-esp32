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

/**
 * @brief Request to switch to a new script
 *
 * @param script_path Full path to the script file (e.g., "/sdcard/app.rb")
 * @return true if request was accepted, false otherwise
 */
bool picoruby_esp32_request_script_change(const char *script_path);

/**
 * @brief Get the currently running script filename
 *
 * @return Pointer to current script filename (or NULL if none)
 */
const char* picoruby_esp32_get_current_script(void);

/**
 * @brief Request current script to stop
 */
void picoruby_esp32_request_stop(void);

/**
 * @brief Check if stop was requested
 *
 * @return true if stop requested
 */
bool picoruby_esp32_stop_requested(void);

/**
 * @brief Clear stop request flag
 *
 * Called after script ends to prepare for next script
 */
void picoruby_esp32_clear_stop_flag(void);

/**
 * @brief Perform MIDI cleanup (send All Notes Off, All Sound Off, MIDI Stop to all devices)
 *
 * Called automatically on script change to ensure clean state
 */
void picoruby_esp32_midi_cleanup(void);

/**
 * @brief Script list management (called from Ruby to notify C about available scripts)
 */
#define PICORUBY_MAX_SCRIPTS 20
#define PICORUBY_MAX_SCRIPT_NAME 32

/**
 * @brief Clear the script list
 */
void picoruby_esp32_clear_script_list(void);

/**
 * @brief Add a script to the list
 *
 * @param filename Script filename (without path, e.g., "app.rb")
 * @return true if added successfully, false if list is full
 */
bool picoruby_esp32_add_script(const char *filename);

/**
 * @brief Get number of scripts in the list
 *
 * @return Number of scripts
 */
int picoruby_esp32_get_script_count(void);

/**
 * @brief Get script name by index
 *
 * @param index Script index (0 to count-1)
 * @return Pointer to script filename, or NULL if index out of range
 */
const char* picoruby_esp32_get_script_name(int index);

/**
 * @brief Check if script list is available (SD card mounted and scripts enumerated)
 *
 * @return true if script list is available
 */
bool picoruby_esp32_script_list_ready(void);

/**
 * @brief Mark script list as ready
 */
void picoruby_esp32_set_script_list_ready(bool ready);

/**
 * @brief Request SD card re-initialization from UI
 *
 * Called when user presses Refresh button and SD card was not available at boot
 */
void picoruby_esp32_request_sd_refresh(void);

/**
 * @brief Get script list version counter
 *
 * Incremented each time the script list is updated (set_ready called with true).
 * UI can poll this to detect when the list has been refreshed.
 */
uint32_t picoruby_esp32_get_script_list_version(void);

#endif // PICORUBY_ESP32_H
