MRuby::CrossBuild.new("esp32") do |conf|
  conf.toolchain("gcc")

  conf.cc.command = "riscv32-esp-elf-gcc"
  conf.linker.command = "riscv32-esp-elf-ld"
  conf.archiver.command = "riscv32-esp-elf-ar"

  conf.cc.host_command = "gcc"
  # ESP32-P4 uses RISC-V with single-precision float ABI
  conf.cc.flags << "-march=rv32imafc_zicsr_zifencei"
  conf.cc.flags << "-mabi=ilp32f"
  conf.cc.flags << "-Wall"
  conf.cc.flags << "-Wno-format"
  conf.cc.flags << "-Wno-unused-function"
  conf.cc.flags << "-Wno-maybe-uninitialized"

  conf.cc.defines << "MRBC_TICK_UNIT=10"
  conf.cc.defines << "MRBC_TIMESLICE_TICK_COUNT=1"
  conf.cc.defines << "MRBC_USE_FLOAT=2"
  conf.cc.defines << "MRBC_INT64"
  conf.cc.defines << "MRBC_CONVERT_CRLF=1"
  conf.cc.defines << "USE_FAT_FLASH_DISK"
  conf.cc.defines << "USE_FAT_SD_DISK"
  conf.cc.defines << "ESP32_PLATFORM"
  conf.cc.defines << "NDEBUG"
  # ESP32-P4 with PSRAM has enough memory for more symbols
  conf.cc.defines << "MAX_SYMBOLS_COUNT=2048"

  conf.picoruby(alloc_libc: false)
  conf.gembox 'minimum'
  conf.gembox 'core'
  conf.gembox 'shell'

  # stdlib
  conf.gem core: 'picoruby-rng'
  conf.gem core: 'picoruby-base64'
  conf.gem core: 'picoruby-yaml'

  # peripherals
  conf.gem core: 'picoruby-gpio'
  conf.gem core: 'picoruby-i2c'
  conf.gem core: 'picoruby-spi'
  conf.gem core: 'picoruby-adc'
  conf.gem core: 'picoruby-uart'
  conf.gem core: 'picoruby-pwm'

  # others
  conf.gem core: 'picoruby-esp32'
  conf.gem core: 'picoruby-rmt'
  conf.gem core: 'picoruby-mbedtls'
  conf.gem core: 'picoruby-socket'
  conf.gem core: 'picoruby-mqtt'
  conf.gem core: 'picoruby-adafruit_sk6812'

  # MIDI
  conf.gem core: 'picoruby-usb_midi'
  conf.gem core: 'picoruby-sam2695'
  conf.gem core: 'picoruby-midi'

  # UI (M5Stack)
  conf.gem core: 'picoruby-ui'
end
