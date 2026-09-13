# AirPlay + DLNA Receiver for ESP32-S3

![Firmware](https://img.shields.io/badge/firmware-v3.3.0-blue)
![Target](https://img.shields.io/badge/target-ESP32--S3-green)
![ESP-IDF](https://img.shields.io/badge/ESP--IDF-v5.5-red)
![Display](https://img.shields.io/badge/display-ST7789V%20320%C3%97240-orange)

This customised firmware turns an **ESP32-S3-WROOM-1** into a network audio
receiver with:

- AirPlay 2 playback
- DLNA/UPnP playback
- Internet radio
- PCM5102A I2S audio output
- Five-band software equalizer
- A 2.0-inch GMT020-02 ST7789V TFT now-playing display
- A responsive Liquid Glass web control panel
- Wi-Fi recovery, OTA updates, configuration backup and diagnostics

The current project version is **3.3.0**, with the cumulative stability,
display, playback-control and DLNA audio corrections through **R32**.

> This branch is configured specifically for the generic ESP32-S3 hardware
> described below. Bluetooth Classic/A2DP is not available on ESP32-S3.

## Current status

### Working

- AirPlay 2 audio using AAC/ALAC and PTP timing
- DLNA playback for MP3, FLAC, WAV, AAC, M4A, OGG, Opus and AMR
- Internet-radio presets
- Automatic source ownership between AirPlay, DLNA and radio
- Local volume, mute and Play/Pause fallback for the PCM5102A
- Five-band EQ, balance, limiter and speaker-protection processing
- AirPlay title, artist, album, playback time and progress metadata
- Long-title scrolling
- Spanish, Persian/Arabic and selected Japanese display glyphs
- Metadata-derived colour themes on the TFT
- Wi-Fi SSID shown on the TFT while idle or waiting for metadata
- Manual OTA and GitHub Release OTA controls
- Safe Mode, configuration migration and Wi-Fi credential testing
- Home Assistant/MQTT integration
- Settings backup and restore
- Light, dark and automatic web-interface themes
- Password protection and confirmed factory reset

### Intentionally disabled or limited

- **Album artwork is disabled.** Direct compressed-JPEG rendering previously
  produced a moving white rectangle and could interfere with audio playback.
- **Live WebSocket logs are disabled/removed from the main control panel.** Use
  USB serial output and retained diagnostics instead.
- **AirPlay 2 Previous/Next is sender-dependent.** These commands need a usable
  DACP connection or MRP support. MRP is not implemented, so the interface may
  correctly report that remote track control is unavailable.
- **The iPhone/iPad friendly name is not guaranteed.** AirPlay does not always
  transmit it. The display therefore shows the connected Wi-Fi SSID until real
  track metadata arrives.
- Bluetooth A2DP is not supported by the ESP32-S3 because it has no Bluetooth
  Classic radio.

## Required hardware

| Part | Recommended specification |
| --- | --- |
| Microcontroller | ESP32-S3-WROOM-1, preferably N16R8 with 16 MB flash and 8 MB PSRAM |
| DAC | PCM5102A I2S stereo DAC module |
| Display | GMT020-02, ST7789V/ST7789VW, 240×320, four-wire SPI |
| Amplifier | Powered speakers or a separate stereo amplifier |
| Power | Stable USB 5 V supply suitable for the ESP32-S3 and attached modules |

PSRAM is required for the large AirPlay jitter buffer and the LVGL display.

## PCM5102A wiring

| ESP32-S3 | PCM5102A | Purpose |
| --- | --- | --- |
| 5V | VIN | DAC power |
| GND | GND | Common ground |
| GPIO 8 | SCK/MCK | I2S master clock if the module exposes it |
| GPIO 11 | BCK | I2S bit clock |
| GPIO 13 | LCK/LRCK/WS | Left/right word clock |
| GPIO 12 | DIN | I2S audio data |

The configuration also defines GPIO 14 as the legacy software-ground output.
A real ESP32 **GND pin is preferred** for the DAC ground whenever wiring allows
it. Never connect PCM5102A audio outputs directly to passive speakers; use an
amplifier or powered speakers.

## GMT020-02 ST7789V display wiring

The display is SPI even though its board labels the clock and data pins `SCL`
and `SDA`. These are not I2C pins.

| Display pin | ESP32-S3 | Purpose |
| --- | --- | --- |
| GND | GND | Common ground |
| VCC | 3.3V | Display logic/backlight power |
| SCL | GPIO 18 | SPI clock |
| SDA | GPIO 17 | SPI MOSI/data |
| RST | GPIO 21 | Display reset |
| DC | GPIO 16 | Data/command selection |
| CS | GPIO 15 | Chip select |

The GMT020-02 board used by this project has no separate backlight header, so
`CONFIG_DISPLAY_BL_GPIO=-1`. Its native portrait resolution is 240×320; the
firmware configures it as **320×240 landscape** at a conservative 10 MHz SPI
clock for reliable operation with jumper wires.

Do not use old OLED instructions that assign SCL to GPIO 22. Those instructions
refer to a different I2C display and are not applicable to this ST7789 module.

## First installation

1. Download the latest GitHub Actions artifact produced from the current
   repository commit.
2. Extract the artifact.
3. Connect the ESP32-S3 to the computer by USB.
4. Open the [Espressif Web Flasher](https://espressif.github.io/esptool-js/) in
   Chrome or Edge.
5. Select the file ending in `-install.bin`.
6. Flash it at address `0x0`.
7. Fully power-cycle the ESP32-S3 after flashing.

For a clean installation, erase the flash before programming. This removes old
Wi-Fi settings, retained diagnostics and an incompatible partition layout.

> Use the **install BIN** only for USB flashing at `0x0`. Use the **OTA BIN**
> only through the firmware-update page. Interchanging them can prevent boot.

## Wi-Fi setup

1. Power on the receiver.
2. Join **AirPlay and DLNA Setup** from a phone or computer.
3. If the captive portal does not open automatically, browse to
   `http://192.168.240.1`.
4. Select the home Wi-Fi SSID, enter its password and choose the receiver name.
5. Save the configuration.
6. Find the receiver's normal LAN IP in the router's connected-device list.
7. Open that IP in a browser to access the control panel.

The setup access point remains available briefly after the station interface
connects, giving phones enough time to finish configuration. It then closes to
protect real-time audio performance. Reboot the receiver to reopen the setup
window when required.

For best AirPlay stability:

- Use 2.4 GHz Wi-Fi with a strong signal, preferably better than -70 dBm.
- Keep the ESP32-S3 away from noisy USB 3 devices and unshielded power supplies.
- Do not pin the receiver permanently to one mesh BSSID.
- Disable Wi-Fi power saving; the supplied ESP32-S3 defaults already do this.
- If the network drops, the firmware rebuilds stale RTSP/audio sockets after
  Wi-Fi reconnects.

## Using AirPlay

1. Connect the iPhone, iPad or Mac to the same network as the receiver.
2. Open Apple Music or another AirPlay-capable application.
3. Open the AirPlay output menu and select the configured receiver name.
4. Start playback.

The TFT initially shows `AirPlay Ready` and the Wi-Fi SSID. During connection it
shows `AirPlay Connected` and `Waiting for track details...`. Title, artist,
album, playback time and progress replace these fallback messages as soon as
the sender supplies metadata.

If metadata remains absent after updating the firmware:

1. Stop AirPlay.
2. Turn the iPhone's Wi-Fi off for ten seconds.
3. Turn Wi-Fi back on.
4. Select the receiver again.

This refreshes the sender's cached mDNS capability information. The receiver
advertises `md=0,2` for text metadata and progress while artwork is disabled.

## Using DLNA

Select the receiver from a DLNA/UPnP controller on the same network. The
renderer supports common music formats including MP3, FLAC, WAV, AAC, M4A,
OGG/Vorbis, Opus and AMR, subject to the source server providing a compatible
HTTP stream.

The R31 audio path includes:

- Negative EQ preamp/headroom when bands are boosted
- A lightweight peak limiter
- Rejection of non-finite DSP transients
- A 10 ms fade to silence on Pause
- A 10 ms fade-in on Resume
- EQ and limiter-history clearing at the silent pause boundary

These changes prevent the clipping, crackling and pause/disconnection noise
previously heard during DLNA playback.

## Equalizer and volume behaviour

The software DSP provides five peak bands, balance, normalization, limiter and
speaker protection.

It is normal for the volume to become slightly lower when positive EQ boosts
are applied. The firmware automatically creates headroom before the filters so
a full-scale DLNA or AirPlay signal does not clip. Pressing **Reset** returns the
bands to 0 dB and removes that EQ headroom, so the output becomes louder again.

Recommended practice:

- Prefer cutting unwanted frequencies instead of applying large boosts.
- Keep boosts below approximately +6 dB.
- Keep the limiter enabled.
- Compare EQ settings at matched listening volume.
- Start testing from flat EQ, then change one band at a time.

## Playback controls

| Control | AirPlay 2 behaviour | DLNA behaviour |
| --- | --- | --- |
| Volume −/+ | Local PCM output adjustment | Local PCM output adjustment |
| Mute | Local software mute gate | Local software mute gate |
| Play/Pause | Sender command when DACP is available; otherwise local audible gate | Renderer transport control |
| Previous/Next | Requires sender DACP or MRP | Available when the DLNA controller/queue supports it |

The PCM5102A has no writable volume register, so local volume and mute are
implemented in the final Q15 PCM path. This is expected.

When the panel reports:

> The active source does not expose remote Previous/Next control.

the firmware is operating correctly but the AirPlay sender did not expose a
usable DACP remote service. Modern AirPlay 2 senders may use Apple's MRP
protocol instead, which is not implemented in this project.

## TFT now-playing display

The 320×240 screen shows:

- Track title
- Artist
- Album
- Playing or paused state
- Elapsed and remaining time
- Progress bar
- Volume and mute state
- Scrolling text for long metadata
- Connected Wi-Fi name while waiting for metadata
- A stable colour palette derived from title, artist and album text

The generated LVGL font supports UTF-8, compressed glyph bitmaps,
left-to-right/right-to-left direction and Arabic/Persian contextual shaping.
It includes Latin/Spanish, Persian/Arabic and a selected Japanese character
set. A character not included in the generated font may still appear as a
missing-glyph box.

### Album artwork

Album artwork is deliberately disabled in the current stable build:

```ini
# CONFIG_LV_USE_TJPGD is not set
# CONFIG_ENABLE_AIRPLAY_ARTWORK is not set
```

Do not enable these options until artwork is decoded into a bounded RGB565
buffer outside the real-time audio path. The current metadata-derived colour
theme provides changing visual colour without downloading or decoding images.

## Web control panel

Open `http://<receiver-ip>/` after Wi-Fi setup. The Liquid Glass interface is
responsive on phones and desktop browsers and provides:

- Receiver name and Wi-Fi configuration
- Source, playback, volume and mute control
- Per-source five-band EQ
- Startup-volume and maximum-volume safety
- Sleep timer: 15, 30, 60 or 120 minutes
- Internet-radio favourites
- DLNA queue management
- Codec, sample-rate, bit-depth and buffering information
- Multiple saved Wi-Fi networks and diagnostics
- Manual firmware upload and GitHub firmware updater side by side
- Settings backup and restore
- Audio clipping, limiter and speaker-protection information
- Idle-only scheduled restart
- Light, dark and automatic themes
- Configuration-panel password
- Phrase-confirmed factory reset
- Recovery/Safe Mode and settings migration
- Wi-Fi credential testing with rollback
- MQTT/Home Assistant settings
- Left, right, stereo and balance audio tests

The unstable **System Logs** card and long-lived browser WebSocket were removed
from the main control panel. Use retained warnings/errors or USB serial at
115200 baud for diagnostics.

## Backup and restore

Before changing firmware or performing a factory reset:

1. Open the advanced or maintenance section of the control panel.
2. Download the configuration backup.
3. Store the JSON configuration file safely.
4. After updating, use Restore and upload the saved file.
5. Reboot only when requested by the interface.

Configuration migration preserves compatible settings across firmware updates
and resets only values that cannot be safely converted.

## Safe Mode and recovery

After repeated abnormal resets, Safe Mode can start only the minimum networking
and configuration services so a bad setting can be corrected. Recovery tools
can restore the last usable configuration without erasing every setting.

ESP reset reason `4` indicates a watchdog/panic-class reset on this platform;
it is not caused by an ordinary browser socket closing. Use the retained reset
counter and serial output to identify repeated failures.

## Home Assistant and MQTT

MQTT integration is optional and disabled by default. Configure the broker URI,
credentials and discovery settings from the Reliability page. When enabled,
the receiver can publish availability and diagnostic data and expose supported
controls through Home Assistant MQTT discovery.

Keep MQTT disabled while diagnosing AirPlay timing or Wi-Fi stability so there
is only one variable under test.

## Firmware updates

### Manual OTA

1. Build or download the matching `-ota.bin`.
2. Open the firmware-update section.
3. Select the OTA file and begin the update.
4. Do not remove power during programming.
5. Allow the receiver to restart and complete its boot-stability check.

The firmware validates the image and uses the ESP-IDF rollback mechanism. If a
new image cannot complete the required stable boot, the previous OTA partition
can be restored.

### GitHub updater

The Reliability page can check the configured GitHub Releases repository,
compare versions and install a compatible OTA asset. Product and image
validation are performed before the update is accepted.

## Building with GitHub Actions

Do not commit a generated root `sdkconfig`. Keep `sdkconfig.defaults` and all
required `sdkconfig.defaults.*` board profiles.

1. Upload or commit the complete project, preserving all directories and Git
   submodules.
2. Confirm `.gitignore` contains:

   ```gitignore
   sdkconfig
   sdkconfig.old
   build/
   ```

3. Open the repository's **Actions** tab.
4. Select **Build ESP32-S3 installation BIN**.
5. Choose **Run workflow**.
6. Wait for the ESP-IDF 5.5 build and merged-image validation to pass.
7. Download the artifact named similar to:

   ```text
   airplay-esp32s3-v3.3.0-bin-files
   ```

The artifact contains:

```text
airplay2-receiver-esp32s3-v3.3.0-install.bin
airplay2-receiver-esp32s3-v3.3.0-ota.bin
SHA256SUMS-v3.3.0.txt
flash_args
flasher_args.json
```

The build workflow removes stale `sdkconfig`, `sdkconfig.old` and `build/`
before compiling the ESP32-S3 target.

## Local ESP-IDF build

ESP-IDF **5.5** is used by the GitHub workflow.

```bash
git clone --recursive https://github.com/dora310/airplay-DLNA-esp32.git
cd airplay-DLNA-esp32

# Activate ESP-IDF 5.5 first, then:
idf.py set-target esp32s3
idf.py -DSDKCONFIG_DEFAULTS="sdkconfig.defaults;sdkconfig.defaults.esp32s3" build
```

To flash all partitions locally:

```bash
idf.py -p PORT flash monitor
```

Replace `PORT` with the ESP32-S3 serial port, for example `/dev/ttyUSB0` on
Linux or `COM5` on Windows.

## Troubleshooting

### Backlight is on but the TFT is blank

- Confirm the module is the four-wire SPI ST7789V version.
- Remember that display `SCL` means SPI clock and `SDA` means SPI MOSI.
- Verify GPIO 18, 17, 15, 16 and 21 against the wiring table.
- Confirm VCC is 3.3 V and all modules share ground.
- Confirm the build contains `CONFIG_DISPLAY_DRIVER_ST7789=y`.
- Use the complete install BIN so the correct partitions and resources are
  flashed.

### AirPlay connects and then disappears

- Check Wi-Fi RSSI and mesh roaming.
- Confirm power saving is disabled.
- Close old control-panel/log-viewer browser tabs.
- Confirm the firmware rebuilds AirPlay services after Wi-Fi reconnects.
- Use USB serial to distinguish an RTSP session close from a full Wi-Fi drop or
  device restart.

### `Dropping late frame: 10 ms`

An occasional small late frame is not a connection failure. Repeated late
frames measuring hundreds or thousands of milliseconds indicate lost timing,
network interruption or an old audio anchor and require the stream to be
flushed/re-anchored.

Do not delete the warning log statement as a fix; it reports the timing problem
but does not cause it.

### RF calibration warning after erasing flash

```text
phy_init: failed to load RF calibration data ..., falling back to full calibration
```

This is normally harmless on the first boot after a full erase. ESP-IDF runs a
complete RF calibration and stores new data for later boots.

### Browser socket errors

Messages such as `error in recv: 104` usually mean the browser closed a TCP
connection. They do not by themselves indicate that AirPlay audio disconnected.

### EQ sounds quieter

This is expected when bands are boosted because automatic negative preamp
headroom prevents clipping. Resetting to a flat EQ restores the original gain.

### Previous/Next is unavailable

The sender did not expose a usable DACP service, or it uses MRP. Volume still
works locally. This is an AirPlay remote-control limitation, not a PCM5102A or
Wi-Fi fault.

## Important repository rules

- Do not upload generated `sdkconfig`, `sdkconfig.old` or `build/` output.
- Do not delete `sdkconfig.defaults` or `sdkconfig.defaults.esp32s3`.
- Apply replacement packages in revision order unless a package explicitly
  states that it is cumulative.
- Build a new artifact from the exact commit containing the latest files.
- Do not reuse an older Actions artifact after changing source code.
- Keep the confirmed R21 metadata parser unless a replacement specifically
  addresses metadata handling.

## Cumulative custom revision summary

| Revision | Main change |
| --- | --- |
| Stability/Recovery R1–R13 | Wi-Fi roaming, socket restart, AirPlay recovery and diagnostic isolation |
| R14 | Removed the non-working System Logs card |
| R15 | Initial DLNA crackle reduction |
| R18–R19 | GMT020-02 ST7789V 320×240 support and metadata layout |
| R20–R21 | Nested binary-plist/UTF-16 metadata parsing and `md=0,2` advertisement |
| R24 | 120-second setup Wi-Fi window; unstable artwork disabled |
| R27 | Restored the confirmed-working R21 metadata baseline |
| R28 | Compressed UTF-8 international font correction |
| R29 | Metadata-derived display colour theme |
| R30 | PCM5102A mute/Play-Pause fallback and accurate DACP control reporting |
| R31 | DLNA EQ headroom, limiter and pause/resume fade correction |
| R32 | Wi-Fi identity fallback and removal of the `---` display state |

## Credits

This project builds on the AirPlay receiver work from:

- [rbouteiller/airplay-esp32](https://github.com/rbouteiller/airplay-esp32)
- [KinDR007/airplay-esp32](https://github.com/KinDR007/airplay-esp32)

It also uses ESP-IDF, LVGL and their respective third-party components. See the
repository's `LICENSE` file and component licence files. The upstream project
uses non-commercial licensing conditions; review the complete licence before
redistributing firmware or using it commercially.

## Disclaimer

This is an experimental network-audio project. Test at low volume first,
especially after changing DSP, DAC wiring or amplifier hardware. Keep a known
working install BIN and a settings backup before applying updates.
