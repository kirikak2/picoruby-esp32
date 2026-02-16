require 'machine'
require "watchdog"
Watchdog.disable
require "shell"
# BoardConfig is defined above (concatenated by CMake)
STDIN = IO.new
STDOUT = IO.new

puts "Board: #{BoardConfig::BOARD_NAME}"

# Setup flash disk
begin
  STDIN.echo = false
  puts "Initializing FLASH disk as the root volume... "
  Shell.setup_root_volume(:flash, label: 'storage')
  Shell.setup_system_files
  puts "Available"
rescue => e
  puts "Not available"
  puts "#{e.message} (#{e.class})"
end

# Setup SD card
if BoardConfig::SD_MODE == "none"
  puts "SD card disabled (GPIO conflict with PSRAM)"
else
  begin
    puts "Initializing SD card (#{BoardConfig::SD_MODE} mode)..."
    if BoardConfig::SD_MODE == "sdmmc"
      # SDMMC mode
      puts "  CLK=#{BoardConfig::SD_CLK_PIN}, CMD=#{BoardConfig::SD_CMD_PIN}, D0=#{BoardConfig::SD_D0_PIN}"
      Shell.setup_sdcard_sdmmc(
        BoardConfig::SD_CLK_PIN,
        BoardConfig::SD_CMD_PIN,
        BoardConfig::SD_D0_PIN
      )
    else
      # SPI mode (M5Stack)
      require "spi"
      puts "  SCK=#{BoardConfig::SD_SCK_PIN}, MISO=#{BoardConfig::SD_MISO_PIN}, MOSI=#{BoardConfig::SD_MOSI_PIN}, CS=#{BoardConfig::SD_CS_PIN}"
      spi = SPI.new(
        frequency: 5_000_000,
        unit: BoardConfig::SD_SPI_UNIT,
        sck_pin:  BoardConfig::SD_SCK_PIN,
        cipo_pin: BoardConfig::SD_MISO_PIN,
        copi_pin: BoardConfig::SD_MOSI_PIN,
        cs_pin:   BoardConfig::SD_CS_PIN
      )
      Shell.setup_sdcard(spi)
    end
  rescue => e
    puts "SD card not available: #{e.message}"
  end
end

GC.start

begin
  if File.exist?("/home/app.mrb")
    puts "Loading app.mrb"
    load "/home/app.mrb"
  elsif File.exist?("/home/app.rb")
    puts "Loading app.rb"
    load "/home/app.rb"
  elsif File.exist?("/sd/app.rb")
    puts "  Loading /sd/app.rb..."
    load "/sd/app.rb"
  end

  GC.start

  # $shell = Shell.new(clean: true)
  # puts "Starting shell...\n\n"

  # $shell.show_logo
  # $shell.start
rescue => e
  puts "#{e.message} (#{e.class})"
end
