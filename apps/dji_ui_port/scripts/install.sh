#!/bin/sh
# Install to /opt/dji_ui on the board and register a systemd unit.
set -e
[ "$(id -u)" = "0" ] || { echo "run as root on the board"; exit 1; }
SRC=/opt/dji_ui
mkdir -p "$SRC/lib"
cp "$(dirname "$0")/../build/dji_ui" "$SRC/dji_ui"
chmod +x "$SRC/dji_ui"
# runtime deps not present on MaixCAM2 rootfs
for f in libdatachannel.so libtinyalsa.so.2 libsamplerate.so; do
    [ -f "/root/lib/$f" ] && cp "/root/lib/$f" "$SRC/lib/" || true
done
[ -f /root/DroidSansFallbackFull.ttf ] && cp /root/DroidSansFallbackFull.ttf "$SRC/" || true

cat > /etc/systemd/system/dji_ui.service <<UNIT
[Unit]
Description=MaixCAM2 action camera (LVGL)
After=multi-user.target
Conflicts=launcher.service

[Service]
Type=simple
WorkingDirectory=$SRC
Environment=LD_LIBRARY_PATH=$SRC/lib:/opt/lib
TimeoutStartSec=0
ExecStart=$SRC/dji_ui
Restart=always
RestartSec=3

[Install]
WantedBy=multi-user.target
UNIT

systemctl daemon-reload
systemctl disable launcher.service maixvision-server.service || true
systemctl enable dji_ui.service
systemctl start dji_ui.service
