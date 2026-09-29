#pragma once

#include <garnet_settings.h>

// OV5640 / OV3660 camera for the S3 demo: sensor init, an MJPEG stream server on
// port 81 (shown in the UI by the group's "img::81/stream" widget), and
// the "Camera" settings group.
//
// Call cameraBegin() before gwBegin() so the group is registered.
// Returns false (and registers nothing) when no sensor answers.
bool cameraBegin();

// Starts the stream server; call after gwBegin() (needs the network up
// only when a client connects, so any time after WiFi.mode() is fine).
void cameraStartStream();
