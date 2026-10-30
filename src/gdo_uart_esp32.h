#pragma once

#include <driver/gpio.h>
#include <hal/gpio_ll.h>
#include <soc/gpio_struct.h>
#include <soc/gpio_sig_map.h>

// The TX operations are lock-free register operations: panic handling must
// work with the flash cache disabled and without calling the UART driver.
struct Esp32GdoUartPins
{
    __attribute__((always_inline)) inline void preload_low(int pin) { gpio_ll_set_level(&GPIO, pin, 0); }
    __attribute__((always_inline)) inline void select_gpio(int pin) { gpio_ll_func_sel(&GPIO, pin, PIN_FUNC_GPIO); }
    __attribute__((always_inline)) inline void route_gpio(int pin)
    {
        // Both the signal and its output-enable now belong to GPIO, not UART.
        REG_WRITE(GPIO_FUNC0_OUT_SEL_CFG_REG + pin * 4, SIG_GPIO_OUT_IDX | GPIO_FUNC0_OEN_SEL);
    }
    __attribute__((always_inline)) inline void disable_tx_pullup(int pin) { gpio_ll_pullup_dis(&GPIO, pin); }
    __attribute__((always_inline)) inline void enable_tx_pulldown(int pin)
    {
        // gpio_ll_pulldown_en() uses the flash-resident GPIO_PIN_MUX_REG table
        // and may itself be outlined into flash. Use the IDF's DRAM offset table.
        REG_SET_BIT(DR_REG_IO_MUX_BASE + GPIO_PIN_MUX_REG_OFFSET[pin], FUN_PD);
    }
    __attribute__((always_inline)) inline void enable_output(int pin) { gpio_ll_output_enable(&GPIO, pin); }
    bool disable_rx_pulldown(int pin) { return gpio_pulldown_dis((gpio_num_t)pin) == ESP_OK; }
    bool enable_rx_pullup(int pin) { return gpio_pullup_en((gpio_num_t)pin) == ESP_OK; }
};
