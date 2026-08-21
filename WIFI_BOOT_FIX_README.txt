ESP32-S3 v0.3.00 WiFi boot stability replacement files
=======================================================

Copy the extracted files into the ROOT of the GitHub repository and allow
Windows to replace the existing files. Keep the directory structure intact.

Files replaced:
  main/main.c
  main/network/wifi.c
  main/settings.c
  sdkconfig.defaults.esp32s3

Important:
  1. Commit and push all four replacements.
  2. Run the "Build ESP32-S3 installation BIN" GitHub Actions workflow.
  3. Download the new artifact from that run; do not reuse an older BIN.
  4. Erase the ESP32-S3 flash completely.
  5. Flash the new *-install.bin at address 0x0.
  6. Reset the board and look for ESP32-AirPlay-Setup.

Changes in this patch:
  - Starts the setup AP before optional v0.3 services.
  - Makes optional DSP/source/monitor services non-fatal at boot.
  - Disables unstable OTA rollback defaults for fresh installations.
  - Does not attempt STA connection when no SSID is saved.
  - Treats an empty stored SSID as unconfigured.
  - Uses OPEN station auth threshold when credentials are blank.
  - Keeps the AirPlay and DLNA service structure unchanged.
