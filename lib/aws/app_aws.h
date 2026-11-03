#ifndef app_aws_h_
#define app_aws_h_

#include <stdint.h>

// Firebase Realtime Database layout used by the fan:
//   /fan/desired     <- written by the app / voice side (0, 33, 66, 100)
//   /fan/powerlevel  <- reported by the ESP32 (current fan speed)
#define FAN_PATH_DESIRED "/fan/desired"
#define FAN_PATH_REPORTED "/fan/powerlevel"

typedef void (*fan_state_changed_f)(uint8_t);

void appaws_init(fan_state_changed_f);
void appaws_connect(void *);
void appaws_publish(uint8_t);

#endif