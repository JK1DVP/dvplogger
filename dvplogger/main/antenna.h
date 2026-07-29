#ifndef FILE_ANTENNA_H
#define FILE_ANTENNA_H

#include <Arduino.h>
#include "decl.h"

#define ANTENNA_RADIOS 2
#define ANTENNA_MAX_ID 9
#define ANTENNA_PREF_ROWS 3

extern int antenna_control_enable;
extern char antenna_host[64];
extern int antenna_port;
extern char antenna_pref[ANTENNA_PREF_ROWS][N_BAND + 1];
extern char antenna_name[ANTENNA_MAX_ID][24];

void antenna_process();
void antenna_force_resend();
void antenna_settings_changed();
String antenna_status_json();
const char *antenna_controller_state();

#endif
