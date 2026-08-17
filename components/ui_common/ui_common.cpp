#include "ui_common.h"

// Aqui a variável é criada na memória (sem o extern)
int32_t touch_start_x = 0;

// Aqui a função é implementada
void screen_pressed_cb(lv_event_t * e) {
    lv_indev_t * indev = lv_indev_active();
    if(indev) {
        lv_point_t p;
        lv_indev_get_point(indev, &p);
        touch_start_x = p.x; 
    }
}

void style_list_item(lv_obj_t * item) {
    if (item == NULL) return;
    lv_obj_set_style_bg_color(item, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(item, LV_OPA_COVER, 0);
    lv_obj_set_style_text_color(item, lv_color_white(), 0);
    lv_obj_set_style_border_width(item, 0, 0); 
    lv_obj_set_style_text_font(item, &lv_font_montserrat_20, 0); 
    lv_obj_set_style_pad_ver(item, 18, 0);  
    lv_obj_set_style_pad_column(item, 12, 0); 
}