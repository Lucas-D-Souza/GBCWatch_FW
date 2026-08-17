#pragma once

#include "esp_err.h"
#include "sdmmc_cmd.h"

class SdUsbManager {
public:
    // Padrão Singleton - Apenas uma instância controlando o hardware
    static SdUsbManager& get_instance() {
        static SdUsbManager instance;
        return instance;
    }

    // Inicializa o barramento SDMMC e monta para o ESP32 (Uso interno)
    esp_err_t init_local_storage();

    // Desmonta o SD Card de forma segura, salvando os dados
    esp_err_t deinit_local_storage();

    // Alterna para o modo USB (Desmonta do ESP32, expõe para o PC)
    esp_err_t enable_usb_msc();

    // Alterna para o modo Local (Desconecta do PC, remonta no ESP32)
    esp_err_t disable_usb_msc();

    // Verifica se o modo pendrive está ativo
    bool is_msc_active() const { return msc_active; }

private:
    SdUsbManager() = default;
    ~SdUsbManager() = default;

    // Previne cópias (Clean Code)
    SdUsbManager(const SdUsbManager&) = delete;
    SdUsbManager& operator=(const SdUsbManager&) = delete;

    sdmmc_card_t* card = nullptr;
    bool msc_active = false;
    bool sd_initialized = false;
    
    const char* mount_point = "/sdcard";
};