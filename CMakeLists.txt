set(PICORB_VM "mrubyc" CACHE STRING "Select build mode (mrubyc or mruby)")
message(STATUS "PICORB_VM is set to: ${PICORB_VM}")

file(MAKE_DIRECTORY ${COMPONENT_DIR}/picoruby/mrbgems/picoruby-mbedtls/lib/mbedtls/include)

if(${PICORB_VM} STREQUAL "mruby")
  file(MAKE_DIRECTORY ${COMPONENT_DIR}/picoruby/build/esp32-microruby/include)
  set(MRUBY_CONFIG_STRING "${COMPONENT_DIR}/build_config/${IDF_TARGET_ARCH}-esp-microruby.rb")
  set(LIBMRUBY_PATH "esp32-microruby")
  set(CUSTOM_TARGET_NAME "microruby")
  set(ADDITIONAL_INCLUDE_DIRS
    ${COMPONENT_DIR}/picoruby/build/esp32-microruby/include
    ${COMPONENT_DIR}/picoruby/mrbgems/picoruby-mruby/lib/mruby/include
  )
  set(ADDITIONAL_DEFINITIONS
    -DMRB_TICK_UNIT=10
    -DMRB_TIMESLICE_TICK_COUNT=1
    -DMRB_INT64=1
    -DPICORB_VM_MRUBY
  )
else()
  file(MAKE_DIRECTORY ${COMPONENT_DIR}/picoruby/build/esp32/mrbgems)
  set(MRUBY_CONFIG_STRING "${COMPONENT_DIR}/build_config/${IDF_TARGET_ARCH}-esp.rb")
  set(LIBMRUBY_PATH "esp32")
  set(CUSTOM_TARGET_NAME "picoruby")
  set(ADDITIONAL_INCLUDE_DIRS
    ${COMPONENT_DIR}/picoruby/build/esp32/mrbgems
  )
  set(ADDITIONAL_DEFINITIONS
    -DMRBC_TICK_UNIT=10
    -DMRBC_TIMESLICE_TICK_COUNT=1
    -DMRBC_USE_FLOAT=2
    -DMRBC_INT64
    -DMRC_CUSTOM_ALLOC
    -DPICORB_VM_MRUBYC
  )
endif()

idf_component_register(
  SRCS
    ${COMPONENT_DIR}/picoruby-esp32.c
    ${COMPONENT_DIR}/picoruby_supervisor.c
    ${COMPONENT_DIR}/picoruby/mrbgems/picoruby-esp32/ports/esp32.c
    ${COMPONENT_DIR}/picoruby/mrbgems/picoruby-machine/ports/esp32/machine.c
    ${COMPONENT_DIR}/picoruby/mrbgems/picoruby-machine/ports/common/machine.c
    ${COMPONENT_DIR}/picoruby/mrbgems/picoruby-io-console/ports/esp32/io-console.c
    ${COMPONENT_DIR}/picoruby/mrbgems/picoruby-filesystem-fat/ports/esp32/flash_disk.c
    ${COMPONENT_DIR}/picoruby/mrbgems/picoruby-filesystem-fat/ports/esp32/sd_disk.c
    ${COMPONENT_DIR}/picoruby/mrbgems/picoruby-env/ports/esp32/env.c
    ${COMPONENT_DIR}/picoruby/mrbgems/picoruby-gpio/ports/esp32/gpio.c
    ${COMPONENT_DIR}/picoruby/mrbgems/picoruby-adc/ports/esp32/adc.c
    ${COMPONENT_DIR}/picoruby/mrbgems/picoruby-spi/ports/esp32/spi.c
    ${COMPONENT_DIR}/picoruby/mrbgems/picoruby-rng/ports/esp32/rng.c
    ${COMPONENT_DIR}/picoruby/mrbgems/picoruby-pwm/ports/esp32/pwm.c
    ${COMPONENT_DIR}/picoruby/mrbgems/picoruby-rmt/ports/esp32/rmt.c
    ${COMPONENT_DIR}/picoruby/mrbgems/picoruby-watchdog/ports/esp32/watchdog.c
    ${COMPONENT_DIR}/picoruby/mrbgems/picoruby-mbedtls/ports/esp32/timing_alt.c
    ${COMPONENT_DIR}/picoruby/mrbgems/picoruby-mbedtls/ports/common/cipher.c
    ${COMPONENT_DIR}/picoruby/mrbgems/picoruby-mbedtls/ports/common/cmac.c
    ${COMPONENT_DIR}/picoruby/mrbgems/picoruby-mbedtls/ports/common/digest.c
    ${COMPONENT_DIR}/picoruby/mrbgems/picoruby-mbedtls/ports/common/hmac.c
    ${COMPONENT_DIR}/picoruby/mrbgems/picoruby-mbedtls/ports/common/pkey.c
    ${COMPONENT_DIR}/picoruby/mrbgems/picoruby-mbedtls/ports/common/md.c
    ${COMPONENT_DIR}/picoruby/mrbgems/picoruby-socket/ports/esp32/net_helpers.c
    ${COMPONENT_DIR}/picoruby/mrbgems/picoruby-socket/ports/esp32/ssl_socket.c
    ${COMPONENT_DIR}/picoruby/mrbgems/picoruby-socket/ports/esp32/tcp_server.c
    ${COMPONENT_DIR}/picoruby/mrbgems/picoruby-socket/ports/esp32/tcp_socket.c
    ${COMPONENT_DIR}/picoruby/mrbgems/picoruby-socket/ports/esp32/udp_socket.c
    ${COMPONENT_DIR}/picoruby/mrbgems/picoruby-i2c/ports/esp32/i2c.c
    ${COMPONENT_DIR}/picoruby/mrbgems/picoruby-uart/ports/esp32/uart.c
    ${COMPONENT_DIR}/picoruby/mrbgems/picoruby-sdmmc/src/sdmmc.c
    ${COMPONENT_DIR}/picoruby/mrbgems/picoruby-usb_midi/ports/esp32/usb_midi.c
    ${COMPONENT_DIR}/picoruby/mrbgems/picoruby-sam2695/ports/esp32/sam2695.c
    ${COMPONENT_DIR}/picoruby/mrbgems/picoruby-midi/ports/esp32/midi.c
    ${COMPONENT_DIR}/picoruby/mrbgems/picoruby-ui/ports/esp32/ui.c
  INCLUDE_DIRS
    ${COMPONENT_DIR}
    ${COMPONENT_DIR}/picoruby/include
    ${COMPONENT_DIR}/picoruby/mrbgems/picoruby-mrubyc/lib/mrubyc/src
    ${COMPONENT_DIR}/picoruby/mrbgems/picoruby-machine/include
    ${COMPONENT_DIR}/picoruby/mrbgems/picoruby-mbedtls/include
    ${COMPONENT_DIR}/picoruby/mrbgems/picoruby-mbedtls/lib/mbedtls/include
    ${COMPONENT_DIR}/picoruby/mrbgems/picoruby-filesystem-fat/ports/esp32
    ${COMPONENT_DIR}/picoruby/mrbgems/picoruby-filesystem-fat/include
    ${COMPONENT_DIR}/picoruby/mrbgems/picoruby-filesystem-fat/src/hal
    ${COMPONENT_DIR}/picoruby/mrbgems/mruby-compiler2/include
    ${COMPONENT_DIR}/picoruby/mrbgems/mruby-compiler2/lib/prism/include
    ${COMPONENT_DIR}/picoruby/mrbgems/picoruby-sdmmc/include
    ${COMPONENT_DIR}/picoruby/mrbgems/picoruby-usb_midi/include
    ${COMPONENT_DIR}/picoruby/mrbgems/picoruby-sam2695/include
    ${COMPONENT_DIR}/picoruby/mrbgems/picoruby-midi/include
    ${COMPONENT_DIR}/picoruby/mrbgems/picoruby-ui/include
    ${CMAKE_SOURCE_DIR}/main/ui
    ${ADDITIONAL_INCLUDE_DIRS}
  PRIV_REQUIRES
    esp_driver_gpio
    esp_driver_uart
    esp_driver_spi
    esp_driver_sdmmc
    esp_driver_ledc
    esp_driver_rmt
    esp_driver_i2c
    esp_adc
    esp_timer
    esp_hw_support
    spi_flash
    nvs_flash
    esp_wifi
)

add_definitions(
  ${ADDITIONAL_DEFINITIONS}
  -DMRBC_CONVERT_CRLF=1
  -DUSE_FAT_FLASH_DISK
  -DUSE_FAT_SD_DISK
  -DESP32_PLATFORM
  -DNDEBUG
)

set(PICORUBY_DIR ${COMPONENT_DIR}/picoruby)
set(LIBMRUBY_FILE ${PICORUBY_DIR}/build/${LIBMRUBY_PATH}/lib/libmruby.a)

# Build PicoRuby
add_custom_command(
  OUTPUT ${LIBMRUBY_FILE}
  COMMAND ${CMAKE_COMMAND} -E echo "MRUBY_CONFIG=${MRUBY_CONFIG_STRING} rake"
  COMMAND ${CMAKE_COMMAND} -E env MRUBY_CONFIG=${MRUBY_CONFIG_STRING} rake
  WORKING_DIRECTORY ${PICORUBY_DIR}
  COMMENT "PicoRuby build"
  VERBATIM
)

add_prebuilt_library(
  libmruby ${LIBMRUBY_FILE}
  REQUIRES ${COMPONENT_NAME}
)
target_link_libraries(${COMPONENT_LIB} PRIVATE libmruby)
target_include_directories(
  ${COMPONENT_LIB}
  PRIVATE
    ${COMPONENT_DIR}/picoruby/mrbgems/mruby-compiler2/include
    ${COMPONENT_DIR}/picoruby/mrbgems/mruby-compiler2/lib/prism/include
    ${ADDITIONAL_INCLUDE_DIRS}
)

add_custom_target(
  ${CUSTOM_TARGET_NAME} DEPENDS ${LIBMRUBY_FILE}
)
add_dependencies(${COMPONENT_LIB} ${CUSTOM_TARGET_NAME})

# Generate board_config.rb from template based on Kconfig settings
if(CONFIG_USB_MIDI_BOARD_M5STACK_CORES3 OR CONFIG_USB_MIDI_BOARD_M5STACK_CORES3_USB_SERIAL)
  if(CONFIG_USB_MIDI_BOARD_M5STACK_CORES3_USB_SERIAL)
    set(BOARD_NAME "M5Stack CoreS3 SE (USB Serial)")
  else()
    set(BOARD_NAME "M5Stack CoreS3 SE")
  endif()
  set(SD_MODE "spi")
  # SPI pins for M5Stack
  set(SD_SCK_PIN 36)
  set(SD_MISO_PIN 35)
  set(SD_MOSI_PIN 37)
  set(SD_CS_PIN 4)
  # SDMMC pins (not used for M5Stack, set to -1)
  set(SD_CLK_PIN -1)
  set(SD_CMD_PIN -1)
  set(SD_D0_PIN -1)
else()
  # Default to Freenove - SDMMC mode (requires PSRAM disabled to avoid GPIO conflict)
  set(BOARD_NAME "Freenove ESP32-S3")
  set(SD_MODE "sdmmc")
  # SDMMC pins for Freenove (from official documentation)
  set(SD_CLK_PIN 39)
  set(SD_CMD_PIN 38)
  set(SD_D0_PIN 40)
  # SPI pins (not used for Freenove, set to -1)
  set(SD_SCK_PIN -1)
  set(SD_MISO_PIN -1)
  set(SD_MOSI_PIN -1)
  set(SD_CS_PIN -1)
endif()

configure_file(
  ${COMPONENT_DIR}/mrblib/board_config.rb.in
  ${COMPONENT_DIR}/mrblib/board_config.rb
  @ONLY
)

# Concatenate board_config.rb and main_task_base.rb into main_task.rb
# This ensures BoardConfig module is defined before main_task uses it
file(READ ${COMPONENT_DIR}/mrblib/board_config.rb BOARD_CONFIG_CONTENT)
file(READ ${COMPONENT_DIR}/mrblib/main_task_base.rb MAIN_TASK_CONTENT)
file(WRITE ${COMPONENT_DIR}/mrblib/main_task.rb "${BOARD_CONFIG_CONTENT}\n${MAIN_TASK_CONTENT}")

# Compile Ruby files
set(RUBY_FILES main_task)
set(PICORBC ${COMPONENT_DIR}/picoruby/bin/picorbc)
set(GENERATED_C_FILES "")

foreach(rb ${RUBY_FILES})
  set(C_FILE ${COMPONENT_DIR}/mrb/${rb}.c)
  set(RB_FILE ${COMPONENT_DIR}/mrblib/${rb}.rb)
  add_custom_command(
    OUTPUT ${C_FILE}
    COMMAND ${CMAKE_COMMAND} -E echo "Compiling ${RB_FILE}..."
    COMMAND ${CMAKE_COMMAND} -E make_directory ${COMPONENT_DIR}/mrb
    COMMAND ${PICORBC} -B${rb} -o${C_FILE} ${RB_FILE}
    DEPENDS ${CUSTOM_TARGET_NAME} ${RB_FILE}
    COMMENT "Compiling ${RB_FILE}"
    VERBATIM
  )
  list(APPEND GENERATED_C_FILES ${C_FILE})
endforeach(rb)

target_sources(${COMPONENT_LIB} PRIVATE ${GENERATED_C_FILES})
