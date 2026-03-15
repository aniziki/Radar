// Manually written config.h for libqrencode (replaces autoconf output).
// Targets the ESP32 Arduino build via PlatformIO.

#pragma once

#define MAJOR_VERSION 4
#define MINOR_VERSION 1
#define MICRO_VERSION 1
#define VERSION "4.1.1"

// In release builds, internal helpers are declared static.
#define STATIC_IN_RELEASE static

// Pthreads not available on ESP32 bare-metal.
#undef HAVE_PTHREAD