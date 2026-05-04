/**
 * @file picoruby_supervisor.c
 * @brief PicoRuby task supervisor for dynamic script switching
 */

#include <string.h>
#include <stdarg.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/event_groups.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "esp_heap_caps.h"

#include "picoruby_supervisor.h"
#include "picoruby-esp32.h"
#include "midi.h"  /* for MIDI_set_stop_check / MIDI_set_cleanup_hook */

// For mrubyc
#if defined(PICORB_VM_MRUBYC)
#include <mrubyc.h>

// Forward declaration for require system
typedef struct picogems {
    const char *name;
    const uint8_t *mrb;
    void (*initializer)(mrbc_vm *vm);
    bool required;
} picogems;
extern picogems prebuilt_gems[];

// Forward declaration for sandbox cleanup
extern void mrbc_sandbox_cleanup(void);
#endif

static const char *TAG = "SUPERVISOR";

// Platform-specific includes
#if defined(CONFIG_USB_MIDI_BOARD_M5STACK_CORES3) || defined(CONFIG_USB_MIDI_BOARD_M5STACK_CORES3_USB_SERIAL) || defined(CONFIG_USB_MIDI_BOARD_M5STACK_TAB5)
#include "ui_common.h"
#endif

// Supervisor command types (internal)
typedef enum {
    CMD_LOAD_SCRIPT,
    CMD_STOP_SCRIPT,
    CMD_ENTER_UI_MODE,
} supervisor_cmd_type_t;

typedef struct {
    supervisor_cmd_type_t cmd;
    char script_path[128];
} supervisor_cmd_t;

// Event bits
#define EVT_TASK_STARTED     (1 << 0)
#define EVT_TASK_COMPLETED   (1 << 1)
#define EVT_TASK_ERROR       (1 << 2)
#define EVT_STOP_REQUESTED   (1 << 3)

// Supervisor state
static supervisor_state_t s_state = SUPERVISOR_STATE_IDLE;
static char s_current_script[128] = {0};
static char s_requested_script[128] = {0};  // Script to run (passed to main_task.rb)
static supervisor_script_result_t s_last_result = {0};
static volatile bool s_has_result = false;

// FreeRTOS handles
static TaskHandle_t s_supervisor_task = NULL;
static TaskHandle_t s_picoruby_task = NULL;
static QueueHandle_t s_cmd_queue = NULL;
static EventGroupHandle_t s_events = NULL;
static SemaphoreHandle_t s_state_mutex = NULL;

// Script request from Ruby side
static char s_ruby_requested_script[128] = {0};
static volatile bool s_ruby_script_requested = false;

// Forward declarations
static void supervisor_task(void *arg);
static void picoruby_runner_task(void *arg);
static bool run_vm_with_main_task(void);
static void register_script_manager_class(mrbc_vm *vm);

// External declarations from picoruby-esp32.c
extern void initialize_nvs(void);
extern void picoruby_esp32_midi_cleanup(void);

// Heap size configuration (must match picoruby-esp32.c)
#if defined(CONFIG_IDF_TARGET_ESP32P4) && defined(CONFIG_SPIRAM)
#define HEAP_SIZE (1024 * 1024 * 4)
#elif defined(CONFIG_IDF_TARGET_ESP32S3) && defined(CONFIG_SPIRAM)
#define HEAP_SIZE (1024 * 1024 * 2)
#elif defined(CONFIG_IDF_TARGET_ESP32P4)
#define HEAP_SIZE (1024 * 256)
#else
#define HEAP_SIZE (1024 * 180)
#endif

// Heap pool (declared in picoruby-esp32.c)
extern uint8_t heap_pool[];

// main_task bytecode (defined in mrb/main_task.c, included by picoruby-esp32.c)
extern const uint8_t main_task[];

void supervisor_init(void)
{
    ESP_LOGI(TAG, "Initializing supervisor...");

    // Create synchronization primitives
    s_cmd_queue = xQueueCreate(4, sizeof(supervisor_cmd_t));
    s_events = xEventGroupCreate();
    s_state_mutex = xSemaphoreCreateMutex();

    if (!s_cmd_queue || !s_events || !s_state_mutex) {
        ESP_LOGE(TAG, "Failed to create FreeRTOS primitives");
        return;
    }

    // Wire picoruby-midi's cooperative-stop hooks to the supervisor's
    // script-stop flag and to the all-notes-off cleanup helper, so the
    // gem itself stays free of picoruby-esp32 symbols.
    MIDI_set_stop_check(picoruby_esp32_stop_requested);
    MIDI_set_cleanup_hook(picoruby_esp32_midi_cleanup);

    // Initialize NVS (needed for some operations)
    initialize_nvs();

    // Create supervisor task on Core 1
    BaseType_t ret = xTaskCreatePinnedToCore(
        supervisor_task,
        "supervisor",
        4096,
        NULL,
        4,  // Higher priority than picoruby task
        &s_supervisor_task,
        1   // Core 1
    );

    if (ret != pdPASS) {
        ESP_LOGE(TAG, "Failed to create supervisor task");
        return;
    }

    ESP_LOGI(TAG, "Supervisor initialized");
}

bool supervisor_request_script(const char *script_path)
{
    if (!s_cmd_queue) {
        return false;
    }

    supervisor_cmd_t cmd = {0};
    if (script_path) {
        cmd.cmd = CMD_LOAD_SCRIPT;
        strncpy(cmd.script_path, script_path, sizeof(cmd.script_path) - 1);
    } else {
        cmd.cmd = CMD_ENTER_UI_MODE;
    }

    return xQueueSend(s_cmd_queue, &cmd, pdMS_TO_TICKS(100)) == pdPASS;
}

bool supervisor_stop_script(void)
{
    if (!s_cmd_queue) {
        return false;
    }

    supervisor_cmd_t cmd = {
        .cmd = CMD_STOP_SCRIPT
    };

    return xQueueSend(s_cmd_queue, &cmd, pdMS_TO_TICKS(100)) == pdPASS;
}

bool supervisor_is_script_running(void)
{
    return s_state == SUPERVISOR_STATE_RUNNING;
}

supervisor_state_t supervisor_get_state(void)
{
    return s_state;
}

const char* supervisor_get_current_script(void)
{
    return s_current_script[0] ? s_current_script : NULL;
}

bool supervisor_get_last_result(supervisor_script_result_t *result)
{
    if (!s_has_result || !result) {
        return false;
    }
    xSemaphoreTake(s_state_mutex, portMAX_DELAY);
    memcpy(result, &s_last_result, sizeof(supervisor_script_result_t));
    xSemaphoreGive(s_state_mutex);
    return true;
}

void supervisor_log(const char *format, ...)
{
    char buf[256];
    va_list args;
    va_start(args, format);
    vsnprintf(buf, sizeof(buf), format, args);
    va_end(args);

    // Always log to serial
    ESP_LOGI(TAG, "%s", buf);

#if defined(CONFIG_USB_MIDI_BOARD_M5STACK_CORES3) || defined(CONFIG_USB_MIDI_BOARD_M5STACK_CORES3_USB_SERIAL) || defined(CONFIG_USB_MIDI_BOARD_M5STACK_TAB5)
    // Also log to M5Stack UI
    extern void ui_add_log(const char *msg);
    ui_add_log(buf);
#endif
}

const char* supervisor_get_requested_script(void)
{
    return s_requested_script[0] ? s_requested_script : NULL;
}

void supervisor_clear_requested_script(void)
{
    s_requested_script[0] = '\0';
}

void supervisor_ruby_request_script(const char *script_path)
{
    if (script_path) {
        strncpy(s_ruby_requested_script, script_path, sizeof(s_ruby_requested_script) - 1);
        s_ruby_requested_script[sizeof(s_ruby_requested_script) - 1] = '\0';
    } else {
        s_ruby_requested_script[0] = '\0';
    }
    s_ruby_script_requested = true;
    ESP_LOGI(TAG, "Ruby requested script: %s", script_path ? script_path : "(UI mode)");
}

bool supervisor_ruby_script_requested(void)
{
    return s_ruby_script_requested;
}

const char* supervisor_get_ruby_requested_script(void)
{
    return s_ruby_requested_script[0] ? s_ruby_requested_script : NULL;
}

void supervisor_clear_ruby_request(void)
{
    s_ruby_script_requested = false;
    s_ruby_requested_script[0] = '\0';
}

// ============================================================================
// Internal Functions
// ============================================================================

#if defined(PICORB_VM_MRUBYC)
/**
 * @brief Build error message from VM exception
 * @param vm      Pointer to VM
 * @param buf     Output buffer
 * @param bufsize Buffer size
 * @return true if error message was built, false if no exception
 */
static bool build_error_message(const mrbc_vm *vm, char *buf, size_t bufsize)
{
    if (!mrbc_israised(vm)) {
        return false;
    }

    const mrbc_exception *exc = vm->exception.exception;
    const char *clsname = mrbc_symid_to_str(exc->cls->sym_id);
    const char *message = exc->message ? (const char *)exc->message : clsname;

    int offset = 0;

    // Build basic message: "ExceptionClass in `method': message"
    if (exc->method_id) {
        offset = snprintf(buf, bufsize, "%s in `%s': %s",
                         clsname,
                         mrbc_symid_to_str(exc->method_id),
                         message);
    } else {
        offset = snprintf(buf, bufsize, "%s: %s", clsname, message);
    }

    // Add call stack (max 3 levels for UI display)
    for (int i = 0; i < 3 && i < MRBC_EXCEPTION_CALL_NEST_LEVEL; i++) {
        if (!exc->call_nest[i]) break;
        int remaining = bufsize - offset;
        if (remaining <= 0) break;
        offset += snprintf(buf + offset, remaining, "\n  in `%s'",
                          mrbc_symid_to_str(exc->call_nest[i]));
    }

    return true;
}
#endif

static void stop_picoruby_task(void)
{
    if (s_picoruby_task == NULL) {
        return;
    }

    ESP_LOGI(TAG, "Stopping PicoRuby task...");
    s_state = SUPERVISOR_STATE_STOPPING;

    // Request stop via picoruby-esp32 mechanism
    extern void picoruby_esp32_request_stop(void);
    picoruby_esp32_request_stop();

    // Wait for task to complete (with timeout)
    EventBits_t bits = xEventGroupWaitBits(
        s_events,
        EVT_TASK_COMPLETED | EVT_TASK_ERROR,
        pdTRUE,
        pdFALSE,
        pdMS_TO_TICKS(5000)
    );

    if (!(bits & (EVT_TASK_COMPLETED | EVT_TASK_ERROR))) {
        // Timeout - force delete
        ESP_LOGW(TAG, "Task stop timeout, forcing deletion");
        if (s_picoruby_task != NULL) {
            vTaskDelete(s_picoruby_task);
        }
    }

    s_picoruby_task = NULL;
    ESP_LOGI(TAG, "PicoRuby task stopped");
}

static void cleanup_vm(void)
{
#if defined(PICORB_VM_MRUBYC)
    ESP_LOGI(TAG, "Cleaning up VM...");
    mrbc_cleanup();

    // Reset require flags so gems can be re-required in the next VM instance
    // This is critical because mrbc_cleanup() doesn't reset the prebuilt_gems[].required flags
    for (int i = 0; prebuilt_gems[i].name != NULL; i++) {
        prebuilt_gems[i].required = false;
    }
    ESP_LOGI(TAG, "Require flags reset");

    // Reset sandbox global state (g_suspend_vm_code)
    // This is critical because mrbc_cleanup() invalidates the memory that g_suspend_vm_code points to
    mrbc_sandbox_cleanup();

    // Note: mrbc_init_global() will be called automatically in the next mrbc_init()
    // Calling it here would crash because mrbc_cleanup() clears the memory allocator

    ESP_LOGI(TAG, "VM cleanup complete");
#endif
}

static void start_picoruby_task(const char *script_path)
{
    ESP_LOGI(TAG, "Starting PicoRuby task, script: %s",
             script_path ? script_path : "(UI mode)");

    // Set the requested script for main_task.rb to read
    if (script_path) {
        strncpy(s_requested_script, script_path, sizeof(s_requested_script) - 1);
        s_requested_script[sizeof(s_requested_script) - 1] = '\0';
        strncpy(s_current_script, script_path, sizeof(s_current_script) - 1);
        s_current_script[sizeof(s_current_script) - 1] = '\0';
    } else {
        s_requested_script[0] = '\0';
        s_current_script[0] = '\0';
    }

    // Clear Ruby request flag
    supervisor_clear_ruby_request();

    // Create the picoruby runner task
    BaseType_t ret = xTaskCreatePinnedToCore(
        picoruby_runner_task,
        "picoruby",
        16384,  // 16KB stack
        NULL,
        3,      // Lower priority than supervisor
        &s_picoruby_task,
        1       // Core 1
    );

    if (ret != pdPASS) {
        ESP_LOGE(TAG, "Failed to create PicoRuby task");
        s_state = SUPERVISOR_STATE_IDLE;
        return;
    }

    s_state = SUPERVISOR_STATE_RUNNING;
    xEventGroupSetBits(s_events, EVT_TASK_STARTED);
}

static void supervisor_task(void *arg)
{
    (void)arg;
    supervisor_cmd_t cmd;

    ESP_LOGI(TAG, "Supervisor task started");

    // Start in UI mode initially
    start_picoruby_task(NULL);

    while (1) {
        // Check for commands
        if (xQueueReceive(s_cmd_queue, &cmd, pdMS_TO_TICKS(100)) == pdPASS) {
            switch (cmd.cmd) {
                case CMD_LOAD_SCRIPT:
                    ESP_LOGI(TAG, "Command: Load script %s", cmd.script_path);
                    // Stop current task if running
                    if (s_picoruby_task != NULL) {
                        stop_picoruby_task();
                    }
                    // MIDI cleanup before VM reset
                    picoruby_esp32_midi_cleanup();
                    vTaskDelay(pdMS_TO_TICKS(50));  // Allow MIDI messages to be sent
                    // Cleanup and reinitialize VM
                    cleanup_vm();
                    // Start new task
                    start_picoruby_task(cmd.script_path);
                    break;

                case CMD_STOP_SCRIPT:
                    ESP_LOGI(TAG, "Command: Stop script");
                    if (s_picoruby_task != NULL) {
                        stop_picoruby_task();
                        picoruby_esp32_midi_cleanup();
                        cleanup_vm();
                        // Return to UI mode
                        start_picoruby_task(NULL);
                    }
                    break;

                case CMD_ENTER_UI_MODE:
                    ESP_LOGI(TAG, "Command: Enter UI mode");
                    if (s_picoruby_task != NULL) {
                        stop_picoruby_task();
                    }
                    picoruby_esp32_midi_cleanup();
                    cleanup_vm();
                    start_picoruby_task(NULL);
                    break;
            }
        }

        // Check if Ruby requested a script change
        if (s_ruby_script_requested) {
            const char *requested = supervisor_get_ruby_requested_script();
            ESP_LOGI(TAG, "Ruby requested script change: %s",
                     requested ? requested : "(UI mode)");

            // Wait for current task to complete naturally
            // (Ruby should exit after calling request_script)
            EventBits_t bits = xEventGroupWaitBits(
                s_events,
                EVT_TASK_COMPLETED | EVT_TASK_ERROR,
                pdTRUE,
                pdFALSE,
                pdMS_TO_TICKS(2000)
            );

            if (bits & (EVT_TASK_COMPLETED | EVT_TASK_ERROR)) {
                // Task ended, process the request
                char next_script[128] = {0};
                if (requested) {
                    strncpy(next_script, requested, sizeof(next_script) - 1);
                }
                supervisor_clear_ruby_request();

                // MIDI cleanup
                picoruby_esp32_midi_cleanup();
                vTaskDelay(pdMS_TO_TICKS(50));

                // Cleanup old task handle
                s_picoruby_task = NULL;

                // Cleanup VM
                cleanup_vm();

                // Start new task
                start_picoruby_task(next_script[0] ? next_script : NULL);
            } else {
                // Timeout waiting for task
                ESP_LOGW(TAG, "Timeout waiting for task to exit, forcing restart");
                supervisor_clear_ruby_request();
                stop_picoruby_task();
                picoruby_esp32_midi_cleanup();
                cleanup_vm();
                start_picoruby_task(NULL);
            }
        }

        // Check if task completed without a script request (script ended normally)
        EventBits_t bits = xEventGroupWaitBits(
            s_events,
            EVT_TASK_COMPLETED | EVT_TASK_ERROR,
            pdTRUE,
            pdFALSE,
            0  // Don't wait
        );

        if ((bits & (EVT_TASK_COMPLETED | EVT_TASK_ERROR)) && !s_ruby_script_requested) {
            s_picoruby_task = NULL;

            // Log the result
            if (bits & EVT_TASK_ERROR) {
                supervisor_log("[Script] Error: %s", s_last_result.error_message);
            } else {
                supervisor_log("[Script] Completed in %lu ms",
                              (unsigned long)s_last_result.execution_time_ms);
            }

            // MIDI cleanup
            picoruby_esp32_midi_cleanup();
            vTaskDelay(pdMS_TO_TICKS(50));

            // Check if there's a pending UI script change request (e.g., user selected another script)
            extern volatile bool g_script_change_requested;
            extern char g_requested_script[];
            if (g_script_change_requested && g_requested_script[0] != '\0') {
                // UI requested a script change - run that script instead of returning to UI mode
                char next_script[128];
                strncpy(next_script, g_requested_script, sizeof(next_script) - 1);
                next_script[sizeof(next_script) - 1] = '\0';

                // Clear the request flags
                g_script_change_requested = false;
                extern volatile bool g_stop_requested;
                g_stop_requested = false;
                g_requested_script[0] = '\0';

                ESP_LOGI(TAG, "UI requested script change: %s", next_script);

                // Cleanup VM and start the requested script
                cleanup_vm();
                start_picoruby_task(next_script);
            } else {
                // No pending request, return to UI mode
                ESP_LOGI(TAG, "Script ended, returning to UI mode");
                cleanup_vm();
                start_picoruby_task(NULL);
            }
        }
    }
}

static void picoruby_runner_task(void *arg)
{
    (void)arg;
    uint32_t start_time = xTaskGetTickCount();

    ESP_LOGI(TAG, "PicoRuby runner task started");

    // Run the VM with main_task.rb (returns success/failure)
    bool success = run_vm_with_main_task();

    // Calculate execution time
    uint32_t end_time = xTaskGetTickCount();
    uint32_t exec_time = (end_time - start_time) * portTICK_PERIOD_MS;

    // Store result (error_message is already set in run_vm_with_main_task if error)
    xSemaphoreTake(s_state_mutex, portMAX_DELAY);
    s_last_result.execution_time_ms = exec_time;
    strncpy(s_last_result.script_path, s_current_script, sizeof(s_last_result.script_path) - 1);
    if (success) {
        s_last_result.success = true;
        s_last_result.error_message[0] = '\0';
    }
    // else: success=false and error_message already set by run_vm_with_main_task()
    s_has_result = true;
    xSemaphoreGive(s_state_mutex);

    ESP_LOGI(TAG, "PicoRuby runner task completed (exec time: %lu ms, success: %d)",
             (unsigned long)exec_time, success);

    // Notify supervisor with appropriate event
    if (success) {
        xEventGroupSetBits(s_events, EVT_TASK_COMPLETED);
    } else {
        xEventGroupSetBits(s_events, EVT_TASK_ERROR);
    }

    // Suspend self - supervisor will delete
    vTaskSuspend(NULL);
}

// ScriptManager methods for supervisor integration
#if defined(PICORB_VM_MRUBYC)

// Forward declarations for original ScriptManager methods
extern void picoruby_esp32_clear_script_list(void);
extern bool picoruby_esp32_add_script(const char *filename);
extern void picoruby_esp32_set_script_list_ready(bool ready);
extern void picoruby_esp32_midi_cleanup(void);
extern bool picoruby_esp32_stop_requested(void);
extern void picoruby_esp32_clear_stop_flag(void);

static void c_sm_clear(mrbc_vm *vm, mrbc_value v[], int argc)
{
    (void)vm; (void)v; (void)argc;
    picoruby_esp32_clear_script_list();
    SET_NIL_RETURN();
}

static void c_sm_add(mrbc_vm *vm, mrbc_value v[], int argc)
{
    (void)argc;
    if (v[1].tt != MRBC_TT_STRING) {
        SET_FALSE_RETURN();
        return;
    }
    const char *filename = (const char *)v[1].string->data;
    bool result = picoruby_esp32_add_script(filename);
    if (result) {
        SET_TRUE_RETURN();
    } else {
        SET_FALSE_RETURN();
    }
}

static void c_sm_set_ready(mrbc_vm *vm, mrbc_value v[], int argc)
{
    (void)argc;
    bool ready = true;
    if (argc > 0 && v[1].tt == MRBC_TT_FALSE) {
        ready = false;
    }
    picoruby_esp32_set_script_list_ready(ready);
    SET_NIL_RETURN();
}

static void c_sm_get_requested(mrbc_vm *vm, mrbc_value v[], int argc)
{
    (void)v; (void)argc;
    // Check for UI script change request (via picoruby_esp32_request_script_change)
    extern volatile bool g_script_change_requested;
    extern char g_requested_script[];
    if (g_script_change_requested && g_requested_script[0] != '\0') {
        ESP_LOGI(TAG, "get_requested returning: %s", g_requested_script);
        mrbc_value str = mrbc_string_new_cstr(vm, g_requested_script);
        SET_RETURN(str);
    } else {
        SET_NIL_RETURN();
    }
}

static void c_sm_clear_request(mrbc_vm *vm, mrbc_value v[], int argc)
{
    (void)vm; (void)v; (void)argc;
    extern volatile bool g_script_change_requested;
    extern volatile bool g_stop_requested;
    extern char g_requested_script[];
    ESP_LOGI(TAG, "clear_request called - clearing all stop flags");
    g_script_change_requested = false;
    g_stop_requested = false;
    g_requested_script[0] = '\0';
    SET_NIL_RETURN();
}

static void c_sm_stop_requested(mrbc_vm *vm, mrbc_value v[], int argc)
{
    (void)vm; (void)v; (void)argc;
    if (picoruby_esp32_stop_requested()) {
        SET_TRUE_RETURN();
    } else {
        SET_FALSE_RETURN();
    }
}

static void c_sm_check_console(mrbc_vm *vm, mrbc_value v[], int argc)
{
    (void)v; (void)argc;

    // Console buffer (local to this function for simplicity)
    static char console_buffer[256];
    static int console_pos = 0;

    int c = getchar();
    if (c == EOF || c < 0) {
        SET_NIL_RETURN();
        return;
    }

    // Handle backspace
    if (c == 0x08 || c == 0x7F) {
        if (console_pos > 0) {
            console_pos--;
            printf("\b \b");
            fflush(stdout);
        }
        SET_NIL_RETURN();
        return;
    }

    // Handle newline
    if (c == '\n' || c == '\r') {
        printf("\n");
        fflush(stdout);

        console_buffer[console_pos] = '\0';

        // Check for "load" command
        if (console_pos > 5 && strncmp(console_buffer, "load ", 5) == 0) {
            char *script_path = console_buffer + 5;
            while (*script_path == ' ') script_path++;

            if (*script_path != '\0') {
                ESP_LOGI(TAG, "Console: load %s", script_path);
                mrbc_value str = mrbc_string_new_cstr(vm, script_path);
                console_pos = 0;
                SET_RETURN(str);
                return;
            }
        } else if (strcmp(console_buffer, "heap") == 0) {
            size_t free_heap = esp_get_free_heap_size();
            printf("Free heap: %zu bytes\n", free_heap);
            fflush(stdout);
        } else if (strcmp(console_buffer, "restart") == 0) {
            printf("Restarting ESP32...\n");
            fflush(stdout);
            vTaskDelay(pdMS_TO_TICKS(100));
            extern void esp_restart(void);
            esp_restart();
        } else if (console_pos > 0) {
            printf("Unknown command: %s\n", console_buffer);
            printf("Commands: load /sd/app.rb, heap, restart\n");
            fflush(stdout);
        }

        console_pos = 0;
        printf("> ");
        fflush(stdout);
        SET_NIL_RETURN();
        return;
    }

    // Handle printable characters
    if (c >= 32 && c < 127 && console_pos < (int)sizeof(console_buffer) - 1) {
        putchar(c);
        fflush(stdout);
        console_buffer[console_pos++] = (char)c;
    }
    SET_NIL_RETURN();
}

static void c_sm_cleanup_midi(mrbc_vm *vm, mrbc_value v[], int argc)
{
    (void)vm; (void)v; (void)argc;
    ESP_LOGI(TAG, "Ruby requested MIDI cleanup");
    picoruby_esp32_midi_cleanup();
    SET_NIL_RETURN();
}

static void c_sm_free_heap(mrbc_vm *vm, mrbc_value v[], int argc)
{
    (void)v; (void)argc;
    size_t free_heap = esp_get_free_heap_size();
    SET_INT_RETURN(free_heap);
}

// New method: get_autorun_script - returns script path from supervisor
static void c_sm_get_autorun_script(mrbc_vm *vm, mrbc_value v[], int argc)
{
    (void)v; (void)argc;
    const char *script = supervisor_get_requested_script();
    if (script && script[0] != '\0') {
        ESP_LOGI(TAG, "get_autorun_script returning: %s", script);
        mrbc_value str = mrbc_string_new_cstr(vm, script);
        SET_RETURN(str);
    } else {
        SET_NIL_RETURN();
    }
}

// New method: request_script - request supervisor to switch scripts
static void c_sm_request_script(mrbc_vm *vm, mrbc_value v[], int argc)
{
    (void)vm;
    const char *script_path = NULL;
    if (argc > 0 && v[1].tt == MRBC_TT_STRING) {
        script_path = (const char *)v[1].string->data;
    }
    supervisor_ruby_request_script(script_path);
    SET_NIL_RETURN();
}

// SD refresh methods
static void c_sm_sd_refresh_requested(mrbc_vm *vm, mrbc_value v[], int argc)
{
    (void)vm; (void)v; (void)argc;
    extern volatile bool g_sd_refresh_requested;
    if (g_sd_refresh_requested) {
        SET_TRUE_RETURN();
    } else {
        SET_FALSE_RETURN();
    }
}

static void c_sm_clear_sd_refresh(mrbc_vm *vm, mrbc_value v[], int argc)
{
    (void)vm; (void)v; (void)argc;
    extern volatile bool g_sd_refresh_requested;
    g_sd_refresh_requested = false;
    ESP_LOGI(TAG, "SD refresh request cleared");
    SET_NIL_RETURN();
}

static void c_sm_add_log(mrbc_vm *vm, mrbc_value v[], int argc)
{
    (void)vm;
    if (argc < 1) {
        SET_NIL_RETURN();
        return;
    }

    mrbc_value str = v[1];
    if (mrbc_type(str) != MRBC_TT_STRING) {
        SET_NIL_RETURN();
        return;
    }

    const char *text = (const char *)mrbc_string_cstr(&str);
#if defined(CONFIG_USB_MIDI_BOARD_M5STACK_CORES3) || defined(CONFIG_USB_MIDI_BOARD_M5STACK_CORES3_USB_SERIAL) || defined(CONFIG_USB_MIDI_BOARD_M5STACK_TAB5)
    extern void ui_add_log(const char *msg);
    ui_add_log(text);
#else
    // For non-M5Stack boards, just log to console
    ESP_LOGI(TAG, "[Log] %s", text);
#endif
    SET_NIL_RETURN();
}

static void register_script_manager_class(mrbc_vm *vm)
{
    mrbc_class *cls = mrbc_define_class(vm, "ScriptManager", mrbc_class_object);
    mrbc_define_method(vm, cls, "clear", c_sm_clear);
    mrbc_define_method(vm, cls, "add", c_sm_add);
    mrbc_define_method(vm, cls, "set_ready", c_sm_set_ready);
    mrbc_define_method(vm, cls, "get_requested", c_sm_get_requested);
    mrbc_define_method(vm, cls, "clear_request", c_sm_clear_request);
    mrbc_define_method(vm, cls, "stop_requested?", c_sm_stop_requested);
    mrbc_define_method(vm, cls, "check_console", c_sm_check_console);
    mrbc_define_method(vm, cls, "cleanup_midi", c_sm_cleanup_midi);
    mrbc_define_method(vm, cls, "free_heap", c_sm_free_heap);
    mrbc_define_method(vm, cls, "sd_refresh_requested?", c_sm_sd_refresh_requested);
    mrbc_define_method(vm, cls, "clear_sd_refresh", c_sm_clear_sd_refresh);
    mrbc_define_method(vm, cls, "add_log", c_sm_add_log);
    // New supervisor-based methods
    mrbc_define_method(vm, cls, "get_autorun_script", c_sm_get_autorun_script);
    mrbc_define_method(vm, cls, "request_script", c_sm_request_script);
    ESP_LOGI(TAG, "ScriptManager class registered");
}

#endif // PICORB_VM_MRUBYC

static bool run_vm_with_main_task(void)
{
#if defined(PICORB_VM_MRUBYC)
    ESP_LOGI(TAG, "Initializing VM...");

    // Initialize mruby/c
    mrbc_init(heap_pool, HEAP_SIZE);

    // Create main task from bytecode
    mrbc_tcb *main_tcb = mrbc_create_task(main_task, 0);
    if (main_tcb == NULL) {
        ESP_LOGE(TAG, "Failed to create main_task");
        return false;
    }
    mrbc_set_task_name(main_tcb, "main_task");

    // Set flag_permanence to prevent automatic mrbc_vm_end() call in mrbc_run()
    // This allows us to check for exceptions after mrbc_run() returns
    main_tcb->vm.flag_permanence = 1;

    mrbc_vm *vm = &main_tcb->vm;

    // Initialize require system
    extern void picoruby_init_require(mrbc_vm *vm);
    picoruby_init_require(vm);

    // Register ScriptManager class
    register_script_manager_class(vm);

    // Run the VM (this blocks until all tasks complete)
    mrbc_run();

    // Check for exceptions after VM execution
    bool success = true;
    mrbc_tcb *tcb = mrbc_find_task("main_task");
    if (tcb && mrbc_israised(&tcb->vm)) {
        success = false;

        // Build error message and store in result
        xSemaphoreTake(s_state_mutex, portMAX_DELAY);
        build_error_message(&tcb->vm, s_last_result.error_message,
                           sizeof(s_last_result.error_message));
        s_last_result.success = false;
        xSemaphoreGive(s_state_mutex);

        ESP_LOGE(TAG, "Script error: %s", s_last_result.error_message);

        // Clear exception before calling mrbc_vm_end() to avoid duplicate output
        mrbc_clear_exception(&tcb->vm);
    }

    // Manually end the VM (since we set flag_permanence = 1)
    if (tcb) {
        mrbc_vm_end(&tcb->vm);
    }

    ESP_LOGI(TAG, "main_task.rb completed (success=%d)", success);
    return success;
#else
    ESP_LOGW(TAG, "mruby VM not supported in supervisor mode");
    return false;
#endif
}
