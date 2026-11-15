#!/bin/sh
# give ISP/camera kernel modules a moment after boot before opening the sensor
sleep 6
exec /opt/dji_ui/dji_ui
