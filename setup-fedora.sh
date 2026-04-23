#!/bin/bash

set -e

echo "=== SenseCAP Indicator D1S Flash Setup für Fedora 43 ==="

# System-Updates
echo "1. System-Updates..."
sudo dnf update -y

# Entwicklungstools installieren
echo "2. Installiere Entwicklungstools..."
sudo dnf install -y git python3 python3-pip cmake ninja-build gcc g++ make flex bison gperf \
    ncurses-devel libffi-devel libssl-devel dfu-util libusb1 wget unzip

# USB-Regeln für ESP32
echo "3. Konfiguriere USB-Regeln..."
sudo tee /etc/udev/rules.d/99-espressif.rules > /dev/null <<'EOF'
SUBSYSTEM=="usb", ATTR{idVendor}=="0403", MODE="0666"
SUBSYSTEM=="usb", ATTR{idVendor}=="303a", MODE="0666"
SUBSYSTEM=="usb", ATTR{idVendor}=="239a", MODE="0666"
EOF
sudo udevadm control --reload-rules

# ESP-IDF Setup
echo "4. Installiere ESP-IDF v5.1..."
cd ~
if [ ! -d "esp-idf" ]; then
    git clone --recursive https://github.com/espressif/esp-idf.git
    cd esp-idf
    git checkout v5.1
    git submodule update --init --recursive
    ./install.sh
else
    echo "ESP-IDF bereits installiert, wird übersprungen."
fi

echo ""
echo "=== Installation abgeschlossen! ==="
echo ""
echo "Nächste Schritte:"
echo ""
echo "Für ESP32 (nicht vergessen: PSRAM Octal 120M Patch anwenden!):"
echo "  source ~/esp-idf/export.sh"
echo "  cd ~/sensecap-firmware/SenseCAP_Indicator_ESP32"
echo "  idf.py set-target esp32s3"
echo "  idf.py build"
echo "  idf.py -p /dev/ttyACM0 flash monitor"
echo ""
echo "Für RP2040:"
echo "  cd ~/sensecap-firmware/SenseCAP_Indicator_RP2040"
echo "  BOOTSEL Taste halten, USB verbinden und .uf2 Datei auf RPI-RP2 Drive kopieren"
echo ""
