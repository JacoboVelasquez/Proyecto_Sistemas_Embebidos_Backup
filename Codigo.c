#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include "driver/adc.h"
#include "driver/uart.h"
#include "driver/i2c.h"
#include "esp_timer.h"

// Sistema de automatizacion para galpon avicola.
// Monitorea temperatura y nivel de agua, activando
// lampara, bomba de agua y buzzer segun corresponda.
// Muestra el estado en pantalla LCD 16x2 via I2C.

// Pines de salida
#define LAMPARA_GPIO        14
#define BOMBA_GPIO          23
#define BUZZER_GPIO         22

// Configuracion I2C y LCD
#define I2C_PORT            I2C_NUM_0
#define I2C_SDA             21
#define I2C_SCL             18
#define I2C_FREQ_HZ         100000      // 100 kHz, modo estandar I2C
#define LCD_ADDR            0x27        // direccion PCF8574T, si es AT usar 0x3F
#define LCD_COLS            16
#define LCD_ROWS            2

// Canales ADC
#define ADC_LM35            ADC1_CHANNEL_6   // GPIO34
#define ADC_SENSOR_BEB      ADC1_CHANNEL_7   // GPIO35
#define ADC_SENSOR_TAN      ADC1_CHANNEL_4   // GPIO32

// VREF ajustado empiricamente comparando con termometro de referencia
// hasta que la lectura del LM35 coincidio con la temperatura real
#define VREF                1.537f

// Umbrales del bebedero calibrados con el sensor SEN-3003 en pruebas
#define BEB_NIVEL_BAJO      520     // activa la bomba si baja de aqui
#define BEB_NIVEL_LLENO     1000    // apaga la bomba cuando llega aqui

// Umbral del tanque
#define TAN_NIVEL_ALERTA    1350    // activa el buzzer si baja de aqui

// Rango de temperatura normal del galpon
#define TEMP_MIN_DEFAULT    22.0f   // enciende lampara si baja de aqui
#define TEMP_MAX_DEFAULT    25.0f   // apaga lampara cuando sube hasta aqui

// Rangos validos de ADC para detectar si un sensor esta desconectado.
// En condiciones normales ningun sensor deberia leer 0 ni el maximo de 4095.
#define LM35_RAW_MIN        50
#define LM35_RAW_MAX        4000
#define SENSOR_RAW_MIN      10
#define SENSOR_RAW_MAX      4090

// Configuracion UART
#define UART_PORT           UART_NUM_0
#define UART_BAUD           115200
#define UART_BUF            256

// Codigos de error del sistema
#define ERR_NONE                0
#define ERR_SENSOR_LM35         1
#define ERR_SENSOR_BEBEDERO     2
#define ERR_SENSOR_TANQUE       3

// Niveles de severidad para el modulo de logging
typedef enum { LOG_INFO, LOG_WARN, LOG_ERROR } log_level_t;

// Umbrales de temperatura, modificables en tiempo real desde UART
static volatile float g_temp_min = TEMP_MIN_DEFAULT;
static volatile float g_temp_max = TEMP_MAX_DEFAULT;


// Driver LCD 16x2 con modulo PCF8574 via I2C en modo 4 bits

#define LCD_BACKLIGHT   0x08
#define LCD_EN          0x04
#define LCD_RW          0x02
#define LCD_RS          0x01

static void lcd_i2c_write(uint8_t data) {
    i2c_cmd_handle_t cmd = i2c_cmd_link_create();
    i2c_master_start(cmd);
    i2c_master_write_byte(cmd, (LCD_ADDR << 1) | I2C_MASTER_WRITE, true);
    i2c_master_write_byte(cmd, data | LCD_BACKLIGHT, true);
    i2c_master_stop(cmd);
    i2c_master_cmd_begin(I2C_PORT, cmd, pdMS_TO_TICKS(10));
    i2c_cmd_link_delete(cmd);
}

static void lcd_pulse_enable(uint8_t data) {
    // el pulso en EN le indica al LCD que lea el dato
    lcd_i2c_write(data | LCD_EN);
    vTaskDelay(pdMS_TO_TICKS(1));
    lcd_i2c_write(data & ~LCD_EN);
    vTaskDelay(pdMS_TO_TICKS(1));
}

static void lcd_send_nibble(uint8_t nibble, uint8_t mode) {
    uint8_t data = (nibble & 0xF0) | mode;
    lcd_i2c_write(data);
    lcd_pulse_enable(data);
}

// En modo 4 bits se manda primero el nibble alto y luego el bajo
static void lcd_send_byte(uint8_t byte, uint8_t mode) {
    lcd_send_nibble(byte & 0xF0, mode);
    lcd_send_nibble((byte << 4) & 0xF0, mode);
}

static void lcd_cmd(uint8_t cmd) {
    lcd_send_byte(cmd, 0x00);
}

static void lcd_char(char c) {
    lcd_send_byte((uint8_t)c, LCD_RS);
}

static void lcd_set_cursor(uint8_t col, uint8_t row) {
    uint8_t offsets[] = {0x00, 0x40};
    lcd_cmd(0x80 | (col + offsets[row]));
}

static void lcd_clear(void) {
    lcd_cmd(0x01);
    vTaskDelay(pdMS_TO_TICKS(2));
}

static void lcd_print(const char *str) {
    while (*str) {
        lcd_char(*str++);
    }
}

static void lcd_init(void) {
    // secuencia de inicializacion segun datasheet del HD44780
    vTaskDelay(pdMS_TO_TICKS(50));
    lcd_send_nibble(0x30, 0x00);
    vTaskDelay(pdMS_TO_TICKS(5));
    lcd_send_nibble(0x30, 0x00);
    vTaskDelay(pdMS_TO_TICKS(1));
    lcd_send_nibble(0x30, 0x00);
    vTaskDelay(pdMS_TO_TICKS(1));
    lcd_send_nibble(0x20, 0x00);  // cambia a modo 4 bits
    vTaskDelay(pdMS_TO_TICKS(1));
    lcd_cmd(0x28);  // 4 bits, 2 filas, fuente 5x8
    lcd_cmd(0x0C);  // display encendido, cursor apagado
    lcd_cmd(0x06);  // cursor se mueve a la derecha automaticamente
    lcd_clear();
}


// Modulo de logging por UART

static uint32_t get_timestamp_s(void) {
    // retorna los segundos transcurridos desde que arranco el sistema
    return (uint32_t)(esp_timer_get_time() / 1000000);
}

static void log_msg(log_level_t level, int error_code, const char *msg) {
    const char *lvl_str;
    switch (level) {
        case LOG_INFO:  lvl_str = "INFO";  break;
        case LOG_WARN:  lvl_str = "WARN";  break;
        case LOG_ERROR: lvl_str = "ERROR"; break;
        default:        lvl_str = "INFO";  break;
    }
    if (error_code != ERR_NONE) {
        printf("[%lu] [%s] [ERR_%02d] %s\n",
            get_timestamp_s(), lvl_str, error_code, msg);
    } else {
        printf("[%lu] [%s] %s\n",
            get_timestamp_s(), lvl_str, msg);
    }
}


// Lectura ADC con promedio de 64 muestras para reducir el ruido
// del ADC interno del ESP32, que es bastante ruidoso con señales pequeñas
static int adc_avg(adc1_channel_t ch) {
    int sum = 0;
    for (int i = 0; i < 64; i++) {
        sum += adc1_get_raw(ch);
    }
    return sum / 64;
}


// Inicializaciones de perifericos

static void gpio_init_all(void) {
    gpio_config_t out_conf = {
        .pin_bit_mask = (1ULL << LAMPARA_GPIO) |
                        (1ULL << BOMBA_GPIO)   |
                        (1ULL << BUZZER_GPIO),
        .mode         = GPIO_MODE_OUTPUT,
        .pull_up_en   = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_DISABLE,
    };
    gpio_config(&out_conf);
    // todos los actuadores apagados al arrancar
    gpio_set_level(LAMPARA_GPIO, 0);
    gpio_set_level(BOMBA_GPIO,   0);
    gpio_set_level(BUZZER_GPIO,  0);
}

static void adc_init_all(void) {
    adc1_config_width(ADC_WIDTH_BIT_12);
    // LM35 usa DB_0 porque su señal maxima es ~0.95V con 3.3V de alimentacion
    // los sensores de agua usan DB_12 porque su señal llega hasta 3.3V
    adc1_config_channel_atten(ADC_LM35,       ADC_ATTEN_DB_0);
    adc1_config_channel_atten(ADC_SENSOR_BEB, ADC_ATTEN_DB_12);
    adc1_config_channel_atten(ADC_SENSOR_TAN, ADC_ATTEN_DB_12);
}

static void uart_init_all(void) {
    uart_config_t cfg = {
        .baud_rate  = UART_BAUD,
        .data_bits  = UART_DATA_8_BITS,
        .parity     = UART_PARITY_DISABLE,
        .stop_bits  = UART_STOP_BITS_1,
        .flow_ctrl  = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };
    uart_driver_install(UART_PORT, UART_BUF * 2, 0, 0, NULL, 0);
    uart_param_config(UART_PORT, &cfg);
}

static void i2c_init_all(void) {
    i2c_config_t conf = {
        .mode             = I2C_MODE_MASTER,
        .sda_io_num       = I2C_SDA,
        .scl_io_num       = I2C_SCL,
        .sda_pullup_en    = GPIO_PULLUP_ENABLE,
        .scl_pullup_en    = GPIO_PULLUP_ENABLE,
        .master.clk_speed = I2C_FREQ_HZ,
    };
    i2c_param_config(I2C_PORT, &conf);
    i2c_driver_install(I2C_PORT, I2C_MODE_MASTER, 0, 0, 0);
}


// Procesamiento de comandos por UART.
// Permite cambiar parametros en caliente sin recompilar el firmware.
// Comandos disponibles:
//   SET_TEMP_MIN:XX.X  cambia la temperatura minima
//   SET_TEMP_MAX:XX.X  cambia la temperatura maxima
//   STATUS             imprime el estado actual de todos los actuadores

static char uart_buf[UART_BUF];
static int  uart_pos = 0;

static void uart_process(void) {
    uint8_t byte;
    int rx = uart_read_bytes(UART_PORT, &byte, 1, 0);
    if (rx <= 0) return;

    if (byte == '\n' || byte == '\r') {
        if (uart_pos == 0) return;
        uart_buf[uart_pos] = '\0';
        uart_pos = 0;

        if (strncmp(uart_buf, "SET_TEMP_MIN:", 13) == 0) {
            float val = atof(uart_buf + 13);
            if (val > 0.0f && val < 50.0f && val < g_temp_max) {
                g_temp_min = val;
                char msg[64];
                snprintf(msg, sizeof(msg), "TEMP_MIN actualizada a %.1f C", g_temp_min);
                log_msg(LOG_INFO, ERR_NONE, msg);
            } else {
                log_msg(LOG_WARN, ERR_NONE, "SET_TEMP_MIN: valor invalido");
            }

        } else if (strncmp(uart_buf, "SET_TEMP_MAX:", 13) == 0) {
            float val = atof(uart_buf + 13);
            if (val > 0.0f && val < 50.0f && val > g_temp_min) {
                g_temp_max = val;
                char msg[64];
                snprintf(msg, sizeof(msg), "TEMP_MAX actualizada a %.1f C", g_temp_max);
                log_msg(LOG_INFO, ERR_NONE, msg);
            } else {
                log_msg(LOG_WARN, ERR_NONE, "SET_TEMP_MAX: valor invalido");
            }

        } else if (strcmp(uart_buf, "STATUS") == 0) {
            char msg[128];
            snprintf(msg, sizeof(msg),
                "TEMP_MIN=%.1f | TEMP_MAX=%.1f | LAMPARA=%d | BOMBA=%d | BUZZER=%d",
                g_temp_min, g_temp_max,
                gpio_get_level(LAMPARA_GPIO),
                gpio_get_level(BOMBA_GPIO),
                gpio_get_level(BUZZER_GPIO));
            log_msg(LOG_INFO, ERR_NONE, msg);

        } else {
            log_msg(LOG_WARN, ERR_NONE,
                "Comando desconocido. Usa SET_TEMP_MIN:XX / SET_TEMP_MAX:XX / STATUS");
        }

    } else if (uart_pos < UART_BUF - 1) {
        uart_buf[uart_pos++] = (char)byte;
    }
}


// Actualiza la pantalla LCD con el estado actual del sistema
static void lcd_update(float temp, bool lampara, bool bomba, int raw_tan) {
    char line1[17], line2[17];

    // fila superior: temperatura y estado de la lampara
    snprintf(line1, sizeof(line1), "T:%.1fC Lmp:%s", temp, lampara ? "ON " : "OFF");

    // fila inferior: bomba y nivel del tanque
    snprintf(line2, sizeof(line2), "Bom:%s Tan:%s",
        bomba ? "ON " : "OFF",
        raw_tan < TAN_NIVEL_ALERTA ? "BAJ" : "OK ");

    lcd_set_cursor(0, 0);
    lcd_print(line1);
    lcd_set_cursor(0, 1);
    lcd_print(line2);
}


void app_main(void) {

    gpio_init_all();
    adc_init_all();
    uart_init_all();
    i2c_init_all();
    lcd_init();

    bool bomba_on   = false;
    bool lampara_on = false;

    log_msg(LOG_INFO, ERR_NONE, "Sistema galpon iniciado");
    log_msg(LOG_INFO, ERR_NONE, "Comandos: SET_TEMP_MIN:XX / SET_TEMP_MAX:XX / STATUS");

    lcd_clear();
    lcd_set_cursor(0, 0);
    lcd_print("Galpon Avicola");
    lcd_set_cursor(0, 1);
    lcd_print("Iniciando...");
    vTaskDelay(pdMS_TO_TICKS(2000));

    int   telem_cnt = 0;
    float temp_last = 0.0f;
    int   raw_tan   = 0;
    int   raw_beb   = 0;

    while (1) {

        uart_process();

        // lectura y control de temperatura
        int raw_lm35 = adc_avg(ADC_LM35);

        if (raw_lm35 < LM35_RAW_MIN || raw_lm35 > LM35_RAW_MAX) {
            // valor imposible, probablemente el sensor esta desconectado
            log_msg(LOG_ERROR, ERR_SENSOR_LM35,
                "ERR_SENSOR_LM35: valor fuera de rango, lampara desactivada");
            gpio_set_level(LAMPARA_GPIO, 0);
            lampara_on = false;
        } else {
            temp_last = (raw_lm35 / 4095.0f) * VREF * 100.0f;

            if (!lampara_on && temp_last < g_temp_min) {
                gpio_set_level(LAMPARA_GPIO, 1);
                lampara_on = true;
                log_msg(LOG_WARN, ERR_NONE, "Temperatura baja — lampara ON");
            } else if (lampara_on && temp_last >= g_temp_max) {
                gpio_set_level(LAMPARA_GPIO, 0);
                lampara_on = false;
                log_msg(LOG_INFO, ERR_NONE, "Temperatura normal — lampara OFF");
            }
        }

        // lectura y control del bebedero
        raw_beb = adc_avg(ADC_SENSOR_BEB);

        if (raw_beb < SENSOR_RAW_MIN || raw_beb > SENSOR_RAW_MAX) {
            log_msg(LOG_ERROR, ERR_SENSOR_BEBEDERO,
                "ERR_SENSOR_BEBEDERO: valor fuera de rango");
        } else {
            if (!bomba_on && raw_beb < BEB_NIVEL_BAJO) {
                gpio_set_level(BOMBA_GPIO, 1);
                bomba_on = true;
                log_msg(LOG_WARN, ERR_NONE, "Bebedero bajo — bomba ON");
            } else if (bomba_on && raw_beb > BEB_NIVEL_LLENO) {
                gpio_set_level(BOMBA_GPIO, 0);
                bomba_on = false;
                log_msg(LOG_INFO, ERR_NONE, "Bebedero lleno — bomba OFF");
            }
        }

        // lectura y control del tanque
        raw_tan = adc_avg(ADC_SENSOR_TAN);

        if (raw_tan < SENSOR_RAW_MIN || raw_tan > SENSOR_RAW_MAX) {
            log_msg(LOG_ERROR, ERR_SENSOR_TANQUE,
                "ERR_SENSOR_TANQUE: valor fuera de rango");
        } else {
            if (raw_tan < TAN_NIVEL_ALERTA) {
                if (!gpio_get_level(BUZZER_GPIO)) {
                    log_msg(LOG_WARN, ERR_NONE, "Tanque vacio — buzzer ON");
                }
                gpio_set_level(BUZZER_GPIO, 1);
            } else {
                gpio_set_level(BUZZER_GPIO, 0);
            }
        }

        // actualiza la pantalla cada segundo
        lcd_update(temp_last, lampara_on, bomba_on, raw_tan);

        // imprime telemetria completa cada 10 segundos
        telem_cnt++;
        if (telem_cnt >= 10) {
            telem_cnt = 0;
            char msg[128];
            snprintf(msg, sizeof(msg),
                "Temp=%.1f C | Lampara=%s | Beb=%d | Bomba=%s | Tan=%d | Buzzer=%s",
                temp_last,
                lampara_on                  ? "ON"  : "OFF",
                raw_beb,
                bomba_on                    ? "ON"  : "OFF",
                raw_tan,
                gpio_get_level(BUZZER_GPIO) ? "ON"  : "OFF");
            log_msg(LOG_INFO, ERR_NONE, msg);
        }

        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}
