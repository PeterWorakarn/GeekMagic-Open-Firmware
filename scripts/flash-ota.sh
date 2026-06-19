#!/bin/bash

set -e

FIRMWARE=".pio/build/esp12e/firmware.bin"
LITTLEFS=".pio/build/esp12e/littlefs.bin"

usage() {
  echo "Usage: $0 <device-ip> [--firmware-only | --fs-only]"
  echo ""
  echo "  <device-ip>      IP address of the GeekMagic device"
  echo "  --firmware-only  Flash only firmware.bin"
  echo "  --fs-only        Flash only littlefs.bin (via AP 192.168.4.1)"
  echo ""
  echo "Examples:"
  echo "  $0 192.168.1.42"
  echo "  $0 192.168.1.42 --firmware-only"
  echo "  $0 192.168.1.42 --fs-only"
  exit 1
}

if [ -z "$1" ]; then
  usage
fi

DEVICE_IP="$1"
MODE="${2:-}"

flash_firmware() {
  if [ ! -f "$FIRMWARE" ]; then
    echo "Error: $FIRMWARE not found. Run 'pio run' first."
    exit 1
  fi

  echo "Uploading firmware.bin to http://$DEVICE_IP/update ..."
  HTTP_STATUS=$(curl -s -o /dev/null -w "%{http_code}" \
    -X POST "http://$DEVICE_IP/update" \
    -F "file=@$FIRMWARE")

  if [ "$HTTP_STATUS" = "200" ]; then
    echo "firmware.bin uploaded successfully. Device is rebooting..."
  else
    echo "Error: Upload failed (HTTP $HTTP_STATUS)"
    exit 1
  fi
}

flash_filesystem() {
  if [ ! -f "$LITTLEFS" ]; then
    echo "Error: $LITTLEFS not found. Run 'pio run --target buildfs' first."
    exit 1
  fi

  echo "Uploading littlefs.bin to http://192.168.4.1/legacyupdate ..."
  echo "Make sure you are connected to the 'GeekMagic' WiFi AP (password: \$str0ngPa\$\$w0rd)"
  echo ""
  read -r -p "Press Enter when connected to GeekMagic AP..."

  HTTP_STATUS=$(curl -s -o /dev/null -w "%{http_code}" \
    -X POST "http://192.168.4.1/legacyupdate" \
    -F "file=@$LITTLEFS")

  if [ "$HTTP_STATUS" = "200" ]; then
    echo "littlefs.bin uploaded successfully. Device is rebooting..."
    echo "Setup complete! Device will connect to your WiFi."
  else
    echo "Error: Upload failed (HTTP $HTTP_STATUS)"
    exit 1
  fi
}

case "$MODE" in
  --firmware-only)
    flash_firmware
    ;;
  --fs-only)
    flash_filesystem
    ;;
  *)
    flash_firmware
    echo ""
    echo "Waiting 10 seconds for device to reboot..."
    sleep 10
    flash_filesystem
    ;;
esac
