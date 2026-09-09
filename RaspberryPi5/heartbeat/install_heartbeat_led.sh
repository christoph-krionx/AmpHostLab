#!/bin/bash

set -e

CONFIG="/boot/firmware/config.txt"
OVERLAY="dtoverlay=gpio-led,gpio=17,label=statusled,trigger=heartbeat"

# Ensure the script is executed with root privileges
if [ "$EUID" -ne 0 ]; then
    echo "Please run this script with sudo."
    exit 1
fi

# Verify that the Raspberry Pi boot configuration exists
if [ ! -f "$CONFIG" ]; then
    echo "Configuration file not found: $CONFIG"
    exit 1
fi

# Create a timestamped backup before modifying the file
cp "$CONFIG" "${CONFIG}.bak.$(date +%Y%m%d-%H%M%S)"

# Add the overlay only if it does not already exist
if grep -Fxq "$OVERLAY" "$CONFIG"; then
    echo "Heartbeat LED overlay is already configured."
else
    echo "" >> "$CONFIG"
    echo "# Heartbeat LED on GPIO17" >> "$CONFIG"
    echo "$OVERLAY" >> "$CONFIG"
    echo "Heartbeat LED overlay added."
fi

echo
echo "Configuration completed."
echo "A reboot is required for the changes to take effect."

# Ask the user whether to reboot immediately
read -p "Reboot now? (y/N): " REBOOT

if [[ "$REBOOT" =~ ^[Yy]$ ]]; then
    reboot
fi