#include "esp_log.h"
#include "bsp/esp-bsp.h"
#include "lvgl.h"
#include "driver/gpio.h"
#include "SdUsbManager.hpp"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/ringbuf.h"
#include "bsp_board_extra.h"
#include "nvs_flash.h"
#include "esp_timer.h"
#include <dirent.h>
#include <string.h>
#include "display/lv_display_private.h"
#include "esp_ota_ops.h"
#include "esp_partition.h"
#include "driver/i2c_master.h"

LV_IMAGE_DECLARE(icon_gbc); // Declara a imagem que você já tem compilada
static lv_obj_t * scr_splash = NULL;

static void show_splash_screen(const char* version) {
    // 1. Cria a tela de Splash
    scr_splash = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(scr_splash, lv_color_black(), 0);
    lv_obj_remove_flag(scr_splash, LV_OBJ_FLAG_SCROLLABLE);

    // 2. Container Transparente (Age como uma "Caixa" centralizando os itens juntos)
    lv_obj_t * cont_center = lv_obj_create(scr_splash);
    lv_obj_set_size(cont_center, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_align(cont_center, LV_ALIGN_CENTER, 0, -30);
    lv_obj_set_style_bg_opa(cont_center, LV_OPA_TRANSP, 0); // Transparente
    lv_obj_set_style_border_width(cont_center, 0, 0); // Sem bordas
    
    // Configura o Flexbox: Coloca os itens Lado a Lado (Ícone na Esquerda, Texto na Direita)
    lv_obj_set_flex_flow(cont_center, LV_FLEX_FLOW_ROW); 
    lv_obj_set_flex_align(cont_center, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_all(cont_center, 0, 0);
    lv_obj_set_style_pad_column(cont_center, 25, 0); // Espaço de 25px entre o Ícone e o Texto

    // 3. Ícone do GBC
    lv_obj_t * logo = lv_image_create(cont_center);
    lv_image_set_src(logo, &icon_gbc);
    
    // TRUQUE LVGL v9: Escala a imagem via hardware para dobrar de tamanho (256 = 100%, 512 = 200%)
    // O seu icone de 50x50 vai ser renderizado como 100x100 pixels!
    lv_image_set_scale(logo, 512); 

    // 4. Texto GIGANTE do App
    lv_obj_t * title = lv_label_create(cont_center);
    // Usamos a quebra de linha (\n) para o texto caber enorme sem estourar a tela
    lv_label_set_text(title, "Game Boy\nColor"); 
    lv_obj_set_style_text_font(title, &lv_font_montserrat_30, 0); 
    lv_obj_set_style_text_color(title, lv_color_white(), 0);

    // 5. Versão no rodapé
    lv_obj_t * lbl_version = lv_label_create(scr_splash);
    lv_label_set_text_fmt(lbl_version, "v%s", version);
    lv_obj_set_style_text_font(lbl_version, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_color(lbl_version, lv_color_hex(0x555555), 0); // Cinza discreto
    lv_obj_align(lbl_version, LV_ALIGN_BOTTOM_MID, 0, -25);

    // 6. Joga imediatamente na tela sem animação
    lv_screen_load(scr_splash);
}

// Globais da Bateria
static lv_obj_t *icon_batt_warning = NULL;
static bool has_low_batt_saved = false;

#define BOOT_BTN_PIN GPIO_NUM_0

// Variáveis Globais de Áudio
static bool audio_enabled = true; 
static RingbufHandle_t audio_ringbuf = NULL;

extern "C" {
    #include "gnuboy.h"
    #include "cpu.h"
    #include "hw.h"
    #include "lcd.h"
    #include "minigb_apu.h"

    // Ponte real de áudio do Gnuboy
    uint8_t audio_read(uint16_t addr) { return audio_enabled ? minigb_audio_read(addr) : 0xFF; }
    void audio_write(uint16_t addr, uint8_t val) { if(audio_enabled) minigb_audio_write(addr, val); }
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

// Variáveis de Áudio
static int current_volume = 50;
static TaskHandle_t audio_task_handle = NULL;

// Variável para guardar o caminho do jogo e criar o .sav
static char current_rom_path[256] = "";

// ==========================================
// FUNÇÕES DE HARDWARE E LVGL
// ==========================================
static void audio_drain_task(void *arg) {
    size_t bytes_written;
    while (emu_running && audio_enabled) {
        size_t item_size;
        uint8_t *data = (uint8_t *)xRingbufferReceive(audio_ringbuf, &item_size, pdMS_TO_TICKS(100));
        if (data) {
            bsp_extra_i2s_write(data, item_size, &bytes_written, portMAX_DELAY);
            vRingbufferReturnItem(audio_ringbuf, (void *)data);
        }
    }
    audio_task_handle = NULL; 
    vTaskDelete(NULL);
}

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

static uint8_t read_battery_percentage(void) {
    i2c_master_bus_handle_t i2c_bus = bsp_i2c_get_handle();
    if (!i2c_bus) return 100; // Retorna 100% caso o barramento não esteja pronto
    
    i2c_device_config_t pmu_cfg = {};
    pmu_cfg.dev_addr_length = I2C_ADDR_BIT_LEN_7;
    pmu_cfg.device_address = 0x34; // Endereço do AXP2101
    pmu_cfg.scl_speed_hz = 400000;
    
    i2c_master_dev_handle_t pmu_handle;
    uint8_t batt_percent = 100;
    
    if (i2c_master_bus_add_device(i2c_bus, &pmu_cfg, &pmu_handle) == ESP_OK) {
        uint8_t reg_addr = 0xA4;
        i2c_master_transmit_receive(pmu_handle, &reg_addr, 1, &batt_percent, 1, 1000);
        i2c_master_bus_rm_device(pmu_handle);
    }
    return (batt_percent > 100) ? 100 : batt_percent;
}

// ==========================================
// O CORAÇÃO DO EMULADOR (TAREFA DE DMA A 60 FPS)
// ==========================================
static void emulator_task(void* arg) {
    uint32_t last_fps_tick = xthal_get_ccount();
    uint32_t frames_logic = 0;
    uint32_t frames_draw = 0;
    uint64_t next_frame_target_us = esp_timer_get_time(); 

    while (emu_running) {
        
        // --- 1. LEITURA DOS BOTÕES ---
        static int gpad = 0; 
        if (lvgl_port_lock(0)) {
            gpad = 0; 
            lv_indev_t * indev = lv_indev_get_next(NULL);
            if (indev && lv_indev_get_state(indev) == LV_INDEV_STATE_PRESSED) {
                lv_point_t p;
                lv_indev_get_point(indev, &p);
                int32_t tx = p.x, ty = p.y;
                if (tx < 100 && ty > 80 && ty < 240) {
                    if (ty < 160) gpad |= GB_PAD_START; 
                    else gpad |= GB_PAD_SELECT;         
                } else if (tx < 200 && ty > 250) { 
                    int cx = 87, cy = 402; 
                    if (tx < cx - 22) gpad |= GB_PAD_LEFT;
                    else if (tx > cx + 22) gpad |= GB_PAD_RIGHT;
                    if (ty < cy - 22) gpad |= GB_PAD_UP;
                    else if (ty > cy + 22) gpad |= GB_PAD_DOWN;
                } else if (tx >= 200 && ty > 250) {
                    if (tx > 310) gpad |= GB_PAD_A; 
                    else gpad |= GB_PAD_B;
                }
            }
            lvgl_port_unlock();
        }
        gnuboy_set_pad(gpad);
        
        // --- 2. FRAMESKIP (60 Lógica / 30 Visual) ---
        static uint8_t frame_skip = 0;
        bool draw_this_frame = ((++frame_skip) % 2 == 0); 

        gnuboy_run(draw_this_frame); 
        
        // --- 3. GERAÇÃO DE ÁUDIO ---
        if (audio_enabled) {
            // O assert pede estritamente este cálculo em bytes
            const int req_sz = AUDIO_SAMPLES * 2 * sizeof(int16_t);
            static uint8_t sample_buffer[4096]; 

            audio_callback(NULL, sample_buffer, req_sz);
            
            if (audio_ringbuf) {
                // Time-out rápido (2ms) igual ao factory para não travar a DMA de vídeo
                xRingbufferSend(audio_ringbuf, sample_buffer, req_sz, pdMS_TO_TICKS(2));
            }
        }

        // --- 4. RENDERIZAÇÃO DE VÍDEO (Pula 1 Quadro) ---
        if (draw_this_frame) {
            for (int chunk = 0; chunk < (144 / CHUNK_LINES); chunk++) {
                int src_y = chunk * CHUNK_LINES;
                for (int y = 0; y < CHUNK_LINES; y++) {
                    uint32_t *src_row_32 = (uint32_t*)&gnuboy_fb[(src_y + y) * 160];
                    uint32_t *dst_row1 = (uint32_t*)dma_buffer[current_buf] + (y * 2) * 160;
                    uint32_t *dst_row2 = (uint32_t*)dma_buffer[current_buf] + (y * 2 + 1) * 160;

                    for (int x = 0; x < 80; x++) {
                        uint32_t dp = src_row_32[x]; 
                        uint32_t ca = ((dp & 0xFFFF) << 16) | (dp & 0xFFFF);
                        uint32_t cb = ((dp >> 16) << 16) | (dp >> 16);
                        int dst_idx = x * 2;
                        dst_row1[dst_idx] = ca; dst_row1[dst_idx + 1] = cb;
                        dst_row2[dst_idx] = ca; dst_row2[dst_idx + 1] = cb;
                    }
                }
                lv_area_t area = {
                    .x1 = 45, .y1 = 15 + (chunk * SCALED_CHUNK_LINES),
                    .x2 = 45 + 320 - 1, .y2 = 15 + (chunk * SCALED_CHUNK_LINES) + SCALED_CHUNK_LINES - 1
                };
                if (lvgl_port_lock(pdMS_TO_TICKS(10))) {
                    lv_display_t * disp = lv_display_get_default();
                    if (disp && disp->flush_cb) disp->flush_cb(disp, &area, (uint8_t*)dma_buffer[current_buf]);
                    lvgl_port_unlock();
                }
                current_buf = !current_buf; 
            }
            frames_draw++;
        }
        
        frames_logic++;
        uint32_t now = xthal_get_ccount();
        if ((now - last_fps_tick) >= 240000000) {  
            ESP_LOGI(TAG, "FPS: %lu Lógico / %lu Visual", frames_logic, frames_draw);
            frames_logic = frames_draw = 0;
            last_fps_tick = now;

            // --- NOVO: MONITORAMENTO DE BATERIA (A cada 30 segundos) ---
            static uint8_t sec_counter = 30; // Começa em 30 para verificar imediatamente no boot do jogo
            sec_counter++;

            if (sec_counter >= 30) {
                sec_counter = 0; // Reseta o contador
                uint8_t current_battery_pct = read_battery_percentage();

                // current_battery_pct = 25;

                if (current_battery_pct <= 30) {
                    
                    // 1. Gatilho de Auto-Save (Executa ANTES de mudar a interface)
                    if (current_battery_pct <= 10 && !has_low_batt_saved) {
                        ESP_LOGW(TAG, "Nível crítico (10%%)! Salvando SRAM automaticamente...");
                        if (gnuboy_sram_dirty()) {
                            char sav_path[256];
                            snprintf(sav_path, sizeof(sav_path), "%s", current_rom_path);
                            char *ext = strrchr(sav_path, '.');
                            if (ext) strcpy(ext, ".sav");
                            gnuboy_save_sram(sav_path, false);
                        }
                        has_low_batt_saved = true;
                    }

                    // 2. Atualiza a Interface Gráfica de uma vez só com o estado final
                    if (lvgl_port_lock(0)) {
                        lv_obj_remove_flag(icon_batt_warning, LV_OBJ_FLAG_HIDDEN);
                        
                        if (current_battery_pct <= 10) {
                            lv_obj_set_style_text_color(icon_batt_warning, lv_color_hex(0xFF3333), 0); // Vermelho
                            lv_label_set_text(icon_batt_warning, LV_SYMBOL_BATTERY_EMPTY " 10% - Jogo Salvo!");
                        } else {
                            lv_obj_set_style_text_color(icon_batt_warning, lv_color_hex(0xFFD700), 0); // Amarelo
                            lv_label_set_text(icon_batt_warning, LV_SYMBOL_BATTERY_EMPTY " Bateria Fraca!");
                        }
                        lvgl_port_unlock();
                    }

                } else {
                    // Bateria segura: Esconde o ícone
                    if (lvgl_port_lock(0)) {
                        lv_obj_add_flag(icon_batt_warning, LV_OBJ_FLAG_HIDDEN);
                        lvgl_port_unlock();
                    }
                }
            }
            // --------------------------------------
        }

        // --- 5. MARCAPASSO DOS 60 FPS (16.74ms do Gameboy) ---
        next_frame_target_us += 16742; 
        uint64_t current_time_us = esp_timer_get_time();
        
        if (current_time_us < next_frame_target_us) {
            uint32_t delay_us = next_frame_target_us - current_time_us;
            if (delay_us > 1000) {
                // Emulador está rápido: Devolve a sobra de tempo pro FreeRTOS (Alimenta Watchdog)
                vTaskDelay(pdMS_TO_TICKS(delay_us / 1000));
            } else {
                taskYIELD();
            }
        } else {
            // Emulador está Atrasado/Pesado!
            // APLICAÇÃO DA SUA CORREÇÃO: Força a CPU1 a respirar a cada 10 frames lógicos
            // Isso impede que o CPU1 monopolize o Ringbuffer e cause o Timeout no CPU0
            if (frames_logic % 10 == 0) {
                vTaskDelay(pdMS_TO_TICKS(1));
            } else {
                taskYIELD(); 
            }

            // Impede que a meta de tempo fique inalcançável (evita a espiral da morte)
            if (current_time_us - next_frame_target_us > 33484) {
                next_frame_target_us = current_time_us; 
            }
        }
    }
    emu_task_handle = NULL;
    vTaskDelete(NULL);
}

// ==========================================
// CONTROLE DO JOGO
// ==========================================
static void start_game(const char* path) {
    if (emu_running) return;

    strncpy(current_rom_path, path, sizeof(current_rom_path));

    // Aloca os buffers de vídeo na SRAM interna
    if (!dma_buffer[0]) dma_buffer[0] = (uint16_t*)heap_caps_malloc(320 * SCALED_CHUNK_LINES * 2, MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA);
    if (!dma_buffer[1]) dma_buffer[1] = (uint16_t*)heap_caps_malloc(320 * SCALED_CHUNK_LINES * 2, MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA);
    if (!gnuboy_fb) gnuboy_fb = (uint16_t*)heap_caps_malloc(160 * 144 * 2, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    
    // --- LÓGICA EXATA DO FACTORY FIRMWARE ---
    if (audio_enabled) {
        static bool i2s_initialized = false;
        if (!i2s_initialized) {
            bsp_extra_codec_init();
            bsp_extra_codec_set_fs(44100, 16, I2S_SLOT_MODE_STEREO); // Frequência que o DAC aceita
            i2s_initialized = true;
        }
        bsp_extra_codec_mute_set(false);
        bsp_extra_codec_volume_set(current_volume, NULL);
        
        if (!audio_ringbuf) {
            audio_ringbuf = xRingbufferCreate(8192, RINGBUF_TYPE_BYTEBUF);
        }
    }

    FILE* f = fopen(path, "rb");
    if (!f) return;
    fseek(f, 0, SEEK_END);
    size_t rom_size = ftell(f);
    rewind(f);
    
    size_t padded_size = (rom_size + 16383) & ~16383; 
    emu_rom_buffer = (uint8_t*)heap_caps_calloc(1, padded_size, MALLOC_CAP_SPIRAM);
    fread(emu_rom_buffer, 1, rom_size, f);
    fclose(f);

    ESP_LOGI(TAG, "Iniciando Gnuboy a 44100Hz...");
    
    // APU MiniGB inicializada AQUI (depois do Codec, antes do Gnuboy)
    if (audio_enabled) {
        audio_init();
    }

    // Inicializa motor Gnuboy em 44100Hz
    if (gnuboy_init(44100, GB_AUDIO_STEREO_S16, GB_PIXEL_565_LE, NULL, NULL) != 0) return;

    gnuboy_set_framebuffer(gnuboy_fb);
    gnuboy_load_rom(emu_rom_buffer, rom_size);
    gnuboy_reset(true);

    // --- NOVO: LÓGICA DE CARREGAMENTO DO .SAV ---
    char sav_path[256];
    snprintf(sav_path, sizeof(sav_path), "%s", current_rom_path);
    char *ext = strrchr(sav_path, '.');
    if (ext) strcpy(ext, ".sav"); // Troca .gb/.gbc por .sav

    gnuboy_load_sram(sav_path); // O Gnuboy trata silenciosamente se o arquivo não existir
    ESP_LOGI(TAG, "Save Game carregado (se existente): %s", sav_path);
    // ---------------------------------------------
    
    emu_running = true; 

    has_low_batt_saved = false;
    
    // Inicia as tasks usando os níveis do Factory Firmware
    if (audio_enabled) { 
        xTaskCreatePinnedToCore(audio_drain_task, "audio_drain", 4096, NULL, 5, &audio_task_handle, 0); 
    }
    xTaskCreatePinnedToCore(emulator_task, "emu_task", 16384, NULL, 5, &emu_task_handle, 1);
}

static void stop_game() {
    if (!emu_running) return;

    // 1. Sinaliza para TODAS as tasks pararem Imediatamente
    emu_running = false; 

    // 2. Aguarda a task do emulador (Vídeo/Lógica) terminar de forma segura
    while (emu_task_handle != NULL) { 
        vTaskDelay(pdMS_TO_TICKS(10)); 
    }

    // 3. Aguarda a task de áudio terminar de ler o buffer e fechar
    if (audio_enabled) {
        while (audio_task_handle != NULL) { 
            vTaskDelay(pdMS_TO_TICKS(10)); 
        }
    }

    // 4. AGORA É SEGURO DELETAR O RINGBUFFER E MUDAR O HARDWARE
    if (audio_enabled) {
        bsp_extra_codec_mute_set(true); 
        if (audio_ringbuf) { 
            vRingbufferDelete(audio_ringbuf); 
            audio_ringbuf = NULL; 
        }
    }

    // Lógica gravação do .sav
    if (gnuboy_sram_dirty()) {
        char sav_path[256];
        snprintf(sav_path, sizeof(sav_path), "%s", current_rom_path);
        char *ext = strrchr(sav_path, '.');
        if (ext) strcpy(ext, ".sav");
        
        gnuboy_save_sram(sav_path, false);
        ESP_LOGI(TAG, "Progresso salvo com sucesso em: %s", sav_path);
    } else {
        ESP_LOGI(TAG, "Nenhuma alteracao na SRAM. Save ignorado.");
    }

    // 5. Libera a memória da ROM e dos Buffers de Vídeo
    gnuboy_free_rom();

    if (dma_buffer[0]) { heap_caps_free(dma_buffer[0]); dma_buffer[0] = nullptr; }
    if (dma_buffer[1]) { heap_caps_free(dma_buffer[1]); dma_buffer[1] = nullptr; }
    if (gnuboy_fb) { heap_caps_free(gnuboy_fb); gnuboy_fb = nullptr; }
    if (emu_rom_buffer) { heap_caps_free(emu_rom_buffer); emu_rom_buffer = nullptr; }

    // 6. Devolve o controle para a interface Gráfica (Menu)
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
    lv_obj_align(title, LV_ALIGN_TOP_LEFT, 50, 20);

    // --- NOVO: BOTÃO (SWITCH) DE ÁUDIO NO MENU ---
    lv_obj_t * sw_audio = lv_switch_create(scr_menu);
    lv_obj_align(sw_audio, LV_ALIGN_TOP_RIGHT, -50, 20);
    if(audio_enabled) lv_obj_add_state(sw_audio, LV_STATE_CHECKED);
    lv_obj_add_event_cb(sw_audio, [](lv_event_t *e) {
        lv_obj_t * sw = (lv_obj_t *)lv_event_get_target(e);
        audio_enabled = lv_obj_has_state(sw, LV_STATE_CHECKED);
    }, LV_EVENT_VALUE_CHANGED, NULL);
    
    lv_obj_t * lbl_audio = lv_label_create(scr_menu);
    lv_label_set_text(lbl_audio, LV_SYMBOL_AUDIO);
    lv_obj_set_style_text_color(lbl_audio, lv_color_white(), 0);
    lv_obj_align_to(lbl_audio, sw_audio, LV_ALIGN_OUT_LEFT_MID, -10, 0);

    // LISTA MAIS LARGA
    list_roms = lv_list_create(scr_menu);
    lv_obj_set_size(list_roms, 400, 380);
    lv_obj_align(list_roms, LV_ALIGN_BOTTOM_MID, 0, -20);
    lv_obj_set_style_bg_color(list_roms, lv_color_black(), 0);
    lv_obj_set_style_border_width(list_roms, 0, 0);

    // TELA DE JOGO 
    scr_play = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(scr_play, lv_color_black(), 0);
    lv_obj_remove_flag(scr_play, LV_OBJ_FLAG_SCROLLABLE);

    // Ícone de Aviso de Bateria (Oculto por Padrão)
    icon_batt_warning = lv_label_create(scr_play);
    lv_label_set_text(icon_batt_warning, LV_SYMBOL_BATTERY_EMPTY " Bateria Fraca!");
    lv_obj_set_style_text_color(icon_batt_warning, lv_color_hex(0xFFD700), 0); // Amarelo
    lv_obj_align(icon_batt_warning, LV_ALIGN_TOP_MID, 0, 0); // Fica no topo, no meio
    lv_obj_add_flag(icon_batt_warning, LV_OBJ_FLAG_HIDDEN);

    lv_obj_t *btn_exit = lv_label_create(scr_play);
    lv_label_set_text(btn_exit, LV_SYMBOL_CLOSE);
    lv_obj_set_style_text_color(btn_exit, lv_color_hex(0xEE2222), 0);
    lv_obj_set_style_text_font(btn_exit, &lv_font_montserrat_14, 0);
    lv_obj_align(btn_exit, LV_ALIGN_TOP_LEFT, 90, 0);
    lv_obj_add_flag(btn_exit, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_ext_click_area(btn_exit, 20); 
    lv_obj_add_event_cb(btn_exit, [](lv_event_t *e) { stop_game(); }, LV_EVENT_CLICKED, NULL);

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

    lv_obj_t * btn_a = lv_obj_create(scr_play);
    lv_obj_set_size(btn_a, 70, 70);
    lv_obj_align(btn_a, LV_ALIGN_BOTTOM_RIGHT, -20, -100);
    lv_obj_set_style_radius(btn_a, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(btn_a, lv_color_hex(0x9b2256), 0);
    lv_obj_set_style_bg_opa(btn_a, LV_OPA_70, 0);
    lv_obj_set_style_border_width(btn_a, 0, 0);
    lv_obj_t * lbl_a = lv_label_create(btn_a);
    lv_label_set_text(lbl_a, "A");
    lv_obj_set_style_text_color(lbl_a, lv_color_black(), 0);
    lv_obj_set_style_text_font(lbl_a, &lv_font_montserrat_30, 0);
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
    lv_obj_set_style_text_color(lbl_b, lv_color_black(), 0);
    lv_obj_set_style_text_font(lbl_b, &lv_font_montserrat_30, 0);
    lv_obj_center(lbl_b);

    lv_obj_t * btn_start = lv_obj_create(scr_play);
    lv_obj_set_size(btn_start, 40, 30); 
    lv_obj_align(btn_start, LV_ALIGN_LEFT_MID, 5, -130); 
    lv_obj_set_style_radius(btn_start, 10, 0);
    lv_obj_set_style_bg_color(btn_start, lv_color_hex(0x222222), 0);
    lv_obj_set_style_bg_opa(btn_start, LV_OPA_70, 0);
    lv_obj_set_style_border_width(btn_start, 0, 0);
    lv_obj_t * lbl_start = lv_label_create(btn_start);
    lv_label_set_text(lbl_start, "ST");
    lv_obj_set_style_text_color(lbl_start, lv_color_hex(0x666666), 0);
    lv_obj_center(lbl_start);

    lv_obj_t * btn_select = lv_obj_create(scr_play);
    lv_obj_set_size(btn_select, 40, 30); 
    lv_obj_align(btn_select, LV_ALIGN_LEFT_MID, 5, -30); 
    lv_obj_set_style_radius(btn_select, 10, 0);
    lv_obj_set_style_bg_color(btn_select, lv_color_hex(0x222222), 0);
    lv_obj_set_style_bg_opa(btn_select, LV_OPA_70, 0);
    lv_obj_set_style_border_width(btn_select, 0, 0);
    lv_obj_t * lbl_select = lv_label_create(btn_select);
    lv_label_set_text(lbl_select, "SL");
    lv_obj_set_style_text_color(lbl_select, lv_color_hex(0x666666), 0);
    lv_obj_center(lbl_select);

    lv_obj_t * btn_vol_up = lv_btn_create(scr_play);
    lv_obj_set_size(btn_vol_up, 35, 35);
    lv_obj_align(btn_vol_up, LV_ALIGN_TOP_RIGHT, -10, 60);
    lv_obj_set_style_bg_color(btn_vol_up, lv_color_hex(0x222222), 0);
    lv_obj_set_style_bg_opa(btn_vol_up, LV_OPA_70, 0);
    lv_obj_t * lbl_vup = lv_label_create(btn_vol_up);
    lv_label_set_text(lbl_vup, LV_SYMBOL_VOLUME_MAX);
    lv_obj_set_style_text_color(lbl_vup, lv_color_hex(0x666666), 0);
    lv_obj_center(lbl_vup);
    lv_obj_add_event_cb(btn_vol_up, [](lv_event_t *e) {
        if(!emu_running) return;
        if (current_volume < 100) current_volume += 10;
        bsp_extra_codec_volume_set(current_volume, NULL);
    }, LV_EVENT_CLICKED, NULL);

    lv_obj_t * btn_vol_down = lv_btn_create(scr_play);
    lv_obj_set_size(btn_vol_down, 35, 35);
    lv_obj_align(btn_vol_down, LV_ALIGN_TOP_RIGHT, -10, 110);
    lv_obj_set_style_bg_color(btn_vol_down, lv_color_hex(0x222222), 0);
    lv_obj_set_style_bg_opa(btn_vol_down, LV_OPA_70, 0);
    lv_obj_t * lbl_vdown = lv_label_create(btn_vol_down);
    lv_label_set_text(lbl_vdown, LV_SYMBOL_VOLUME_MID);
    lv_obj_set_style_text_color(lbl_vdown, lv_color_hex(0x666666), 0);
    lv_obj_center(lbl_vdown);
    lv_obj_add_event_cb(btn_vol_down, [](lv_event_t *e) {
        if(!emu_running) return;
        if (current_volume > 0) current_volume -= 10;
        bsp_extra_codec_volume_set(current_volume, NULL);
    }, LV_EVENT_CLICKED, NULL);
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
                
                // Usa diretamente o nome do arquivo (ent->d_name)
                lv_obj_t *btn = lv_list_add_button(list_roms, LV_SYMBOL_PLAY, ent->d_name);
                
                lv_obj_set_style_bg_color(btn, lv_color_black(), 0);
                lv_obj_set_style_text_color(btn, lv_color_white(), 0);
                lv_obj_set_style_text_font(btn, &lv_font_montserrat_20, 0);
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
        lv_obj_set_style_text_font(lbl, &lv_font_montserrat_20, 0);
    }
}

extern "C" void app_main(void) {
    clear_i2c_bus();

    esp_ota_mark_app_valid_cancel_rollback();

    // Configuração do Botão BOOT
    gpio_config_t io_conf = {};
    io_conf.intr_type = GPIO_INTR_DISABLE;
    io_conf.mode = GPIO_MODE_INPUT;
    io_conf.pin_bit_mask = (1ULL << BOOT_BTN_PIN);
    io_conf.pull_up_en = GPIO_PULLUP_ENABLE;
    gpio_config(&io_conf);

    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        nvs_flash_erase();
        nvs_flash_init();
    }

    bsp_display_start();
    
    if (bsp_display_lock(pdMS_TO_TICKS(100))) {
        // Chamamos a Splash passando a versão
        show_splash_screen("1.0.0");
        bsp_display_unlock();
    }
    vTaskDelay(pdMS_TO_TICKS(50));
    bsp_display_brightness_set(80);

    SdUsbManager::get_instance().init_local_storage();

    if (bsp_display_lock(pdMS_TO_TICKS(100))) {
        build_ui();
        refresh_rom_list();
        lv_scr_load_anim(scr_menu, LV_SCR_LOAD_ANIM_FADE_ON, 400, 1500, true);
        bsp_display_unlock();
    }

    ESP_LOGI(TAG, "GBC_OS Pronto a 240MHz!");
    
    // Loop principal da aplicação atuando como vigilante do botão BOOT
    while(1) {
        // Verifica se o botão foi pressionado (Nível Lógico Baixo)
        if (gpio_get_level(BOOT_BTN_PIN) == 0) {
            ESP_LOGI(TAG, "Botão BOOT pressionado! Retornando ao Factory Firmware...");

            // 1. Se o jogo estiver rodando, desliga graciosamente (Garante o Save do .sav!)
            if (emu_running) {
                stop_game();
            }

            // 2. Apaga a tela para dar feedback visual imediato ao usuário
            bsp_display_brightness_set(0); 

            // 3. Localiza a partição de fábrica original
            const esp_partition_t *factory_part = esp_partition_find_first(
                ESP_PARTITION_TYPE_APP, 
                ESP_PARTITION_SUBTYPE_APP_FACTORY, 
                NULL
            );
            
            // 4. Altera o ponteiro do Bootloader e reinicia
            if (factory_part) {
                esp_ota_set_boot_partition(factory_part);
                vTaskDelay(pdMS_TO_TICKS(500));
                esp_restart();
            }
        }
        
        // Verifica a cada 100ms para manter a responsividade sem gastar CPU
        vTaskDelay(pdMS_TO_TICKS(100));
    }
}