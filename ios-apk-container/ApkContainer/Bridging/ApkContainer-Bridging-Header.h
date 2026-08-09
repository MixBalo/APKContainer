//
//  ApkContainer-Bridging-Header.h
//  Imports the C ABI in ApkContainer.h so Swift façades can call
//  apkcontainer_runtime_launch / suspend / resume / force_quit /
//  lifecycle_dispatch / graphics_attach_layer / input_enqueue_touch /
//  audio_start / audio_stop. Implementations live in Native/RuntimeGlue/runtime_glue.c.
//
#ifndef APKCONTAINER_BRIDGING_HEADER_H
#define APKCONTAINER_BRIDGING_HEADER_H
#include "ApkContainer.h"
#endif
