#include "I2C_Driver.h"

static const char *I2C_TAG = "I2C";

static i2c_master_bus_handle_t s_bus = NULL;
static i2c_master_dev_handle_t s_dev = NULL;
static uint8_t s_dev_addr = 0;

static esp_err_t i2c_get_device(uint8_t addr)
{
    if (!s_bus) return ESP_ERR_INVALID_STATE;
    if (s_dev && addr == s_dev_addr) return ESP_OK;

    if (s_dev) {
        i2c_master_bus_rm_device(s_dev);
        s_dev = NULL;
    }

    i2c_device_config_t dev_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = addr,
        .scl_speed_hz = I2C_MASTER_FREQ_HZ,
    };
    esp_err_t err = i2c_master_bus_add_device(s_bus, &dev_cfg, &s_dev);
    if (err == ESP_OK) {
        s_dev_addr = addr;
    }
    return err;
}

/**
 * @brief i2c master initialization (new driver)
 */
static esp_err_t i2c_master_init(void)
{
    if (s_bus) return ESP_OK;

    i2c_master_bus_config_t conf = {
        .i2c_port = I2C_MASTER_NUM,
        .sda_io_num = I2C_Touch_SDA_IO,
        .scl_io_num = I2C_Touch_SCL_IO,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };

    return i2c_new_master_bus(&conf, &s_bus);
}

void I2C_Init(void)
{
    ESP_ERROR_CHECK(i2c_master_init());
    ESP_LOGI(I2C_TAG, "I2C initialized successfully");
}

// Reg addr is 8 bit
esp_err_t I2C_Write(uint8_t Driver_addr, uint8_t Reg_addr, const uint8_t *Reg_data, uint32_t Length)
{
    uint8_t buf[Length + 1];
    buf[0] = Reg_addr;
    memcpy(&buf[1], Reg_data, Length);

    esp_err_t err = i2c_get_device(Driver_addr);
    if (err != ESP_OK) return err;
    return i2c_master_transmit(s_dev, buf, Length + 1, I2C_MASTER_TIMEOUT_MS);
}

esp_err_t I2C_Read(uint8_t Driver_addr, uint8_t Reg_addr, uint8_t *Reg_data, uint32_t Length)
{
    esp_err_t err = i2c_get_device(Driver_addr);
    if (err != ESP_OK) return err;
    return i2c_master_transmit_receive(s_dev, &Reg_addr, 1, Reg_data, Length, I2C_MASTER_TIMEOUT_MS);
}
