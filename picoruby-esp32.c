#include <inttypes.h>
#include <string.h>
#include <nvs_flash.h>
#include <stdio.h>
#include "esp_log.h"
#include "esp_system.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "picoruby.h"
#include "picoruby-esp32.h"

static const char *TAG = "PICORUBY";

// Console command buffer for serial input
static char s_console_buffer[256];
static int s_console_buffer_pos = 0;

#if defined(CONFIG_IDF_TARGET_ESP32S3) && defined(CONFIG_SPIRAM)
#include "esp_attr.h"
#define HEAP_IN_PSRAM 1
#endif

#if defined(PICORB_VM_MRUBYC)
#include <mrubyc.h>
#elif defined(PICORB_VM_MRUBY)
#include "hal.h" // in picoruby-machine
#endif

// main_task bytecode is compiled separately and linked
extern const uint8_t main_task[];

// Forward declarations for Ruby-callable C functions
#if defined(PICORB_VM_MRUBYC)
static void c_script_manager_clear(mrbc_vm *vm, mrbc_value v[], int argc);
static void c_script_manager_add(mrbc_vm *vm, mrbc_value v[], int argc);
static void c_script_manager_set_ready(mrbc_vm *vm, mrbc_value v[], int argc);
static void c_script_manager_get_requested(mrbc_vm *vm, mrbc_value v[], int argc);
static void c_script_manager_clear_request(mrbc_vm *vm, mrbc_value v[], int argc);
static void c_script_manager_stop_requested(mrbc_vm *vm, mrbc_value v[], int argc);
static void c_script_manager_check_console(mrbc_vm *vm, mrbc_value v[], int argc);
static void c_script_manager_cleanup_midi(mrbc_vm *vm, mrbc_value v[], int argc);
static void c_script_manager_free_heap(mrbc_vm *vm, mrbc_value v[], int argc);
static void c_script_manager_esp_restart(mrbc_vm *vm, mrbc_value v[], int argc);
static void c_script_manager_set_autorun(mrbc_vm *vm, mrbc_value v[], int argc);
static void c_script_manager_get_autorun(mrbc_vm *vm, mrbc_value v[], int argc);
static void c_script_manager_clear_autorun(mrbc_vm *vm, mrbc_value v[], int argc);
static void c_script_manager_sd_refresh_requested(mrbc_vm *vm, mrbc_value v[], int argc);
static void c_script_manager_clear_sd_refresh(mrbc_vm *vm, mrbc_value v[], int argc);
#endif

// NVS namespace and key for autorun script
#define NVS_NAMESPACE "picoruby"
#define NVS_KEY_AUTORUN "autorun"

#ifndef HEAP_SIZE
#if defined(CONFIG_IDF_TARGET_ESP32S3) && defined(CONFIG_SPIRAM)
// ESP32-S3 with PSRAM: Use 2MB heap (plenty of room in 8MB PSRAM)
#define HEAP_SIZE (1024 * 1024 * 2)
#elif defined(CONFIG_IDF_TARGET_ESP32S3)
// ESP32-S3 without PSRAM: Conservative heap
#define HEAP_SIZE (1024 * 180)
#elif defined(CONFIG_IDF_TARGET_ESP32C3)
#define HEAP_SIZE (1024 * 120)
#else
#define HEAP_SIZE (1024 * 120)
#endif
#endif

#if defined(HEAP_IN_PSRAM)
// Place heap in PSRAM for ESP32-S3 with external memory
// Note: Not static, accessible from picoruby_supervisor.c
EXT_RAM_BSS_ATTR uint8_t heap_pool[HEAP_SIZE];
#else
uint8_t heap_pool[HEAP_SIZE];
#endif

#if defined(PICORB_VM_MRUBY)
mrb_state *global_mrb = NULL;
#endif

// Track initialization state
static bool g_vm_initialized = false;

#if defined(PICORB_VM_MRUBYC)
static mrbc_vm *g_vm = NULL;
#endif

// Script management (accessible from picoruby_supervisor.c)
static char g_current_script[128] = {0};
char g_requested_script[128] = {0};
volatile bool g_stop_requested = false;
volatile bool g_script_change_requested = false;

void
initialize_nvs(void)
{
  esp_err_t ret = nvs_flash_init();
  if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
    ESP_ERROR_CHECK(nvs_flash_erase());
    ret = nvs_flash_init();
  }
  ESP_ERROR_CHECK(ret);
}

void
picoruby_esp32(void)
{
  initialize_nvs();

#if defined(PICORB_VM_MRUBYC)
  mrbc_init(heap_pool, HEAP_SIZE);

  mrbc_tcb *main_tcb = mrbc_create_task(main_task, 0);
  mrbc_set_task_name(main_tcb, "main_task");
  mrbc_vm *vm = &main_tcb->vm;
  g_vm = vm;  // Store for later use

  picoruby_init_require(vm);

  // Register ScriptManager class for Ruby to notify C about scripts
  mrbc_class *class_ScriptManager = mrbc_define_class(vm, "ScriptManager", mrbc_class_object);
  mrbc_define_method(vm, class_ScriptManager, "clear", c_script_manager_clear);
  mrbc_define_method(vm, class_ScriptManager, "add", c_script_manager_add);
  mrbc_define_method(vm, class_ScriptManager, "set_ready", c_script_manager_set_ready);
  mrbc_define_method(vm, class_ScriptManager, "get_requested", c_script_manager_get_requested);
  mrbc_define_method(vm, class_ScriptManager, "clear_request", c_script_manager_clear_request);
  mrbc_define_method(vm, class_ScriptManager, "stop_requested?", c_script_manager_stop_requested);
  mrbc_define_method(vm, class_ScriptManager, "check_console", c_script_manager_check_console);
  mrbc_define_method(vm, class_ScriptManager, "cleanup_midi", c_script_manager_cleanup_midi);
  mrbc_define_method(vm, class_ScriptManager, "free_heap", c_script_manager_free_heap);
  mrbc_define_method(vm, class_ScriptManager, "esp_restart", c_script_manager_esp_restart);
  mrbc_define_method(vm, class_ScriptManager, "set_autorun", c_script_manager_set_autorun);
  mrbc_define_method(vm, class_ScriptManager, "get_autorun", c_script_manager_get_autorun);
  mrbc_define_method(vm, class_ScriptManager, "clear_autorun", c_script_manager_clear_autorun);
  mrbc_define_method(vm, class_ScriptManager, "sd_refresh_requested?", c_script_manager_sd_refresh_requested);
  mrbc_define_method(vm, class_ScriptManager, "clear_sd_refresh", c_script_manager_clear_sd_refresh);
  ESP_LOGI(TAG, "ScriptManager class registered");

  g_vm_initialized = true;
  mrbc_run();
#elif defined(PICORB_VM_MRUBY)
  mrb_state *mrb = mrb_open_with_custom_alloc(heap_pool, HEAP_SIZE);
  global_mrb = mrb;
  mrc_irep *irep = mrb_read_irep(mrb, main_task);
  mrc_ccontext *cc = mrc_ccontext_new(mrb);
  mrb_value name = mrb_str_new_lit(mrb, "R2P2");
  mrb_value task = mrc_create_task(cc, irep, name, mrb_nil_value(), mrb_obj_value(mrb->top_self));
  if (mrb_nil_p(task)) {
    const char *msg = "mrbc_create_task failed\n";
    hal_write(1, msg, strlen(msg));
  }
  else {
    mrb_tasks_run(mrb);
  }
  if (mrb->exc) {
    mrb_print_error(mrb);
  }
  mrb_close(mrb);
  mrc_ccontext_free(cc);
#endif
}

bool
picoruby_esp32_init(void)
{
  if (g_vm_initialized) {
    return true;
  }

  initialize_nvs();

#if defined(PICORB_VM_MRUBYC)
  mrbc_init(heap_pool, HEAP_SIZE);
  g_vm = mrbc_vm_open(NULL);
  if (g_vm == NULL) {
    ESP_LOGE(TAG, "Failed to open mrubyc VM");
    return false;
  }
  picoruby_init_require(g_vm);

  // Register ScriptManager class for Ruby to notify C about scripts
  mrbc_class *class_ScriptManager = mrbc_define_class(g_vm, "ScriptManager", mrbc_class_object);
  mrbc_define_method(g_vm, class_ScriptManager, "clear", c_script_manager_clear);
  mrbc_define_method(g_vm, class_ScriptManager, "add", c_script_manager_add);
  mrbc_define_method(g_vm, class_ScriptManager, "set_ready", c_script_manager_set_ready);
  mrbc_define_method(g_vm, class_ScriptManager, "get_requested", c_script_manager_get_requested);
  mrbc_define_method(g_vm, class_ScriptManager, "clear_request", c_script_manager_clear_request);
  mrbc_define_method(g_vm, class_ScriptManager, "stop_requested?", c_script_manager_stop_requested);
  mrbc_define_method(g_vm, class_ScriptManager, "check_console", c_script_manager_check_console);
  mrbc_define_method(g_vm, class_ScriptManager, "cleanup_midi", c_script_manager_cleanup_midi);
  mrbc_define_method(g_vm, class_ScriptManager, "free_heap", c_script_manager_free_heap);
  mrbc_define_method(g_vm, class_ScriptManager, "esp_restart", c_script_manager_esp_restart);
  mrbc_define_method(g_vm, class_ScriptManager, "set_autorun", c_script_manager_set_autorun);
  mrbc_define_method(g_vm, class_ScriptManager, "get_autorun", c_script_manager_get_autorun);
  mrbc_define_method(g_vm, class_ScriptManager, "clear_autorun", c_script_manager_clear_autorun);
  mrbc_define_method(g_vm, class_ScriptManager, "sd_refresh_requested?", c_script_manager_sd_refresh_requested);
  mrbc_define_method(g_vm, class_ScriptManager, "clear_sd_refresh", c_script_manager_clear_sd_refresh);
  ESP_LOGI(TAG, "ScriptManager class registered");

  g_vm_initialized = true;
  ESP_LOGI(TAG, "PicoRuby VM (mrubyc) initialized");
  return true;
#elif defined(PICORB_VM_MRUBY)
  global_mrb = mrb_open_with_custom_alloc(heap_pool, HEAP_SIZE);
  if (global_mrb == NULL) {
    ESP_LOGE(TAG, "Failed to open mruby VM");
    return false;
  }
  g_vm_initialized = true;
  ESP_LOGI(TAG, "PicoRuby VM (mruby) initialized");
  return true;
#else
  return false;
#endif
}

bool
picoruby_esp32_is_initialized(void)
{
  return g_vm_initialized;
}

bool
picoruby_esp32_run_script(const char *script, size_t script_len, const char *filename)
{
  if (script == NULL) {
    ESP_LOGE(TAG, "Script is NULL");
    return false;
  }

  // Auto-initialize if not done
  if (!g_vm_initialized) {
    if (!picoruby_esp32_init()) {
      return false;
    }
  }

  if (script_len == 0) {
    script_len = strlen(script);
  }

  ESP_LOGI(TAG, "Running script: %s (%zu bytes)", filename ? filename : "<buffer>", script_len);

#if defined(PICORB_VM_MRUBYC)
  // Create compiler context
  mrc_ccontext *cc = mrc_ccontext_new(NULL);
  if (cc == NULL) {
    ESP_LOGE(TAG, "Failed to create compiler context");
    return false;
  }

  if (filename) {
    mrc_ccontext_filename(cc, filename);
  }

  // Compile the script
  const uint8_t *src = (const uint8_t *)script;
  mrc_irep *irep = mrc_load_string_cxt(cc, &src, script_len);
  if (irep == NULL) {
    ESP_LOGE(TAG, "Failed to compile script");
    mrc_ccontext_free(cc);
    return false;
  }

  // Dump irep to bytecode
  uint8_t *bytecode = NULL;
  size_t bytecode_size = 0;
  if (mrc_dump_irep(cc, irep, 0, &bytecode, &bytecode_size) != MRC_DUMP_OK) {
    ESP_LOGE(TAG, "Failed to dump irep");
    mrc_ccontext_free(cc);
    return false;
  }

  // Create task and run
  mrbc_tcb *tcb = mrbc_create_task(bytecode, 0);
  if (tcb == NULL) {
    ESP_LOGE(TAG, "Failed to create task");
    mrc_ccontext_free(cc);
    return false;
  }
  mrbc_set_task_name(tcb, filename ? filename : "script");

  // Run the task
  mrbc_run();

  mrc_ccontext_free(cc);
  ESP_LOGI(TAG, "Script execution completed");
  return true;

#elif defined(PICORB_VM_MRUBY)
  // Create compiler context
  mrc_ccontext *cc = mrc_ccontext_new(global_mrb);
  if (cc == NULL) {
    ESP_LOGE(TAG, "Failed to create compiler context");
    return false;
  }

  if (filename) {
    mrc_ccontext_filename(cc, filename);
  }

  // Compile the script
  const uint8_t *src = (const uint8_t *)script;
  mrc_irep *irep = mrc_load_string_cxt(cc, &src, script_len);
  if (irep == NULL) {
    ESP_LOGE(TAG, "Failed to compile script");
    mrc_ccontext_free(cc);
    return false;
  }

  // Create task and run
  mrb_value name_val = mrb_str_new_cstr(global_mrb, filename ? filename : "script");
  mrb_value task = mrc_create_task(cc, irep, name_val, mrb_nil_value(), mrb_obj_value(global_mrb->top_self));

  if (mrb_nil_p(task)) {
    ESP_LOGE(TAG, "Failed to create task");
    mrc_ccontext_free(cc);
    return false;
  }

  mrb_tasks_run(global_mrb);

  bool success = true;
  if (global_mrb->exc) {
    mrb_print_error(global_mrb);
    success = false;
  }

  mrc_ccontext_free(cc);
  ESP_LOGI(TAG, "Script execution completed");
  return success;
#else
  return false;
#endif
}

bool
picoruby_esp32_request_script_change(const char *script_path)
{
  if (script_path == NULL) {
    return false;
  }

  strncpy(g_requested_script, script_path, sizeof(g_requested_script) - 1);
  g_requested_script[sizeof(g_requested_script) - 1] = '\0';
  g_script_change_requested = true;
  g_stop_requested = true;

  // Update current script path
  strncpy(g_current_script, script_path, sizeof(g_current_script) - 1);
  g_current_script[sizeof(g_current_script) - 1] = '\0';

  ESP_LOGI(TAG, "Script change requested: %s", script_path);
  return true;
}

const char*
picoruby_esp32_get_current_script(void)
{
  return g_current_script[0] ? g_current_script : NULL;
}

void
picoruby_esp32_request_stop(void)
{
  g_stop_requested = true;
  ESP_LOGI(TAG, "Stop requested");
}

bool
picoruby_esp32_stop_requested(void)
{
  return g_stop_requested;
}

void
picoruby_esp32_clear_stop_flag(void)
{
  g_stop_requested = false;
  g_script_change_requested = false;
}

void
picoruby_esp32_midi_cleanup(void)
{
  ESP_LOGI(TAG, "Performing MIDI cleanup...");

  // Declare external functions
  extern int USB_MIDI_send_packet(uint8_t cable, uint8_t cin, uint8_t midi1, uint8_t midi2, uint8_t midi3);
  extern int SAM2695_send_packet(uint8_t cable, uint8_t cin, uint8_t midi1, uint8_t midi2, uint8_t midi3);

  // Send cleanup messages to all MIDI channels (0-15)
  for (uint8_t ch = 0; ch < 16; ch++) {
    uint8_t status_cc = 0xB0 | ch;  // Control Change

    // All Sound Off (CC #120)
    USB_MIDI_send_packet(0, 0x0B, status_cc, 120, 0);
    SAM2695_send_packet(0, 0x0B, status_cc, 120, 0);

    // All Notes Off (CC #123)
    USB_MIDI_send_packet(0, 0x0B, status_cc, 123, 0);
    SAM2695_send_packet(0, 0x0B, status_cc, 123, 0);
  }

  // Send MIDI Stop (0xFC)
  USB_MIDI_send_packet(0, 0x05, 0xFC, 0, 0);  // CIN 0x05 for single-byte system common
  SAM2695_send_packet(0, 0x05, 0xFC, 0, 0);

  ESP_LOGI(TAG, "MIDI cleanup completed");
}

// Script list management
static char g_script_list[PICORUBY_MAX_SCRIPTS][PICORUBY_MAX_SCRIPT_NAME];
static int g_script_count = 0;
static volatile bool g_script_list_ready = false;

// SD card refresh request (from UI to Ruby)
// Note: Not static, accessible from picoruby_supervisor.c
volatile bool g_sd_refresh_requested = false;

// Ruby-callable C functions for script management
#if defined(PICORB_VM_MRUBYC)
static void
c_script_manager_clear(mrbc_vm *vm, mrbc_value v[], int argc)
{
  (void)vm; (void)v; (void)argc;
  picoruby_esp32_clear_script_list();
  SET_NIL_RETURN();
}

static void
c_script_manager_add(mrbc_vm *vm, mrbc_value v[], int argc)
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

static void
c_script_manager_set_ready(mrbc_vm *vm, mrbc_value v[], int argc)
{
  (void)argc;
  bool ready = true;
  if (argc > 0 && v[1].tt == MRBC_TT_FALSE) {
    ready = false;
  }
  picoruby_esp32_set_script_list_ready(ready);
  SET_NIL_RETURN();
}

static void
c_script_manager_get_requested(mrbc_vm *vm, mrbc_value v[], int argc)
{
  (void)v; (void)argc;
  if (g_script_change_requested && g_requested_script[0] != '\0') {
    ESP_LOGI(TAG, "get_requested returning: %s", g_requested_script);
    mrbc_value str = mrbc_string_new_cstr(vm, g_requested_script);
    SET_RETURN(str);
  } else {
    SET_NIL_RETURN();
  }
}

static void
c_script_manager_clear_request(mrbc_vm *vm, mrbc_value v[], int argc)
{
  (void)vm; (void)v; (void)argc;
  ESP_LOGI(TAG, "clear_request called - clearing all stop flags");
  g_script_change_requested = false;
  g_stop_requested = false;  // IMPORTANT: Clear stop flag too!
  g_requested_script[0] = '\0';
  SET_NIL_RETURN();
}

static void
c_script_manager_stop_requested(mrbc_vm *vm, mrbc_value v[], int argc)
{
  (void)vm; (void)v; (void)argc;
  ESP_LOGD(TAG, "stop_requested? called - g_stop_requested=%d, g_script_change_requested=%d",
           g_stop_requested, g_script_change_requested);
  if (g_stop_requested) {
    SET_TRUE_RETURN();
  } else {
    SET_FALSE_RETURN();
  }
}

static void
c_script_manager_check_console(mrbc_vm *vm, mrbc_value v[], int argc)
{
  (void)v; (void)argc;

  // Check for serial input (non-blocking)
  int c = getchar();
  if (c == EOF || c < 0) {
    SET_NIL_RETURN();
    return;
  }

  // Handle backspace (0x08 or 0x7F)
  if (c == 0x08 || c == 0x7F) {
    if (s_console_buffer_pos > 0) {
      s_console_buffer_pos--;
      // Echo backspace: \b (move cursor back), space (erase char), \b (move cursor back again)
      printf("\b \b");
      fflush(stdout);
    }
    SET_NIL_RETURN();
    return;
  }

  // Handle newline/carriage return
  if (c == '\n' || c == '\r') {
    printf("\n");  // Echo newline
    fflush(stdout);

    // Process command
    s_console_buffer[s_console_buffer_pos] = '\0';

    // Check if it's a "load" command
    if (s_console_buffer_pos > 5 && strncmp(s_console_buffer, "load ", 5) == 0) {
      char *script_path = s_console_buffer + 5;
      // Trim leading spaces
      while (*script_path == ' ') script_path++;

      if (*script_path != '\0') {
        ESP_LOGI(TAG, "Console command: load %s", script_path);
        // Return the script path to Ruby
        // Ruby will handle set_autorun + esp_restart
        mrbc_value str = mrbc_string_new_cstr(vm, script_path);
        s_console_buffer_pos = 0;
        SET_RETURN(str);
        return;
      }
    } else if (strcmp(s_console_buffer, "restart") == 0 || strcmp(s_console_buffer, "reboot") == 0) {
      // Restart ESP32
      printf("Restarting ESP32...\n");
      fflush(stdout);
      vTaskDelay(pdMS_TO_TICKS(100));
      esp_restart();
      // Never returns
    } else if (strcmp(s_console_buffer, "heap") == 0 || strcmp(s_console_buffer, "free") == 0) {
      // Show free heap
      size_t free_heap = esp_get_free_heap_size();
      printf("Free heap: %zu bytes\n", free_heap);
      fflush(stdout);
    } else if (s_console_buffer_pos > 0) {
      // Unknown command
      printf("Unknown command: %s\n", s_console_buffer);
      printf("Available commands:\n");
      printf("  load /sd/app.rb  - Load and run a script\n");
      printf("  heap             - Show free heap memory\n");
      printf("  restart          - Restart ESP32\n");
      fflush(stdout);
    }

    // Reset buffer and show prompt
    s_console_buffer_pos = 0;
    printf("> ");
    fflush(stdout);
    SET_NIL_RETURN();
    return;
  }

  // Handle printable characters
  if (c >= 32 && c < 127 && s_console_buffer_pos < sizeof(s_console_buffer) - 1) {
    // Echo character
    putchar(c);
    fflush(stdout);
    // Add to buffer
    s_console_buffer[s_console_buffer_pos++] = (char)c;
    SET_NIL_RETURN();
  } else if (s_console_buffer_pos >= sizeof(s_console_buffer) - 1) {
    // Buffer overflow, reset
    printf("\nBuffer overflow! Command too long.\n> ");
    fflush(stdout);
    s_console_buffer_pos = 0;
    SET_NIL_RETURN();
  } else {
    // Ignore other control characters
    SET_NIL_RETURN();
  }
}

static void
c_script_manager_cleanup_midi(mrbc_vm *vm, mrbc_value v[], int argc)
{
  (void)vm; (void)v; (void)argc;
  ESP_LOGI(TAG, "Ruby requested MIDI cleanup");
  picoruby_esp32_midi_cleanup();
  SET_NIL_RETURN();
}

static void
c_script_manager_free_heap(mrbc_vm *vm, mrbc_value v[], int argc)
{
  (void)v; (void)argc;
  // Return free heap size in bytes
  size_t free_heap = esp_get_free_heap_size();
  SET_INT_RETURN(free_heap);
}

static void
c_script_manager_esp_restart(mrbc_vm *vm, mrbc_value v[], int argc)
{
  (void)vm; (void)v; (void)argc;
  ESP_LOGI(TAG, "ESP32 restart requested by Ruby");
  // Give time for log to be printed
  vTaskDelay(pdMS_TO_TICKS(100));
  esp_restart();
  // Never returns
  SET_NIL_RETURN();
}

static void
c_script_manager_set_autorun(mrbc_vm *vm, mrbc_value v[], int argc)
{
  (void)vm;
  if (argc < 1 || v[1].tt != MRBC_TT_STRING) {
    SET_FALSE_RETURN();
    return;
  }

  const char *script_path = (const char *)v[1].string->data;
  nvs_handle_t handle;
  esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &handle);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "Failed to open NVS: %s", esp_err_to_name(err));
    SET_FALSE_RETURN();
    return;
  }

  err = nvs_set_str(handle, NVS_KEY_AUTORUN, script_path);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "Failed to set autorun: %s", esp_err_to_name(err));
    nvs_close(handle);
    SET_FALSE_RETURN();
    return;
  }

  err = nvs_commit(handle);
  nvs_close(handle);

  if (err != ESP_OK) {
    ESP_LOGE(TAG, "Failed to commit NVS: %s", esp_err_to_name(err));
    SET_FALSE_RETURN();
    return;
  }

  ESP_LOGI(TAG, "Autorun script set: %s", script_path);
  SET_TRUE_RETURN();
}

static void
c_script_manager_get_autorun(mrbc_vm *vm, mrbc_value v[], int argc)
{
  (void)v; (void)argc;

  nvs_handle_t handle;
  esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READONLY, &handle);
  if (err != ESP_OK) {
    SET_NIL_RETURN();
    return;
  }

  size_t required_size = 0;
  err = nvs_get_str(handle, NVS_KEY_AUTORUN, NULL, &required_size);
  if (err != ESP_OK || required_size == 0) {
    nvs_close(handle);
    SET_NIL_RETURN();
    return;
  }

  char *script_path = (char *)mrbc_raw_alloc(required_size);
  if (script_path == NULL) {
    nvs_close(handle);
    SET_NIL_RETURN();
    return;
  }

  err = nvs_get_str(handle, NVS_KEY_AUTORUN, script_path, &required_size);
  nvs_close(handle);

  if (err != ESP_OK) {
    mrbc_raw_free(script_path);
    SET_NIL_RETURN();
    return;
  }

  ESP_LOGI(TAG, "Autorun script found: %s", script_path);
  mrbc_value str = mrbc_string_new_cstr(vm, script_path);
  mrbc_raw_free(script_path);
  SET_RETURN(str);
}

static void
c_script_manager_clear_autorun(mrbc_vm *vm, mrbc_value v[], int argc)
{
  (void)vm; (void)v; (void)argc;

  nvs_handle_t handle;
  esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &handle);
  if (err != ESP_OK) {
    SET_FALSE_RETURN();
    return;
  }

  err = nvs_erase_key(handle, NVS_KEY_AUTORUN);
  if (err != ESP_OK && err != ESP_ERR_NVS_NOT_FOUND) {
    nvs_close(handle);
    SET_FALSE_RETURN();
    return;
  }

  nvs_commit(handle);
  nvs_close(handle);

  ESP_LOGI(TAG, "Autorun script cleared");
  SET_TRUE_RETURN();
}

static void
c_script_manager_sd_refresh_requested(mrbc_vm *vm, mrbc_value v[], int argc)
{
  (void)vm; (void)v; (void)argc;
  if (g_sd_refresh_requested) {
    SET_TRUE_RETURN();
  } else {
    SET_FALSE_RETURN();
  }
}

static void
c_script_manager_clear_sd_refresh(mrbc_vm *vm, mrbc_value v[], int argc)
{
  (void)vm; (void)v; (void)argc;
  g_sd_refresh_requested = false;
  ESP_LOGI(TAG, "SD refresh request cleared");
  SET_NIL_RETURN();
}
#endif

void
picoruby_esp32_clear_script_list(void)
{
  g_script_count = 0;
  g_script_list_ready = false;
  memset(g_script_list, 0, sizeof(g_script_list));
  ESP_LOGD(TAG, "Script list cleared");
}

bool
picoruby_esp32_add_script(const char *filename)
{
  if (filename == NULL || g_script_count >= PICORUBY_MAX_SCRIPTS) {
    return false;
  }

  strncpy(g_script_list[g_script_count], filename, PICORUBY_MAX_SCRIPT_NAME - 1);
  g_script_list[g_script_count][PICORUBY_MAX_SCRIPT_NAME - 1] = '\0';
  g_script_count++;
  ESP_LOGD(TAG, "Added script: %s (total: %d)", filename, g_script_count);
  return true;
}

int
picoruby_esp32_get_script_count(void)
{
  return g_script_count;
}

const char*
picoruby_esp32_get_script_name(int index)
{
  if (index < 0 || index >= g_script_count) {
    return NULL;
  }
  return g_script_list[index];
}

bool
picoruby_esp32_script_list_ready(void)
{
  return g_script_list_ready;
}

void
picoruby_esp32_set_script_list_ready(bool ready)
{
  g_script_list_ready = ready;
  ESP_LOGI(TAG, "Script list ready: %s (%d scripts)", ready ? "yes" : "no", g_script_count);
}

void
picoruby_esp32_request_sd_refresh(void)
{
  g_sd_refresh_requested = true;
  ESP_LOGI(TAG, "SD card refresh requested from UI");
}
