# WroomCam

ESP32-S3 firmware that acts as a **USB host for a UVC camera** (MJPEG) and serves it as a
`multipart/x-mixed-replace` stream over Wi-Fi.

> **Status:** written against ESP-IDF / `espressif/usb_host_uvc` APIs. It has not yet been compiled or tested on hardware; validate with the build and smoke-test steps below.

## Board identification and USB wiring

The attached photo appears to show a third-party ESP32-S3-WROOM-1 development board, not an official Espressif DevKitC-1: it has a CH343P USB-to-serial bridge, two USB-C connectors, and a module marking that looks like **ESP32-S3-WROOM-1-N16R8**. If that module marking is accurate, it indicates 16 MB Quad-SPI flash and 8 MB Octal-SPI PSRAM; `sdkconfig.defaults` is configured accordingly. Confirm the full module label printed on the metal shield in case the image is illustrative or the board is a clone with a different module.

From the attached board image:

- **Left USB-C connector:** labelled ESP32-S3 USB & OTG; use this native USB connector for the UVC camera.
- **Right USB-C connector:** labelled USB to Serial; use this for flashing and serial logs.
- Native USB D- is GPIO19 and D+ is GPIO20. The ESP32-S3 USB OTG peripheral is Full-Speed; it is not High-Speed USB.
- A USB-C-to-USB-A host adapter/cable is needed to attach a USB camera. Check cable orientation and host-role wiring if the camera is not detected.

The official Espressif DevKitC-1 v1.0 guide independently confirms the native port is a Full-Speed USB OTG interface and identifies GPIO19/20 as USB D-/D+. However, the photographed board is not necessarily the same PCB/schematic as the official DevKitC-1. Neither the photo nor the linked official DevKitC guide proves that this clone's OTG connector sources 5 V VBUS in host mode. Do not assume it powers the camera: use a properly powered USB host adapter/hub or safe 5 V VBUS supply with common ground, and **never power the camera from a GPIO**. Avoid tying two independent 5 V sources together or back-feeding a USB port. Camera VBUS power switching is not implemented by this firmware; stopping capture does not turn off camera power.

Official board references:

- [ESP32-S3-DevKitC-1 v1.0 user guide](https://docs.espressif.com/projects/esp-dev-kits/en/latest/esp32s3/esp32-s3-devkitc-1/user_guide_v1.0.html)
- [ESP32-S3-DevKitC-1 v1.1 user guide](https://docs.espressif.com/projects/esp-dev-kits/en/latest/esp32s3/esp32-s3-devkitc-1/user_guide_v1.1.html)
- [ESP32-S3-WROOM-1 datasheet](https://documentation.espressif.com/esp32-s3-wroom-1_wroom-1u_datasheet_en.html)

The photos/docs establish connector purpose and USB pins, not VBUS host power capability for this pictured third-party PCB. Inspect its exact schematic or measure VBUS safely before relying on it.

## Performance and idle power

The ESP32-S3 USB peripheral is **Full-Speed (12 Mbit/s)**. Actual UVC bandwidth is much lower than the signaling rate; begin with 320x240 or 640x480 MJPEG at 15 fps and increase gradually. Camera-advertised high-resolution modes may require USB High-Speed and may not be available through this host. The camera's frame size, bus mode, Wi-Fi throughput, and PSRAM/RAM determine achievable performance.

Power behavior:

- Wi-Fi remains connected so the HTTP stream can be requested, but uses `WIFI_PS_MIN_MODEM` while idle. This reduces radio activity with possible network-response latency.
- When `/stream` has an active HTTP handler, the firmware disables Wi-Fi modem sleep for lower-latency delivery and signals the UVC supervisor to start capture.
- When the stream client disconnects/times out, capture is stopped and the UVC stream is closed; Wi-Fi modem sleep is restored. The USB host remains installed so it can detect the camera and serve future requests.
- This is not deep sleep: the ESP32 must keep Wi-Fi/HTTP available. The camera may still draw power from VBUS even after UVC capture stops. Full camera power-off needs a controllable VBUS load switch or power-switched hub, which this pictured board/firmware has not been verified to provide.

## Build

Requires ESP-IDF **v5.4 or v5.5** and `espressif/usb_host_uvc ~2.5.2` (resolved by ESP-IDF Component Manager).

```sh
. $IDF_PATH/export.sh
cp sdkconfig.secrets.example sdkconfig.secrets   # edit SSID/password; this file is ignored by git
idf.py set-target esp32s3
idf.py build
idf.py -p <USB-to-serial-port> flash monitor
```

If this exact board does not have the pictured N16R8 module, check the full module label and adjust `sdkconfig.defaults` accordingly. For example, a board with no PSRAM should not force octal PSRAM; the firmware has a 320x240 fallback but build-time PSRAM configuration must still match the module. When changing defaults/secrets after the initial build, remove generated `sdkconfig` or use `idf.py menuconfig`.

## Configuration

Configure in `sdkconfig.secrets` (copy `sdkconfig.secrets.example`) or `idf.py menuconfig` under WroomCam:

| Setting | Default |
|---|---|
| Wi-Fi SSID/password | placeholders; replace before use |
| Static IPv4 address/gateway/netmask/DNS | DHCP by default; static optional |
| Camera width/height/FPS | 320x240 @ 15 fps MJPEG |
| Probe camera MJPEG modes on demand | Off |
| Frame buffer size | auto; increase if the log reports frame overflow |
| HTTP port | 80 |

Raise camera resolution/FPS in steps and check camera mode support and logs. USB Full-Speed means a mode advertised by the camera is not necessarily usable on the ESP32-S3. PSRAM is strongly recommended for larger JPEG frames.

### Temporarily discover camera modes

To test descriptor-reported modes instead of the configured fixed width, height, and FPS, enable **WroomCam → Camera → Probe camera MJPEG modes on demand** in `idf.py menuconfig`, or add this line to your local, git-ignored `sdkconfig.secrets`:

```ini
CONFIG_WROOMCAM_AUTO_DETECT_MODE=y
```

The default is off. Discovery and probing happen only after both Wi-Fi is ready and a `/stream` client is connected. The firmware logs each reported format, resolution, default frame rate, and advertised interval data. It tries at most 64 candidates, ordered by increasing pixel area and then increasing FPS (the lowest-bandwidth advertised rate first within each resolution). Each candidate must both open and start; failed opens/starts are closed before trying the next. The first successful mode is logged as:

```text
Auto-detect selected MJPEG <width>x<height> @ <fps> fps after successful open/start
```

If probing exhausts the list, the existing stream retry/backoff policy applies once to the whole candidate list; the firmware does not reboot after each candidate. After noting the selected mode, turn the option off in menuconfig (or set `CONFIG_WROOMCAM_AUTO_DETECT_MODE=n`) and set `CONFIG_WROOMCAM_CAM_WIDTH`, `CONFIG_WROOMCAM_CAM_HEIGHT`, and `CONFIG_WROOMCAM_CAM_FPS` to the logged values. Rebuild and flash to return to the normal fixed-mode configuration.

## Use

After boot, the serial log prints the assigned IP. Open `http://<device-ip>/` or `http://<device-ip>/stream`. Only one stream viewer is served at a time by this configuration of the ESP-IDF HTTP server.

## Recovery and validation

The UVC supervisor retries camera open/start and recovers from disconnects, transfer errors, and frame stalls with backoff; repeated failures trigger a controlled chip restart. Wi-Fi reconnects with backoff. The firmware has not been compiled or tested on this specific third-party board, so validate before unattended use.

1. Confirm serial logs show Wi-Fi connected and an IPv4 address.
2. Connect a powered USB host adapter/hub and check for UVC camera detection.
3. Request `/stream`; verify capture starts only after a client request and stops after the client closes or times out.
4. Test multipart framing: `python3 tools/check_stream.py http://<device-ip>/stream 50`.
5. Disconnect/reconnect the camera and Wi-Fi; check recovery logs.

Troubleshooting: no camera detection can indicate wrong connector, no VBUS, non-UVC camera, or host adapter/cable wiring. Frame overflow means increase configured buffer memory or lower the camera mode. Repeated resets require checking the preceding log line, camera power stability, and PSRAM/flash configuration.
