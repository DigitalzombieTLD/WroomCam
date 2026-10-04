# WroomCam

ESP32-S3 firmware that acts as a **USB host for a UVC camera** (MJPEG) and serves it as a
`multipart/x-mixed-replace` MJPEG stream over Wi-Fi.

> **Status:** written against the ESP-IDF / `espressif/usb_host_uvc` APIs but **not yet compiled and not
> tested on hardware** (the authoring environment had no ESP-IDF toolchain or board). Expect to fix small
> build issues on first compile; see [Validation](#validation).

## Hardware

- Board: ESP32-S3-DevKitC-1 v1.1 (ESP32-S3-WROOM-1). The native USB pins are **GPIO19 (D-) and GPIO20 (D+)**.
  The board has two USB-C connectors: one goes to the USB-to-UART bridge (flashing/serial log), the other
  to the ESP32-S3 native USB. Check the silkscreen/the
  [official user guide](https://docs.espressif.com/projects/esp-dev-kits/en/latest/esp32s3/esp32-s3-devkitc-1/user_guide_v1.1.html),
  because connector labels differ between documents/revisions. **The camera goes on the native USB port.**
- **Host power (VBUS):** the official guide does not document the board's USB port as a 5 V *output* in host
  mode, so do not assume it supplies the camera (that page could not be fetched while writing this, so
  verify against the schematic). Use a proper USB-OTG/host adapter or cable and an **externally powered USB
  hub**, or another safe 5 V source (shared ground) for the camera's VBUS. **Never power the camera from a GPIO.**
  The camera in question draws about 128 mA.
- Flashing/logs: use the UART port; run the camera from the native port. The native port can power the board
  but you then cannot use it for both host and a PC at once.
- **PSRAM:** DevKitC-1 variants differ (N8R8/N16R8 have octal PSRAM, R2 has quad, N4/N8 none). Defaults assume
  octal PSRAM; boot continues without PSRAM (`CONFIG_SPIRAM_IGNORE_NOTFOUND`) and the firmware then uses
  internal RAM and falls back to 320x240. For quad PSRAM, replace `CONFIG_SPIRAM_MODE_OCT=y` with
  `CONFIG_SPIRAM_MODE_QUAD=y` in `sdkconfig.defaults`. Flash is set to 4 MB (works on every variant).

## Performance expectations

The ESP32-S3 USB peripheral is **Full-Speed (12 Mbit/s)**; usable isochronous bandwidth is well under 1 MB/s,
Espressif's UVC example reports around 0.5 MB/s. A camera may advertise 2048x1536 MJPEG, but that is
a High-Speed USB 2.0 mode and will generally not work (or will not stream) on the S3. Start with 320x240 or
640x480 @ 15 fps. Many cameras only expose Full-Speed alternate settings for small modes; if
`stream_start failed` appears, the mode is not available at Full-Speed. Wi-Fi throughput, JPEG size and
camera exposure also limit frame rate.

## Build

Requires ESP-IDF **v5.4 or v5.5** (`main/idf_component.yml` pins `>=5.4.0,<5.6.0`) and
`espressif/usb_host_uvc ~2.5.2` (downloaded automatically by the component manager; commit the generated
`dependencies.lock` to pin exact versions).

```sh
. $IDF_PATH/export.sh
cp sdkconfig.secrets.example sdkconfig.secrets   # edit SSID / password (git-ignored)
idf.py set-target esp32s3
idf.py build
idf.py -p <UART-PORT> flash monitor
```

If you change `sdkconfig.defaults` / `sdkconfig.secrets` later, delete the generated `sdkconfig` (or use
`idf.py menuconfig` -> *WroomCam*).

## Configuration (menuconfig -> WroomCam, or `sdkconfig.secrets`)

| Setting | Default |
|---|---|
| `WROOMCAM_WIFI_SSID` / `_PASSWORD` | placeholders `YOUR_WIFI_SSID` / `YOUR_WIFI_PASSWORD` |
| `WROOMCAM_STATIC_IP` + `_IP_ADDR`, `_IP_GATEWAY`, `_IP_NETMASK`, `_IP_DNS` | off (DHCP) |
| `WROOMCAM_CAM_WIDTH` / `_HEIGHT` / `_FPS` | 320 / 240 / 15, MJPEG |
| `WROOMCAM_CAM_FRAME_BUF_KB` | 0 (auto, at least 32 KiB) |
| `WROOMCAM_STALL_TIMEOUT_MS`, `_MAX_FAILURES`, `_BACKOFF_MIN_MS`, `_BACKOFF_MAX_MS` | 5000, 8, 1000, 15000 |
| `WROOMCAM_HTTP_PORT`, `_HTTP_SEND_TIMEOUT_S`, `_HTTP_FRAME_WAIT_S` | 80, 5, 15 |

Increasing resolution: raise width/height/fps in steps (320x240 -> 640x480 -> 800x600 ...), keep the
mode to one your camera lists as MJPEG, watch the log for `overflow`/`transfer error`, and raise
`WROOMCAM_CAM_FRAME_BUF_KB` if frames overflow. Requires PSRAM above 320x240.

## Use

Open `http://<device-ip>/` (page embeds the stream) or `http://<device-ip>/stream`. The IP is printed in the
serial log (`Got IP ...`). Streaming runs only while a viewer is connected. One viewer at a time: the
ESP-IDF HTTP server handles a single stream handler, other connections wait.

## Design notes

- `uvc_source.c`: installs USB host + `espressif/usb_host_uvc`; frames are handed to HTTP **without copying**
  (driver buffers, PSRAM if present) through a one-slot queue. A slow client causes the old frame to be dropped
  and the newest kept, so buffering is bounded (3 driver buffers).
- A supervisor task recovers from camera unplug, USB transfer-error bursts, stalls (no frames) and
  open/start failures by stopping/closing/reopening the stream with exponential backoff. After
  `WROOMCAM_MAX_FAILURES` consecutive failures (or if frames cannot be released) the chip restarts.
  Panics and task-watchdog timeouts also reboot. An absent camera is waited for indefinitely, not counted as failure.
- `wifi_manager.c`: station mode, DHCP or static IPv4, backoff reconnect, restart after many failures.
- Bluetooth is disabled and SoftAP/enterprise Wi-Fi support are off in `sdkconfig.defaults`.

## Validation

No automated tests are included (they need hardware). After flashing:

1. Serial log should show `UVC device connected`, `Mode: MJPEG ...`, `Streaming started`.
2. `python3 tools/check_stream.py http://<device-ip>/stream 50` checks boundaries, Content-Length and JPEG markers and prints fps.
3. Unplug/replug the camera: log shows `Camera disconnected`, then streaming restarts. Open the stream in two
   tabs / throttle a client to check that the device keeps running.

## Troubleshooting

- `Wi-Fi SSID is still the placeholder`: create `sdkconfig.secrets`, delete `sdkconfig`, rebuild.
- `Waiting for UVC camera`: wrong USB port, no VBUS power, or a non-UVC camera/bad adapter.
- `stream_start failed` / `ESP_ERR_NOT_FOUND`: mode unsupported at Full-Speed; try 320x240 or 640x480 @ 15 fps.
- `Frame buffer overflow`: increase `WROOMCAM_CAM_FRAME_BUF_KB`.
- Repeated reboots: check power quality/hub and the log line preceding `restarting`.
