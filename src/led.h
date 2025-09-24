/****************************************************************************
 * RATGDO HomeKit
 * https://ratcloud.llc
 * https://github.com/PaulWieland/ratgdo
 *
 * Copyright (c) 2023-25 David A Kerr... https://github.com/dkerr64/
 * All Rights Reserved.
 * Licensed under terms of the GPL-3.0 License.
 *
 */
#pragma once

// C/C++ language includes
#include <stdint.h>

// Arduino includes
#include <Ticker.h>

#define FLASH_MS 500 // default flash period, 500ms
#define FLASH_ACTIVITY_MS 250
#define BLINK_ON_MS 1000
#define BLINK_OFF_MS 5000

class LED
{
private:
    uint8_t pin;
    uint8_t onState = 1;  // What pin state represents "on", can be zero (0) or one (1)
    uint8_t offState = 0; // opposite of on
    uint8_t activeState = 1;
    uint8_t idleState = 0; // opposite of active
    uint8_t currentState = 0;
    Ticker LEDtimer;
    uint64_t onTime;
    uint64_t offTime;
    Ticker LEDBlinkTicker;
    void heartbeatCallback();

public:
    explicit LED(uint8_t gpio_num, uint8_t state = 1);
    void on();
    void off();
    void idle();
    bool state() { return (currentState == onState); };
    void flash(uint64_t ms = FLASH_MS);
    void setIdleState(uint8_t state);
    uint8_t getIdleState() { return idleState; };
    void heartbeatStart(uint64_t onTimeMS = BLINK_ON_MS, uint64_t offTimeMS = BLINK_OFF_MS);
    void heartbeatStop();
};

extern LED led;
#ifdef RATGDO32_DISCO
extern LED laser;
#endif
