#ifndef UI_COMMON_H
#define UI_COMMON_H

#include "lvgl.h"
#include <stdint.h>

extern int32_t touch_start_x;
extern lv_obj_t *scr_apps;
extern uint8_t current_battery_pct;
extern bool ble_started;

void screen_pressed_cb(lv_event_t * e);
void reset_inactivity_timer(void);
void style_list_item(lv_obj_t * item);

#endif