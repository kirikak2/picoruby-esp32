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

#if (defined(CONFIG_IDF_TARGET_ESP32S3) || defined(CONFIG_IDF_TARGET_ESP32P4)) && defined(CONFIG_SPIRAM)
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

#ifndef HEAP_SIZE
#if defined(CONFIG_IDF_TARGET_ESP32P4) && defined(CONFIG_SPIRAM)
// ESP32-P4 with PSRAM (Tab5 has 32MB): Use 4MB heap
#define HEAP_SIZE (1024 * 1024 * 4)
#elif defined(CONFIG_IDF_TARGET_ESP32S3) && defined(CONFIG_SPIRAM)
// ESP32-S3 with PSRAM: Use 2MB heap (plenty of room in 8MB PSRAM)
#define HEAP_SIZE (1024 * 1024 * 2)
#elif defined(CONFIG_IDF_TARGET_ESP32S3)
// ESP32-S3 without PSRAM: Conservative heap
#define HEAP_SIZE (1024 * 180)
#elif defined(CONFIG_IDF_TARGET_ESP32P4)
// ESP32-P4 without PSRAM: Conservative heap
#define HEAP_SIZE (1024 * 256)
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
  extern int USB_MIDI_HOST_send_packet(uint8_t cable, uint8_t cin, uint8_t midi1, uint8_t midi2, uint8_t midi3);
  extern int UART_MIDI_send_packet(uint8_t cable, uint8_t cin, uint8_t midi1, uint8_t midi2, uint8_t midi3);

  // Send cleanup messages to all MIDI channels (0-15)
  for (uint8_t ch = 0; ch < 16; ch++) {
    uint8_t status_cc = 0xB0 | ch;  // Control Change

    // All Sound Off (CC #120)
    USB_MIDI_HOST_send_packet(0, 0x0B, status_cc, 120, 0);
    UART_MIDI_send_packet(0, 0x0B, status_cc, 120, 0);

    // All Notes Off (CC #123)
    USB_MIDI_HOST_send_packet(0, 0x0B, status_cc, 123, 0);
    UART_MIDI_send_packet(0, 0x0B, status_cc, 123, 0);
  }

  // Send MIDI Stop (0xFC)
  USB_MIDI_HOST_send_packet(0, 0x05, 0xFC, 0, 0);  // CIN 0x05 for single-byte system common
  UART_MIDI_send_packet(0, 0x05, 0xFC, 0, 0);

  ESP_LOGI(TAG, "MIDI cleanup completed");
}

// Script list management
static char g_script_list[PICORUBY_MAX_SCRIPTS][PICORUBY_MAX_SCRIPT_NAME];
static int g_script_count = 0;
static volatile bool g_script_list_ready = false;
static volatile uint32_t g_script_list_version = 0;

// SD card refresh request (from UI to Ruby)
// Note: Not static, accessible from picoruby_supervisor.c
volatile bool g_sd_refresh_requested = false;

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
  if (ready) {
    g_script_list_version++;
  }
  ESP_LOGI(TAG, "Script list ready: %s (%d scripts, version=%lu)", ready ? "yes" : "no", g_script_count, (unsigned long)g_script_list_version);
}

uint32_t
picoruby_esp32_get_script_list_version(void)
{
  return g_script_list_version;
}

void
picoruby_esp32_request_sd_refresh(void)
{
  g_sd_refresh_requested = true;
  ESP_LOGI(TAG, "SD card refresh requested from UI");
}
