#!/bin/bash

set -e

echo "=== Arduino CLI Setup für RP2040 ==="

# Arduino CLI installieren
echo "Installiere Arduino CLI..."
ARDUINO_CLI_VERSION="1.0.0"
cd ~
if [ ! -d "arduino-cli" ]; then
    wget https://downloads.arduino.cc/arduino-cli/arduino-cli_latest_Linux_64bit.tar.gz
    tar -xzf arduino-cli_latest_Linux_64bit.tar.gz
    sudo mv arduino-cli /usr/local/bin/
    rm arduino-cli_latest_Linux_64bit.tar.gz
else
    echo "Arduino CLI bereits installiert"
fi

# Arduino CLI initialisieren
arduino-cli config init --additional-urls https://github.com/earlephilhower/arduino-pico/releases/download/global/package_rp2040_index.json

# RP2040 Board Support installieren
echo "Installiere RP2040 Board Support..."
arduino-cli core update-index
arduino-cli core install rp2040:rp2040

# Libraries installieren
echo "Installiere Libraries..."
arduino-cli lib install "Sensirion I2C SGP40"
arduino-cli lib install "Sensirion I2C SCD4x"
arduino-cli lib install "Sensirion Gas Index Algorithm"
arduino-cli lib install "PacketSerial"
arduino-cli lib install "Grove - Multichannel Gas Sensor V2"
arduino-cli lib install "Grove - Laser PM2.5 HM3301"
arduino-cli lib install "Adafruit AHTX0"

echo ""
echo "=== Arduino CLI Setup abgeschlossen! ==="
echo ""
echo "Build & Flash:"
echo "  cd ~/sensecap-firmware/SenseCAP_Indicator_RP2040"
echo "  arduino-cli compile --fqbn rp2040:rp2040:rpipico indicator_rp2040.ino"
echo "  # BOOTSEL halten, USB verbinden"
echo "  arduino-cli upload --fqbn rp2040:rp2040:rpipico -p /dev/ttyACM0 indicator_rp2040.ino"
