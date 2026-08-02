/*
 * Midori serial console input
 *
 * Owns everything between "bytes arrive on the console link" and "the Ruby
 * side is handed a complete command":
 *
 *   USB-MIDI-Device mode (CDC on the USB-C connector)
 *     TinyUSB task --console_cdc_rx_cb()--> RX stream buffer --+
 *                                                              |
 *   host / serial mode (UART or USB-Serial/JTAG)               |
 *     console task polls getchar() -----------------------------+
 *                                                              v
 *                                    console task: echo + line editing
 *                                                              |
 *                                              command queue <-+
 *                                                              |
 *                       ScriptManager#check_console (Ruby VM) <-+
 *
 * Why the CDC hook is mandatory in CDC mode:
 *   picoruby-usb_midi_device registers its own .callback_rx with
 *   esp_tinyusb, and that handler drains the CDC FIFO with
 *   tinyusb_cdcacm_read() on every RX event. With no application callback
 *   installed the bytes are read out and dropped on the floor, so a
 *   getchar() on the CDC VFS finds tud_cdc_n_available() == 0 and most
 *   keystrokes never reach the console at all.
 *
 * Why a dedicated task (same shape as picoruby-midi's midi_input task):
 *   - The CDC hook runs in the TinyUSB task and must return immediately,
 *     so it only copies bytes into a stream buffer.
 *   - Line editing, echo and command parsing then run at priority 4,
 *     decoupled from the Ruby VM task (priority 3), which sleeps 100 ms
 *     between polls. Input is buffered and processed while the VM sleeps,
 *     while it is busy, and while a script is running.
 *
 * Core affinity: the task echoes with printf(), which in CDC mode ends up
 * in tud_cdc_n_write_flush(). Like the gem's MIDI TX task it therefore
 * stays on the TinyUSB core (Core 1) so it never runs in true parallel
 * with tud_task.
 */

#include <stdio.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/stream_buffer.h"
#include "esp_log.h"
#include "esp_system.h"
#include "sdkconfig.h"

#include "console_input.h"

#if CONFIG_USB_MIDI_USB_MODE_MIDI_DEVICE
#include "usb_midi_device.h"
#endif

static const char *TAG = "CONSOLE";

#define CONSOLE_TASK_STACK_SIZE  4096
#define CONSOLE_TASK_PRIORITY    4    /* above the Ruby VM task (3),
                                       * below the TinyUSB task (5) */
#define CONSOLE_TASK_CORE        1    /* same core as tud_task */

#define CONSOLE_RX_STREAM_SIZE   1024 /* raw bytes buffered from the link */
#define CONSOLE_LINE_MAX         256  /* longest editable line */
#define CONSOLE_CMD_MAX          128  /* matches the supervisor's path buffer */
#define CONSOLE_CMD_QUEUE_DEPTH  4    /* commands waiting for the Ruby side */
#define CONSOLE_POLL_INTERVAL_MS 10   /* stdin polling fallback */

static StreamBufferHandle_t s_rx_stream  = NULL;
static QueueHandle_t        s_cmd_queue  = NULL;
static TaskHandle_t         s_task       = NULL;
static bool                 s_cdc_mode   = false;
/* Written by the TinyUSB task, cleared by the console task: a lost update
 * only costs a warning line. */
static volatile uint32_t    s_rx_dropped = 0;

/* Line editor state. Kept on the console task's stack rather than in .bss
 * (see docs/MEMORY_ALLOCATION.md on static buffers and DRAM layout). */
typedef struct {
    char    buf[CONSOLE_LINE_MAX];
    size_t  len;
    bool    last_was_cr;
    uint8_t esc;   /* 0: normal, 1: after ESC, 2: inside a CSI sequence */
} console_line_t;

/*--------------------------------------------------------------------+
 * Output helpers
 *--------------------------------------------------------------------*/

static void console_prompt(void)
{
    printf("> ");
    fflush(stdout);
}

static void console_help(void)
{
    printf("Commands:\n");
    printf("  load /sd/app.rb  - Load and run a script\n");
    printf("  heap             - Show free heap memory\n");
    printf("  restart          - Restart ESP32\n");
    fflush(stdout);
}

/*--------------------------------------------------------------------+
 * Command handling
 *--------------------------------------------------------------------*/

static void console_queue_script(const char *path)
{
    char cmd[CONSOLE_CMD_MAX];
    snprintf(cmd, sizeof(cmd), "%s", path);

    if (xQueueSend(s_cmd_queue, cmd, 0) != pdTRUE) {
        printf("Command queue full, dropped: %s\n", cmd);
        fflush(stdout);
        ESP_LOGW(TAG, "command queue full, dropped: %s", cmd);
    }
}

/* Runs in the console task. Anything that needs the PicoRuby VFS (i.e.
 * actually loading a script) is queued for the Ruby side instead. */
static void console_execute(const char *line)
{
    if (line[0] == '\0') {
        return;                     /* bare Enter */
    }

    if (strncmp(line, "load ", 5) == 0) {
        const char *path = line + 5;
        while (*path == ' ') path++;
        if (*path == '\0') {
            printf("Usage: load /sd/app.rb\n");
            fflush(stdout);
            return;
        }
        if (strlen(path) >= CONSOLE_CMD_MAX) {
            printf("Path too long (max %d)\n", CONSOLE_CMD_MAX - 1);
            fflush(stdout);
            return;
        }
        ESP_LOGI(TAG, "Console: load %s", path);
        console_queue_script(path);
        return;
    }

    if (strcmp(line, "heap") == 0) {
        printf("Free heap: %u bytes\n", (unsigned)esp_get_free_heap_size());
        fflush(stdout);
        return;
    }

    if (strcmp(line, "restart") == 0) {
        printf("Restarting ESP32...\n");
        fflush(stdout);
        vTaskDelay(pdMS_TO_TICKS(100));
        esp_restart();
        return;
    }

    if (strcmp(line, "help") == 0 || strcmp(line, "?") == 0) {
        console_help();
        return;
    }

    printf("Unknown command: %s\n", line);
    console_help();
}

/*--------------------------------------------------------------------+
 * Line editor
 *--------------------------------------------------------------------*/

static void console_feed(console_line_t *st, uint8_t c)
{
    /* Swallow ANSI escape sequences (arrow keys, function keys) so they
     * do not end up as "[A" in the command line. */
    if (st->esc == 1) {
        st->esc = (c == '[' || c == 'O') ? 2 : 0;
        return;
    }
    if (st->esc == 2) {
        if (c >= 0x40 && c <= 0x7E) st->esc = 0;   /* final byte of CSI */
        return;
    }
    if (c == 0x1B) {
        st->esc = 1;
        return;
    }

    bool was_cr = st->last_was_cr;
    st->last_was_cr = (c == '\r');

    if (c == '\n' && was_cr) {
        return;                     /* second half of a CRLF */
    }

    if (c == '\r' || c == '\n') {
        printf("\n");
        fflush(stdout);
        st->buf[st->len] = '\0';
        console_execute(st->buf);
        st->len = 0;
        console_prompt();
        return;
    }

    if (c == 0x08 || c == 0x7F) {   /* BS / DEL */
        if (st->len > 0) {
            st->len--;
            printf("\b \b");
            fflush(stdout);
        }
        return;
    }

    if (c == 0x03 || c == 0x15) {   /* Ctrl-C / Ctrl-U: discard the line */
        st->len = 0;
        printf("\n");
        console_prompt();
        return;
    }

    if (c >= 0x20 && c < 0x7F) {    /* printable */
        if (st->len < CONSOLE_LINE_MAX - 1) {
            st->buf[st->len++] = (char)c;
            putchar((int)c);
            fflush(stdout);
        }
        /* line full: ignore silently, Enter still submits what was typed */
    }
}

/*--------------------------------------------------------------------+
 * CDC receive hook (TinyUSB task context)
 *--------------------------------------------------------------------*/

#if CONFIG_USB_MIDI_USB_MODE_MIDI_DEVICE
static void console_cdc_rx_cb(const uint8_t *data, size_t len, void *arg)
{
    (void)arg;
    if (s_rx_stream == NULL || len == 0) return;

    /* Copy and return: this runs on the task that services USB, so it must
     * never block. Overflow is counted and reported by the console task. */
    size_t sent = xStreamBufferSend(s_rx_stream, data, len, 0);
    if (sent < len) {
        s_rx_dropped += (uint32_t)(len - sent);
    }
}
#endif

/*--------------------------------------------------------------------+
 * Console task
 *--------------------------------------------------------------------*/

static void console_input_task(void *arg)
{
    (void)arg;
    console_line_t st = { .len = 0, .last_was_cr = false, .esc = 0 };
    uint8_t buf[64];

    ESP_LOGI(TAG, "Console input task running (%s)",
             s_cdc_mode ? "CDC callback" : "stdin polling");

    for (;;) {
        if (s_cdc_mode) {
            size_t n = xStreamBufferReceive(s_rx_stream, buf, sizeof(buf),
                                            portMAX_DELAY);
            for (size_t i = 0; i < n; i++) {
                console_feed(&st, buf[i]);
            }
        } else {
            /* UART / USB-Serial-JTAG: no RX callback available, so drain
             * stdin here. Being a task of its own, a short interval costs
             * nothing and keeps typing responsive. */
            int c;
            int guard = 0;
            while (guard++ < (int)sizeof(buf) * 8) {
                c = getchar();
                if (c == EOF || c < 0) break;
                console_feed(&st, (uint8_t)c);
            }
            vTaskDelay(pdMS_TO_TICKS(CONSOLE_POLL_INTERVAL_MS));
        }

        uint32_t dropped = s_rx_dropped;
        if (dropped) {
            s_rx_dropped = 0;
            ESP_LOGW(TAG, "console RX overflow: %u bytes dropped",
                     (unsigned)dropped);
        }
    }
}

/*--------------------------------------------------------------------+
 * Public API
 *--------------------------------------------------------------------*/

void console_input_start(void)
{
    if (s_task != NULL) {
        return;
    }

    s_cmd_queue = xQueueCreate(CONSOLE_CMD_QUEUE_DEPTH, CONSOLE_CMD_MAX);
    if (s_cmd_queue == NULL) {
        ESP_LOGE(TAG, "Failed to create command queue");
        return;
    }

#if CONFIG_USB_MIDI_USB_MODE_MIDI_DEVICE
    s_rx_stream = xStreamBufferCreate(CONSOLE_RX_STREAM_SIZE, 1);
    if (s_rx_stream == NULL) {
        ESP_LOGE(TAG, "Failed to create RX stream buffer");
        vQueueDelete(s_cmd_queue);
        s_cmd_queue = NULL;
        return;
    }
    /* Must be registered: the gem's CDC handler drains the FIFO either
     * way, so without this hook every keystroke is discarded. */
    if (USB_MIDI_DEVICE_set_cdc_rx_callback(console_cdc_rx_cb, NULL) == 0) {
        s_cdc_mode = true;
    } else {
        ESP_LOGW(TAG, "CDC RX hook unavailable, falling back to stdin polling");
        vStreamBufferDelete(s_rx_stream);
        s_rx_stream = NULL;
    }
#endif

    BaseType_t ret = xTaskCreatePinnedToCore(
        console_input_task,
        "console_in",
        CONSOLE_TASK_STACK_SIZE,
        NULL,
        CONSOLE_TASK_PRIORITY,
        &s_task,
        CONSOLE_TASK_CORE);

    if (ret != pdPASS) {
        ESP_LOGE(TAG, "Failed to create console task");
        s_task = NULL;
        return;
    }

    ESP_LOGI(TAG, "Console input started (core %d, prio %d, source: %s)",
             CONSOLE_TASK_CORE, CONSOLE_TASK_PRIORITY,
             s_cdc_mode ? "USB CDC callback" : "stdin");
}

bool console_input_pop_command(char *out, size_t out_size)
{
    if (s_cmd_queue == NULL || out == NULL || out_size == 0) {
        return false;
    }

    char cmd[CONSOLE_CMD_MAX];
    if (xQueueReceive(s_cmd_queue, cmd, 0) != pdTRUE) {
        return false;
    }

    cmd[CONSOLE_CMD_MAX - 1] = '\0';
    snprintf(out, out_size, "%s", cmd);
    return true;
}
