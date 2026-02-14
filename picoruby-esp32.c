#include <inttypes.h>
#include <string.h>
#include <nvs_flash.h>
#include "esp_log.h"
#include "picoruby.h"
#include "picoruby-esp32.h"

static const char *TAG = "PICORUBY";

#if defined(CONFIG_IDF_TARGET_ESP32S3) && defined(CONFIG_SPIRAM)
#include "esp_attr.h"
#define HEAP_IN_PSRAM 1
#endif

#if defined(PICORB_VM_MRUBYC)
#include <mrubyc.h>
#elif defined(PICORB_VM_MRUBY)
#include "hal.h" // in picoruby-machine
#endif

#include "mrb/main_task.c"

#ifndef HEAP_SIZE
#if defined(CONFIG_IDF_TARGET_ESP32S3)
#define HEAP_SIZE (1024 * 180)
#elif defined(CONFIG_IDF_TARGET_ESP32C3)
#define HEAP_SIZE (1024 * 120)
#else
#define HEAP_SIZE (1024 * 120)
#endif
#endif

#if defined(HEAP_IN_PSRAM)
// Place heap in PSRAM for ESP32-S3 with external memory
EXT_RAM_BSS_ATTR static uint8_t heap_pool[HEAP_SIZE];
#else
static uint8_t heap_pool[HEAP_SIZE];
#endif

#if defined(PICORB_VM_MRUBY)
mrb_state *global_mrb = NULL;
#endif

// Track initialization state
static bool g_vm_initialized = false;

#if defined(PICORB_VM_MRUBYC)
static mrbc_vm *g_vm = NULL;
#endif

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

  picoruby_init_require(vm);
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
