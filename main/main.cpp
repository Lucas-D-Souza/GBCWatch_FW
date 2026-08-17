#include "esp_log.h"
#include "bsp/esp-bsp.h"
#include "lvgl.h"
#include "driver/gpio.h"
#include "SdUsbManager.hpp"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "bsp_board_extra.h"
#include "nvs_flash.h"
#include "esp_timer.h"
#include <dirent.h>
#include <string.h>
#include "display/lv_display_private.h"

extern "C" {
    #include "gnuboy.h"
    #include "cpu.h"
    #include "hw.h"
    #include "lcd.h"

    // --- STUBS DE ÁUDIO PARA ENGANAR O LINKER ---
    uint8_t audio_read(uint16_t addr) { return 0xFF; }
    void audio_write(uint16_t addr, uint8_t val) { }
    void audio_callback(void *buffer, size_t length) { }
    void audio_init() { }
}

extern "C" {
    #include "gnuboy.h"
    #include "cpu.h"
    #include "hw.h"
    #include "lcd.h"
}

static const char *TAG = "GBC_OS";

// ==========================================
// VARIÁVEIS GLOBAIS DO EMULADOR
// ==========================================
static uint8_t* emu_rom_buffer = nullptr;
static uint16_t* gnuboy_fb = nullptr;       

#define CHUNK_LINES 16  
#define SCALED_CHUNK_LINES (CHUNK_LINES * 2) 
static uint16_t* dma_buffer[2] = {nullptr, nullptr};
static uint8_t current_buf = 0;

static volatile bool emu_running = false;
static TaskHandle_t emu_task_handle = NULL;

static lv_obj_t *scr_menu = NULL;
static lv_obj_t *scr_play = NULL;
static lv_obj_t *list_roms = NULL;

// ==========================================
// FUNÇÕES DE HARDWARE E LVGL
// ==========================================
static void clear_i2c_bus(void) {
    gpio_config_t io_conf = {};
    io_conf.intr_type = GPIO_INTR_DISABLE;
    io_conf.mode = GPIO_MODE_OUTPUT_OD;
    io_conf.pin_bit_mask = (1ULL << GPIO_NUM_14) | (1ULL << GPIO_NUM_15);
    io_conf.pull_up_en = GPIO_PULLUP_ENABLE;
    gpio_config(&io_conf);

    gpio_set_level(GPIO_NUM_15, 1); esp_rom_delay_us(100);
    for (int i = 0; i < 9; i++) {
        gpio_set_level(GPIO_NUM_14, 0); esp_rom_delay_us(100);
        gpio_set_level(GPIO_NUM_14, 1); esp_rom_delay_us(100);
    }
    gpio_set_level(GPIO_NUM_15, 0); esp_rom_delay_us(100);
    gpio_set_level(GPIO_NUM_14, 1); esp_rom_delay_us(100);
    gpio_set_level(GPIO_NUM_15, 1); esp_rom_delay_us(100);

    gpio_reset_pin(GPIO_NUM_14);
    gpio_reset_pin(GPIO_NUM_15);
}

// ==========================================
// O CORAÇÃO DO EMULADOR (TAREFA DE DMA A 60 FPS)
// ==========================================
static void emulator_task(void* arg) {
    uint32_t last_fps_tick = xthal_get_ccount();
    uint32_t frames = 0;

    while (emu_running) {
        
        // --- 1. LEITURA DOS BOTÕES VIRTUAIS ---
        static int gpad = 0; 
        if (lvgl_port_lock(0)) {
            gpad = 0; 
            lv_indev_t * indev = lv_indev_get_next(NULL);
            if (indev && lv_indev_get_state(indev) == LV_INDEV_STATE_PRESSED) {
                lv_point_t p;
                lv_indev_get_point(indev, &p);
                int32_t tx = p.x;
                int32_t ty = p.y;
                
                // Mapeamento invisível na tela
                if (tx < 100 && ty > 80 && ty < 240) {
                    if (ty < 160) gpad |= GB_PAD_START; 
                    else gpad |= GB_PAD_SELECT;         
                }
                else if (tx < 200 && ty > 250) { 
                    int center_x = 87, center_y = 402; 
                    if (tx < center_x - 22) gpad |= GB_PAD_LEFT;
                    else if (tx > center_x + 22) gpad |= GB_PAD_RIGHT;
                    if (ty < center_y - 22) gpad |= GB_PAD_UP;
                    else if (ty > center_y + 22) gpad |= GB_PAD_DOWN;
                }
                else if (tx >= 200 && ty > 250) {
                    if (tx > 310) gpad |= GB_PAD_A; 
                    else gpad |= GB_PAD_B;
                }
            }
            lvgl_port_unlock();
        }
        gnuboy_set_pad(gpad);
        
        // --- 2. EXECUTA 1 FRAME LÓGICO ---
        gnuboy_run(1); 
        
        // --- 3. RENDERIZAÇÃO DIRETA VIA DMA (UPSCALING 2x) ---
        for (int chunk = 0; chunk < (144 / CHUNK_LINES); chunk++) {
            int src_y = chunk * CHUNK_LINES;
            
            for (int y = 0; y < CHUNK_LINES; y++) {
                uint32_t *src_row_32 = (uint32_t*)&gnuboy_fb[(src_y + y) * 160];
                uint32_t *dst_row1 = (uint32_t*)dma_buffer[current_buf] + (y * 2) * 160;
                uint32_t *dst_row2 = (uint32_t*)dma_buffer[current_buf] + (y * 2 + 1) * 160;

                for (int x = 0; x < 80; x++) {
                    uint32_t dual_pixel = src_row_32[x]; 
                    
                    uint16_t pix_a = (uint16_t)(dual_pixel & 0xFFFF);
                    uint16_t pix_b = (uint16_t)(dual_pixel >> 16);
                    
                    uint32_t color_a_32 = (pix_a << 16) | pix_a;
                    uint32_t color_b_32 = (pix_b << 16) | pix_b;
                    
                    int dst_idx = x * 2;
                    dst_row1[dst_idx] = color_a_32;
                    dst_row1[dst_idx + 1] = color_b_32;
                    
                    dst_row2[dst_idx] = color_a_32;
                    dst_row2[dst_idx + 1] = color_b_32;
                }
            }

            lv_area_t area;
            area.x1 = 45; area.y1 = 15 + (chunk * SCALED_CHUNK_LINES);
            area.x2 = area.x1 + 320 - 1; area.y2 = area.y1 + SCALED_CHUNK_LINES - 1;

            if (lvgl_port_lock(pdMS_TO_TICKS(10))) {
                lv_display_t * disp = lv_display_get_default();
                if (disp && disp->flush_cb) {
                    disp->flush_cb(disp, &area, (uint8_t*)dma_buffer[current_buf]);
                }
                lvgl_port_unlock();
            }
            current_buf = !current_buf; 
            vTaskDelay(pdMS_TO_TICKS(2)); // Alimenta o Watchdog enquanto o DMA envia a imagem
        }
        
        frames++;
        uint32_t now = xthal_get_ccount();
        if ((now - last_fps_tick) >= 240000000) {  
            ESP_LOGI(TAG, "Gnuboy FPS: %lu", frames);
            frames = 0;
            last_fps_tick = now;
        }

        // Descanso perfeito para cravar 60FPS (~16.6ms) sem Busy-Wait!
        vTaskDelay(pdMS_TO_TICKS(2));
    }
    
    emu_task_handle = NULL;
    vTaskDelete(NULL);
}

// ==========================================
// CONTROLE DO JOGO
// ==========================================
static void start_game(const char* path) {
    if (emu_running) return;

    // Aloca os buffers de vídeo direto na SRAM interna!
    if (!dma_buffer[0]) dma_buffer[0] = (uint16_t*)heap_caps_malloc(320 * SCALED_CHUNK_LINES * 2, MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA);
    if (!dma_buffer[1]) dma_buffer[1] = (uint16_t*)heap_caps_malloc(320 * SCALED_CHUNK_LINES * 2, MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA);
    if (!gnuboy_fb) gnuboy_fb = (uint16_t*)heap_caps_malloc(160 * 144 * 2, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);

    FILE* f = fopen(path, "rb");
    if (!f) return;
    fseek(f, 0, SEEK_END);
    size_t rom_size = ftell(f);
    rewind(f);
    
    size_t padded_size = (rom_size + 16383) & ~16383; 
    
    // A ROM é a única coisa que vai pra lenta PSRAM (pois pode ter até 4MB)
    emu_rom_buffer = (uint8_t*)heap_caps_calloc(1, padded_size, MALLOC_CAP_SPIRAM);
    fread(emu_rom_buffer, 1, rom_size, f);
    fclose(f);

    ESP_LOGI(TAG, "Iniciando Gnuboy...");
    
    // Inicia sem áudio por enquanto
    if (gnuboy_init(0, GB_AUDIO_STEREO_S16, GB_PIXEL_565_LE, NULL, NULL) != 0) return;

    gnuboy_set_framebuffer(gnuboy_fb);
    gnuboy_load_rom(emu_rom_buffer, rom_size);
    gnuboy_reset(true);
    
    emu_running = true; 
    xTaskCreatePinnedToCore(emulator_task, "emu_task", 16384, NULL, 5, &emu_task_handle, 1);
}

static void stop_game() {
    if (!emu_running) return;
    
    emu_running = false; 
    while (emu_task_handle != NULL) { vTaskDelay(pdMS_TO_TICKS(10)); }

    gnuboy_free_rom();

    if (dma_buffer[0]) { heap_caps_free(dma_buffer[0]); dma_buffer[0] = nullptr; }
    if (dma_buffer[1]) { heap_caps_free(dma_buffer[1]); dma_buffer[1] = nullptr; }
    if (gnuboy_fb) { heap_caps_free(gnuboy_fb); gnuboy_fb = nullptr; }
    if (emu_rom_buffer) { heap_caps_free(emu_rom_buffer); emu_rom_buffer = nullptr; }

    if (lvgl_port_lock(pdMS_TO_TICKS(100))) {
        lv_scr_load_anim(scr_menu, LV_SCR_LOAD_ANIM_NONE, 0, 0, false);
        lv_obj_invalidate(lv_scr_act()); 
        lvgl_port_unlock();
    }
}

// ==========================================
// INTERFACE LVGL 
// ==========================================
static void rom_click_cb(lv_event_t * e) {
    const char * path = (const char *)lv_event_get_user_data(e);
    lv_scr_load_anim(scr_play, LV_SCR_LOAD_ANIM_NONE, 0, 0, false);
    
    static char delayed_path[256];
    strncpy(delayed_path, path, sizeof(delayed_path));
    
    lv_timer_create([](lv_timer_t *t){
        start_game(delayed_path);
        lv_timer_delete(t);
    }, 100, NULL);
}

static void build_ui() {
    // TELA PRINCIPAL (LISTA)
    scr_menu = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(scr_menu, lv_color_black(), 0);
    
    lv_obj_t * title = lv_label_create(scr_menu);
    lv_label_set_text(title, "Game Boy Color");
    lv_obj_set_style_text_color(title, lv_color_white(), 0); 
    lv_obj_set_style_text_font(title, &lv_font_montserrat_20, 0);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 30);

    list_roms = lv_list_create(scr_menu);
    lv_obj_set_size(list_roms, 320, 360);
    lv_obj_align(list_roms, LV_ALIGN_BOTTOM_MID, 0, -20);
    lv_obj_set_style_bg_color(list_roms, lv_color_black(), 0);
    lv_obj_set_style_border_width(list_roms, 0, 0);

    // TELA DE JOGO (BOTÕES INVISÍVEIS)
    scr_play = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(scr_play, lv_color_black(), 0);
    lv_obj_remove_flag(scr_play, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *btn_exit = lv_label_create(scr_play);
    lv_label_set_text(btn_exit, LV_SYMBOL_CLOSE);
    lv_obj_set_style_text_color(btn_exit, lv_color_hex(0xFF3333), 0);
    lv_obj_set_style_text_font(btn_exit, &lv_font_montserrat_20, 0);
    lv_obj_align(btn_exit, LV_ALIGN_TOP_LEFT, 20, 20);

    lv_obj_add_flag(btn_exit, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_ext_click_area(btn_exit, 30); 
    lv_obj_add_event_cb(btn_exit, [](lv_event_t *e) {
        stop_game();
    }, LV_EVENT_CLICKED, NULL);

    // D-PAD Visível (translúcido)
    lv_obj_t * dpad_v = lv_obj_create(scr_play);
    lv_obj_set_size(dpad_v, 44, 140);
    lv_obj_align(dpad_v, LV_ALIGN_BOTTOM_LEFT, 65, -30);  
    lv_obj_set_style_bg_color(dpad_v, lv_color_hex(0x222222), 0);
    lv_obj_set_style_border_width(dpad_v, 0, 0);
    lv_obj_set_style_bg_opa(dpad_v, LV_OPA_50, 0);
    
    lv_obj_t * dpad_h = lv_obj_create(scr_play);
    lv_obj_set_size(dpad_h, 140, 44);
    lv_obj_align(dpad_h, LV_ALIGN_BOTTOM_LEFT, 17, -78); 
    lv_obj_set_style_bg_color(dpad_h, lv_color_hex(0x222222), 0);
    lv_obj_set_style_border_width(dpad_h, 0, 0);
    lv_obj_set_style_bg_opa(dpad_h, LV_OPA_50, 0);

    // Botões A e B
    lv_obj_t * btn_a = lv_obj_create(scr_play);
    lv_obj_set_size(btn_a, 70, 70);
    lv_obj_align(btn_a, LV_ALIGN_BOTTOM_RIGHT, -20, -100);
    lv_obj_set_style_radius(btn_a, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(btn_a, lv_color_hex(0x9b2256), 0);
    lv_obj_set_style_bg_opa(btn_a, LV_OPA_70, 0);
    lv_obj_set_style_border_width(btn_a, 0, 0);
    lv_obj_t * lbl_a = lv_label_create(btn_a);
    lv_label_set_text(lbl_a, "A");
    lv_obj_center(lbl_a);

    lv_obj_t * btn_b = lv_obj_create(scr_play);
    lv_obj_set_size(btn_b, 70, 70);
    lv_obj_align(btn_b, LV_ALIGN_BOTTOM_RIGHT, -100, -40); 
    lv_obj_set_style_radius(btn_b, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(btn_b, lv_color_hex(0x9b2256), 0);
    lv_obj_set_style_bg_opa(btn_b, LV_OPA_70, 0);
    lv_obj_set_style_border_width(btn_b, 0, 0);
    lv_obj_t * lbl_b = lv_label_create(btn_b);
    lv_label_set_text(lbl_b, "B");
    lv_obj_center(lbl_b);
}

static void refresh_rom_list() {
    lv_obj_clean(list_roms);
    DIR *dir = opendir("/sdcard/GB");
    if (dir) {
        struct dirent *ent;
        while ((ent = readdir(dir)) != NULL) {
            if (strstr(ent->d_name, ".gb") || strstr(ent->d_name, ".gbc")) {
                size_t path_len = strlen("/sdcard/GB/") + strlen(ent->d_name) + 1;
                char *full_path = (char*)malloc(path_len);
                snprintf(full_path, path_len, "/sdcard/GB/%s", ent->d_name);
                
                // MÁGICA: Lê o cabeçalho do arquivo para pegar o nome real!
                char game_title[17] = {0};
                FILE* f = fopen(full_path, "rb");
                if (f) {
                    fseek(f, 0x0134, SEEK_SET);
                    fread(game_title, 1, 16, f);
                    fclose(f);
                }
                if (strlen(game_title) == 0) strcpy(game_title, ent->d_name); // Fallback

                lv_obj_t *btn = lv_list_add_button(list_roms, LV_SYMBOL_PLAY, game_title);
                
                // Estilo da Lista LVGL v9
                lv_obj_set_style_bg_color(btn, lv_color_hex(0x111111), 0);
                lv_obj_set_style_text_color(btn, lv_color_white(), 0);
                lv_obj_set_style_border_width(btn, 0, 0);
                lv_obj_set_style_pad_all(btn, 20, 0);
                
                lv_obj_add_event_cb(btn, rom_click_cb, LV_EVENT_CLICKED, full_path);
            }
        }
        closedir(dir);
    } else {
        lv_obj_t *lbl = lv_list_add_text(list_roms, "Crie a pasta /GB e adicione jogos.");
        lv_obj_set_style_text_color(lbl, lv_color_white(), 0);
        lv_obj_set_style_bg_color(lbl, lv_color_black(), 0);
    }
}

extern "C" void app_main(void) {
    clear_i2c_bus();

    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        nvs_flash_erase();
        nvs_flash_init();
    }

    bsp_display_start();
    bsp_display_lock(0);
    lv_obj_set_style_bg_color(lv_screen_active(), lv_color_black(), 0);
    bsp_display_unlock();
    vTaskDelay(pdMS_TO_TICKS(100)); 
    bsp_display_brightness_set(80);

    SdUsbManager::get_instance().init_local_storage();

    if (bsp_display_lock(pdMS_TO_TICKS(100))) {
        build_ui();
        refresh_rom_list();
        lv_screen_load(scr_menu);
        bsp_display_unlock();
    }

    ESP_LOGI(TAG, "GBC_OS Pronto a 240MHz!");
    
    while(1) {
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}