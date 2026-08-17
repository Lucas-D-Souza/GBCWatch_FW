#include "SdUsbManager.hpp"
#include "esp_log.h"
#include "esp_vfs_fat.h"
#include "tinyusb.h"
#include "tusb_msc_storage.h"
#include "driver/gpio.h" // [NOVO] Necessário para manipular os pinos USB
#include <unistd.h>

// Pinos do SD Card confirmados via diagrama da Waveshare
#define PIN_NUM_MOSI 1
#define PIN_NUM_CLK  2
#define PIN_NUM_MISO 3
#define PIN_NUM_CS   17 

static const char *TAG = "SdUsbMgr";

esp_err_t SdUsbManager::init_local_storage() {
    if (sd_initialized && !msc_active) return ESP_OK;

    ESP_LOGI(TAG, "Montando Cartao SD no ESP32...");

    sdmmc_host_t host = SDSPI_HOST_DEFAULT();
    
    // Força o uso do SPI3_HOST para não conflitar com o Display AMOLED (SPI2_HOST)
    host.slot = SPI3_HOST; 
    
    spi_bus_config_t bus_cfg = {};
    bus_cfg.mosi_io_num = PIN_NUM_MOSI;
    bus_cfg.miso_io_num = PIN_NUM_MISO;
    bus_cfg.sclk_io_num = PIN_NUM_CLK;
    bus_cfg.quadwp_io_num = -1;
    bus_cfg.quadhd_io_num = -1;
    bus_cfg.max_transfer_sz = 4000;

    // Inicializa no SPI3. Ignora se já estiver ligado.
    esp_err_t err = spi_bus_initialize((spi_host_device_t)host.slot, &bus_cfg, SDSPI_DEFAULT_DMA);
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "Falha ao inicializar barramento SPI: %s", esp_err_to_name(err));
        return err;
    }

    sdspi_device_config_t slot_config = SDSPI_DEVICE_CONFIG_DEFAULT();
    slot_config.gpio_cs = (gpio_num_t)PIN_NUM_CS;
    slot_config.host_id = (spi_host_device_t)host.slot;

    esp_vfs_fat_sdmmc_mount_config_t mount_config = {};
    mount_config.format_if_mount_failed = false;
    mount_config.max_files = 5;
    mount_config.allocation_unit_size = 16 * 1024;

    esp_err_t ret = esp_vfs_fat_sdspi_mount(mount_point, &host, &slot_config, &mount_config, &card);

    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Falha ao montar SD Card (0x%x). Verifique os pinos/cartao.", ret);
        return ret;
    }

    sd_initialized = true;
    msc_active = false;
    ESP_LOGI(TAG, "SD Card montado com sucesso em %s", mount_point);
    return ESP_OK;
}

esp_err_t SdUsbManager::enable_usb_msc() {
    if (msc_active) return ESP_OK;

    // Trava anti-crash. Se não houver SD Card, o botão não faz nada e não reinicia a placa.
    if (card == nullptr) {
        ESP_LOGE(TAG, "Cartao SD nao foi inicializado no boot! Abortando MSC para evitar Crash.");
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "Iniciando TinyUSB MSC...");
    
    tinyusb_config_t tusb_cfg = {};
    
    esp_err_t ret = tinyusb_driver_install(&tusb_cfg);
    if (ret != ESP_OK && ret != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "Falha ao instalar TinyUSB");
        return ret;
    }

    tinyusb_msc_sdmmc_config_t msc_config = {};
    msc_config.card = card;

    ret = tinyusb_msc_storage_init_sdmmc(&msc_config);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Falha ao vincular SD ao TinyUSB");
        return ret;
    }

    msc_active = true;
    ESP_LOGI(TAG, "Modo Pendrive Ativo! Conecte no PC.");
    return ESP_OK;
}

esp_err_t SdUsbManager::disable_usb_msc() {
    if (!msc_active) return ESP_OK;

    ESP_LOGI(TAG, "Saindo do Modo Pendrive. O sistema sera reiniciado para restaurar o Monitor Serial...");

    // 1. Derruba fisicamente os pinos USB internos do ESP32-S3 (GPIO 19 e 20)
    // Isso força o Windows a registrar a remoção do Pendrive IMEDIATAMENTE.
    gpio_reset_pin(GPIO_NUM_19);
    gpio_reset_pin(GPIO_NUM_20);
    gpio_set_direction(GPIO_NUM_19, GPIO_MODE_INPUT);
    gpio_set_direction(GPIO_NUM_20, GPIO_MODE_INPUT);

    // 2. Aguarda 2.5 segundos para o PC tocar o som de "desconectado" e limpar a porta COM
    vTaskDelay(pdMS_TO_TICKS(2500));

    // 3. Reinicia o ESP32 graciosamente. O Bootloader vai restaurar o USB JTAG/Serial automaticamente.
    esp_restart();

    return ESP_OK; // O código nunca chegará aqui devido ao reboot
}

esp_err_t SdUsbManager::deinit_local_storage() {
    if (!sd_initialized) return ESP_OK;

    ESP_LOGI(TAG, "Desmontando Cartao SD e ejetando...");
    
    // Código limpo e nativo do ESP-IDF
    esp_err_t err = esp_vfs_fat_sdcard_unmount(mount_point, card);
    
    if (err == ESP_OK) {
        sd_initialized = false;
        card = nullptr;
        ESP_LOGI(TAG, "Cartao SD ejetado com seguranca.");
    } else {
        ESP_LOGE(TAG, "Falha ao ejetar Cartao SD: %s", esp_err_to_name(err));
    }
    
    return err;
}