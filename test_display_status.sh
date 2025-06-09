#!/bin/bash

echo "=== Display Connection Status Test ==="
echo "Date: $(date)"
echo

echo "1. HDMI Status:"
if [ -f "/sys/class/drm/card0-HDMI-A-1/status" ]; then
    HDMI_STATUS=$(cat /sys/class/drm/card0-HDMI-A-1/status)
    echo "   card0-HDMI-A-1: $HDMI_STATUS"
else
    echo "   HDMI status file not found"
fi

echo
echo "2. DisplayPort Status:"
if [ -f "/sys/class/drm/card0-DP-1/status" ]; then
    DP_STATUS=$(cat /sys/class/drm/card0-DP-1/status)
    echo "   card0-DP-1: $DP_STATUS"
else
    echo "   DP status file not found"
fi

echo
echo "3. DSI Status:"
if [ -f "/sys/class/drm/card0-DSI-1/status" ]; then
    DSI_STATUS=$(cat /sys/class/drm/card0-DSI-1/status)
    echo "   card0-DSI-1: $DSI_STATUS"
else
    echo "   DSI status file not found"
fi

echo
echo "4. All DRM connectors:"
find /sys/class/drm -name "card0-*" -type d | sort | while read connector; do
    name=$(basename "$connector")
    if [ -f "$connector/status" ]; then
        status=$(cat "$connector/status")
        echo "   $name: $status"
    fi
done

echo
echo "=== End Test ===" 