#pragma once

/* Zentrale Versionskonstante fuer die sscsaunalogger-ESP32-App.
 * Bei jedem Release-Bump NUR hier aendern - UI, Log-Banner, HTTP-Header
 * ziehen sich den String von hier.
 *
 * Die RP2040-Firmware hat ihre eigene VERSION-Konstante in
 * SenseCAP_Indicator_RP2040.ino::VERSION - die sollte beim Release
 * synchron gehalten werden, muss aber nicht byte-identisch sein (sie
 * enthaelt z.B. den "ssc-"-Prefix fuer Serial-Logs). */
#define SSC_APP_VERSION "0.3.3"
