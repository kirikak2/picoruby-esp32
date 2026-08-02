/*
 * Midori serial console input
 *
 * Accepts console bytes from the serial link (USB CDC, UART or
 * USB-Serial/JTAG depending on the USB port mode), does the line editing
 * and echo in its own task, and hands completed commands to the Ruby side.
 *
 * See console_input.c for the data flow and the reason it is a separate
 * task rather than a poll from the Ruby VM task.
 */

#ifndef MIDORI_CONSOLE_INPUT_H
#define MIDORI_CONSOLE_INPUT_H

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Create the console task and, in USB-MIDI-Device mode, register the CDC
 * receive hook with picoruby-usb_midi_device. Call it after
 * USB_MIDI_DEVICE_start(); calling it twice is a no-op.
 */
void console_input_start(void);

/*
 * Pop one script path requested from the console ("load /sd/app.rb").
 * Non-blocking. Returns true and fills `out` when a command was queued,
 * false when the queue is empty.
 */
bool console_input_pop_command(char *out, size_t out_size);

#ifdef __cplusplus
}
#endif

#endif /* MIDORI_CONSOLE_INPUT_H */
