# WroomCam

ESP-IDF firmware for the **AI-Thinker-style ESP32-CAM** (ESP32-S module, 4 MB PSRAM, **OV2640** camera). It captures JPEG frames from the camera and serves them as a `multipart/x-mixed-replace` MJPEG stream over Wi-Fi.

> **Status:** ported from an ESP32-S3/USB-UVC design. It has not been compiled or tested on hardware in this repository's CI; validate with the steps below.

## Hardware

- Board: ESP32-CAM (ESP32-S, 4 MB flash, 4 MB PSRAM, OV2640). See the [ESP32-CAM getting-started guide](https://lastminuteengineers.com/getting-started-with-esp32-cam/).
- Camera pins (AI-Thinker pin map, fixed in `main/camera_source.c`): PWDN=GPIO32, RESET=none, XCLK=GPIO0, SCCB SDA=GPIO26, SCL=GPIO27, D0..D7=GPIO5,18,19,21,36,39,34,35, VSYNC=GPIO25, HREF=GPIO23, PCLK=GPIO22. Capture uses the official [`espressif/esp32-camera`](https://components.espressif.com/components/espressif/esp32-camera) driver.
- **ESP32-CAM-MB adapter:** it is only a USB-to-serial/programming adapter (UART0 for flashing and logs). It adds no runtime camera interface.
- **GPIO0:** it is the camera XCLK pin and also the boot-mode strap. GPIO0 must be low at reset to enter the serial bootloader. The ESP32-CAM-MB normally handles this through auto-reset; if flashing fails with "Wrong boot mode" / "Failed to connect", hold the adapter's IO0 button while pressing reset (or while plugging in USB), flash, then release it and press reset to boot normally.

## Build and flash

Requires ESP-IDF **v5.4 or v5.5**; `espressif/esp32-camera ~2.0.15` is fetched by the Component Manager.

```sh
. $IDF_PATH/export.sh
# set CONFIG_WROOMCAM_WIFI_SSID / CONFIG_WROOMCAM_WIFI_PASSWORD in sdkconfig.secrets (git-ignored; never commit credentials)
idf.py set-target esp32
idf.py build
idf.py -p <serial-port> flash monitor
```

`sdkconfig.defaults` targets classic `esp32` with 4 MB flash and 4 MB Quad-SPI PSRAM. If you change defaults after an initial build, delete the generated `sdkconfig` or use `idf.py menuconfig`.

## Configuration (`idf.py menuconfig` → WroomCam)

| Setting | Default |
|---|---|
| Wi-Fi SSID/password | placeholders; replace before use |
| Static IPv4 address/gateway/netmask/DNS | DHCP by default; static optional |
| JPEG frame size | QVGA 320x240 (up to UXGA 1600x1200) |
| JPEG quality | 12 (lower = better quality, larger frames) |
| PSRAM frame buffers | 2 |
| Camera XCLK | 20 MHz |
| HTTP port | 80 |

Increase the frame size gradually; larger frames lower the frame rate and need more Wi-Fi bandwidth. PSRAM is required for frame buffers.

## Use

After boot, the serial log prints the assigned IP. Open `http://<device-ip>/` or `http://<device-ip>/stream`. Only one stream viewer is served at a time. Wi-Fi uses modem sleep while idle and is switched to full performance while a stream is active; Wi-Fi reconnects with backoff.

## Validation

1. Confirm serial logs show `OV2640 ready`, Wi-Fi connected and an IPv4 address.
2. Open `/` in a browser and confirm live video.
3. Check multipart framing: `python3 tools/check_stream.py http://<device-ip>/stream 50`.
4. Host-side test of the stream checker (no hardware): `python3 tools/test_check_stream.py`.

Troubleshooting: `esp_camera_init failed` (0x105/0x20001) means a bad camera ribbon connection or insufficient power — use a good USB supply. Brown-outs/resets during streaming usually indicate power problems. Missing frames at high resolutions: lower the frame size or raise the JPEG quality value.
