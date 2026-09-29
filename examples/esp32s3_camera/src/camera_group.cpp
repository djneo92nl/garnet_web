#include "camera_group.h"

#include <SD_MMC.h>
#include <atomic>
#include <esp_camera.h>
#include <garnet_web.h>

namespace {

// ---- Board wiring ------------------------------------------------------------
// ESP32-S3-WROOM "CAM" boards (Freenove-style, CH343 USB): the same camera
// pinout as Espressif's ESP32-S3-EYE. No PWDN / RESET lines.
constexpr int kPinPwdn = -1, kPinReset = -1, kPinXclk = 15;
constexpr int kPinSiod = 4, kPinSioc = 5;
constexpr int kPinD7 = 16, kPinD6 = 17, kPinD5 = 18, kPinD4 = 12;
constexpr int kPinD3 = 10, kPinD2 = 8, kPinD1 = 9, kPinD0 = 11;
constexpr int kPinVsync = 6, kPinHref = 7, kPinPclk = 13;

constexpr const char *kCam = "camera";

// Select index -> sensor frame size. Frame buffers are sized for the
// largest entry at init (see cameraBegin), so every option can be
// switched to live without re-initializing the driver.
const char *const kSizes[] = {"QVGA 320x240", "VGA 640x480",   "SVGA 800x600",
                              "HD 1280x720",  "SXGA 1280x1024", "UXGA 1600x1200",
                              "FHD 1920x1080", "QXGA 2048x1536"};
const framesize_t kSizeIds[] = {FRAMESIZE_QVGA, FRAMESIZE_VGA,  FRAMESIZE_SVGA,
                                FRAMESIZE_HD,   FRAMESIZE_SXGA, FRAMESIZE_UXGA,
                                FRAMESIZE_FHD,  FRAMESIZE_QXGA};
constexpr uint8_t kSizeCount = sizeof(kSizes) / sizeof(kSizes[0]);

String sensorName = "-";
String lastSnap = "none";
bool sdMounted = false;

// Written by the stream task, read by the Info getter.
std::atomic<uint32_t> streamFps10{0};    // fps * 10
std::atomic<uint32_t> streamFrameKB{0};
std::atomic<bool> streaming{false};

httpd_handle_t streamServer = nullptr;

// ---- Settings -> sensor ------------------------------------------------------

void applySettings() {
  sensor_t *s = esp_camera_sensor_get();
  if (s == nullptr) return;
  int size = gsGetInt(kCam, "size");
  s->set_framesize(s, kSizeIds[size >= 0 && size < kSizeCount ? size : 1]);
  s->set_quality(s, gsGetInt(kCam, "quality"));
  s->set_brightness(s, gsGetInt(kCam, "bright"));
  s->set_contrast(s, gsGetInt(kCam, "contrast"));
  s->set_saturation(s, gsGetInt(kCam, "sat"));
  s->set_hmirror(s, gsGetBool(kCam, "hmirror"));
  s->set_vflip(s, gsGetBool(kCam, "vflip"));
}

// Whole-group hook: the sensor settings interact (a size change resets
// some of them on the OV5640), so re-apply all of them together.
void onCameraSaved(const GsGroup &) { applySettings(); }

void snapshotToSd() {
  // Runs on the loop task (garnet_web defers button actions), not the
  // stream task - both may hold a frame buffer at once, which the
  // 2-buffer PSRAM config allows.
  if (!sdMounted) {
    lastSnap = "no SD card";
    return;
  }
  camera_fb_t *fb = esp_camera_fb_get();
  if (fb == nullptr) {
    lastSnap = "capture failed";
    return;
  }
  char path[32];
  snprintf(path, sizeof(path), "/snap_%08lu.jpg", (unsigned long)millis());
  File f = SD_MMC.open(path, FILE_WRITE);
  size_t written = f ? f.write(fb->buf, fb->len) : 0;
  if (f) f.close();
  esp_camera_fb_return(fb);
  lastSnap = written ? String(path) + " (" + String(written / 1024) + " KB)" : String("write failed");
}

String sensorStr() { return sensorName; }
String streamStr() {
  if (!streaming) return "idle";
  uint32_t f = streamFps10;
  return String(f / 10) + "." + String(f % 10) + " fps, " + String((uint32_t)streamFrameKB) +
         " KB/frame";
}
String lastSnapStr() { return lastSnap; }

const GsField kCamFields[] = {
    gsInfo("sensor", "Sensor", sensorStr),
    gsInfo("fps", "Stream", streamStr),
    gsSelect("size", "Resolution", kSizes, kSizeCount, 1),
    gsWithHelp(gsNumber("quality", "JPEG quality", 12, 4, 63), "Lower = better, larger"),
    gsNumber("bright", "Brightness", 0, -2, 2),
    gsNumber("contrast", "Contrast", 0, -2, 2),
    gsNumber("sat", "Saturation", 0, -2, 2),
    gsToggle("hmirror", "Mirror", false),
    gsToggle("vflip", "Flip", false),
    gsButton("snap", "Snapshot to SD", snapshotToSd),
    gsInfo("lastsnap", "Last snapshot", lastSnapStr),
};
const GsGroup kCamGroup = gsWithWidget(
    gsWithOnSave(gsGroup(kCam, "Camera", "camera", kCamFields), onCameraSaved), "img::81/stream");

// ---- MJPEG stream (port 81) ----------------------------------------------------
// Its own esp_http_server instance: a stream handler never returns while
// someone watches, and on garnet_web's server that would block the whole
// settings UI. One viewer at a time is plenty for a demo.

constexpr const char *kBoundary = "gwframe";

esp_err_t handleStream(httpd_req_t *req) {
  log_i("stream: client connected");
  if (!gwAuthorized(req)) {
    log_w("stream: rejected, no valid garnet_web session cookie");
    httpd_resp_set_status(req, "401 Unauthorized");
    return httpd_resp_sendstr(req, "login required");
  }
  httpd_resp_set_type(req, "multipart/x-mixed-replace;boundary=gwframe");
  httpd_resp_set_hdr(req, "Cache-Control", "no-store");
  streaming = true;
  uint32_t windowStart = millis(), frames = 0, misses = 0;
  esp_err_t err = ESP_OK;
  while (err == ESP_OK) {
    camera_fb_t *fb = esp_camera_fb_get();
    if (fb == nullptr) {
      if (++misses % 20 == 1) log_w("stream: no frame from camera (%u misses)", (unsigned)misses);
      delay(10);
      continue;
    }
    if (frames == 0 && streamFps10 == 0) log_i("stream: first frame %u bytes", (unsigned)fb->len);
    char head[96];
    int n = snprintf(head, sizeof(head),
                     "\r\n--%s\r\nContent-Type: image/jpeg\r\nContent-Length: %u\r\n\r\n",
                     kBoundary, (unsigned)fb->len);
    err = httpd_resp_send_chunk(req, head, n);
    if (err == ESP_OK) err = httpd_resp_send_chunk(req, (const char *)fb->buf, fb->len);
    streamFrameKB = fb->len / 1024;
    esp_camera_fb_return(fb);

    frames++;
    uint32_t elapsed = millis() - windowStart;
    if (elapsed >= 2000) {
      streamFps10 = frames * 10000 / elapsed;
      frames = 0;
      windowStart = millis();
    }
  }
  // The send failing is how we learn the viewer left (tab closed, group
  // switched, or the UI paused it while hidden).
  streaming = false;
  log_i("stream: client left");
  return ESP_OK;
}

// Single JPEG, e.g. for "save image" from a browser.
esp_err_t handleCapture(httpd_req_t *req) {
  if (!gwAuthorized(req)) {
    httpd_resp_set_status(req, "401 Unauthorized");
    return httpd_resp_sendstr(req, "login required");
  }
  camera_fb_t *fb = esp_camera_fb_get();
  if (fb == nullptr) return httpd_resp_send_500(req);
  httpd_resp_set_type(req, "image/jpeg");
  httpd_resp_set_hdr(req, "Content-Disposition", "inline; filename=capture.jpg");
  esp_err_t err = httpd_resp_send(req, (const char *)fb->buf, fb->len);
  esp_camera_fb_return(fb);
  return err;
}

} // namespace

bool cameraBegin() {
  camera_config_t c = {};
  c.pin_pwdn = kPinPwdn;
  c.pin_reset = kPinReset;
  c.pin_xclk = kPinXclk;
  c.pin_sccb_sda = kPinSiod;
  c.pin_sccb_scl = kPinSioc;
  c.pin_d7 = kPinD7;
  c.pin_d6 = kPinD6;
  c.pin_d5 = kPinD5;
  c.pin_d4 = kPinD4;
  c.pin_d3 = kPinD3;
  c.pin_d2 = kPinD2;
  c.pin_d1 = kPinD1;
  c.pin_d0 = kPinD0;
  c.pin_vsync = kPinVsync;
  c.pin_href = kPinHref;
  c.pin_pclk = kPinPclk;
  c.xclk_freq_hz = 20000000;
  c.ledc_timer = LEDC_TIMER_0;
  c.ledc_channel = LEDC_CHANNEL_0;
  c.pixel_format = PIXFORMAT_JPEG;
  // Buffers sized for the largest selectable resolution, so switching
  // size later is just set_framesize (see kSizes).
  c.frame_size = FRAMESIZE_QXGA;
  c.jpeg_quality = 12;
  c.fb_count = 2;
  c.fb_location = CAMERA_FB_IN_PSRAM;
  c.grab_mode = CAMERA_GRAB_LATEST; // a slow viewer gets fresh frames, not a backlog

  esp_err_t err = esp_camera_init(&c);
  if (err != ESP_OK) {
    log_e("camera init failed: 0x%x - check the pin map in camera_group.cpp", err);
    return false;
  }
  sensor_t *s = esp_camera_sensor_get();
  switch (s->id.PID) {
  case OV5640_PID: sensorName = "OV5640"; break;
  case OV3660_PID: sensorName = "OV3660"; break;
  case OV2640_PID: sensorName = "OV2640"; break;
  default: sensorName = "PID 0x" + String(s->id.PID, HEX); break;
  }
  log_i("camera: %s", sensorName.c_str());

  // SD slot on the SDMMC bus, 1-bit mode (CLK 39, CMD 38, D0 40).
  SD_MMC.setPins(39, 38, 40);
  sdMounted = SD_MMC.begin("/sdcard", true);
  if (sdMounted) {
    gwSetSd(SD_MMC);
    gwAddFs("sd", "SD Card", SD_MMC); // snapshots land here - browse/download under Files
  }
  else log_w("SD card not mounted");

  gsRegister(kCamGroup);
  applySettings();
  return true;
}

void cameraStartStream() {
  httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
  cfg.server_port = 81;
  cfg.ctrl_port = 32769; // garnet_web's server uses port + 32768
  cfg.max_open_sockets = 3;
  cfg.lru_purge_enable = true;
  if (httpd_start(&streamServer, &cfg) != ESP_OK) {
    log_e("stream server failed to start");
    return;
  }
  static const httpd_uri_t kStream = {"/stream", HTTP_GET, handleStream, nullptr};
  static const httpd_uri_t kCapture = {"/capture", HTTP_GET, handleCapture, nullptr};
  httpd_register_uri_handler(streamServer, &kStream);
  httpd_register_uri_handler(streamServer, &kCapture);
}
