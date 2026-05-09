MRuby::CrossBuild.new("esp32") do |conf|
  conf.toolchain("gcc")

  conf.cc.command = "xtensa-esp32-elf-gcc"
  conf.linker.command = "xtensa-esp32-elf-ld"
  conf.archiver.command = "xtensa-esp32-elf-ar"

  conf.cc.host_command = "gcc"
  conf.cc.flags << "-Wall"
  conf.cc.flags << "-Wno-format"
  conf.cc.flags << "-Wno-unused-function"
  conf.cc.flags << "-Wno-maybe-uninitialized"
  conf.cc.flags << "-mlongcalls"

  conf.cc.defines << "MRBC_TICK_UNIT=10"
  conf.cc.defines << "MRBC_TIMESLICE_TICK_COUNT=1"
  conf.cc.defines << "MRBC_USE_FLOAT=2"
  conf.cc.defines << "MRBC_INT64"
  conf.cc.defines << "MRBC_CONVERT_CRLF=1"
  conf.cc.defines << "USE_FAT_FLASH_DISK"
  conf.cc.defines << "USE_FAT_SD_DISK"
  conf.cc.defines << "ESP32_PLATFORM"
  conf.cc.defines << "NDEBUG"
  # ESP32-S3 with PSRAM has enough memory for more symbols
  conf.cc.defines << "MAX_SYMBOLS_COUNT=2048"

  conf.femtoruby(alloc_libc: false)

  # Pull in FAT BEFORE the gemboxes so that gembox-time conditionals
  # (`if gems.any? { picoruby-filesystem-fat }`) see it and skip pulling
  # picoruby-littlefs as a dependency.
  conf.gem core: 'picoruby-filesystem-fat'
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
  conf.gem core: 'picoruby-sdmmc'
  conf.gem core: 'picoruby-adc'
  conf.gem core: 'picoruby-uart'
  conf.gem core: 'picoruby-pwm'

  # others
  conf.gem core: 'picoruby-esp32'
  conf.gem core: 'picoruby-rmt'
  conf.gem core: 'picoruby-mbedtls'
  conf.gem core: 'picoruby-socket'
  conf.gem core: 'picoruby-adafruit_sk6812'

  # MIDI
  conf.gem core: 'picoruby-usb_midi_host'
  conf.gem core: 'picoruby-uart_midi'
  conf.gem core: 'picoruby-sam2695'
  conf.gem core: 'picoruby-midi'
  conf.gem core: 'picoruby-midi-mml'

  # UI (M5Stack) — midori-specific gem, lives outside the picoruby submodule.
  conf.gem File.expand_path('../../../mrbgems/picoruby-ui', __dir__)
end
