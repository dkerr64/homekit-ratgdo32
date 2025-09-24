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

// RATGDO project includes
#include "ratgdo.h"
#include "led.h"

// Logger tag
// static const char *TAG = "ratgdo-led";

// Construct the singleton object for LED access
LED led(LED_BUILTIN, LED_BUILTIN_ON_STATE);
#ifdef RATGDO32_DISCO
LED laser(LASER_PIN);
#endif

// Constructor for LED class
LED::LED(uint8_t gpio_num, uint8_t state)
{
    pin = gpio_num;
    activeState = onState = state;
    // off is opposite of on, which can be zero or one.
    currentState = offState = (onState == 1) ? 0 : 1;
    idleState = (activeState == 1) ? 0 : 1;
    LEDtimer = Ticker();
    pinMode(pin, OUTPUT);
}

void LED::on()
{
    digitalWrite(pin, onState);
    currentState = onState;
}

void LED::off()
{
    digitalWrite(pin, offState);
    currentState = offState;
}

void LED::idle()
{
    digitalWrite(pin, idleState);
    currentState = idleState;
}

void LED::setIdleState(uint8_t state)
{
    heartbeatStop();

    // 0 = LED flashes on (off when idle)
    // 1 = LED flashes off (on when idle)
    // 2 = LED disabled (active and idle both off)
    // 3 = LED heartbeat (1s on, 5s off)
    if (state == 2)
    {
        // set all to 0 (off)
        idleState = activeState = offState;
    }
    if (state == 3)
    {
        heartbeatStart();
    }
    else
    {
        idleState = state ? onState : offState;
        // active state is opposite of idle state which can be zero or one.
        activeState = (idleState == 1) ? 0 : 1;
    }
}

void LED::flash(uint64_t ms)
{
    // Don't flash if we are already in a flash.
    if (!LEDtimer.active())
    {
        digitalWrite(pin, activeState);
        currentState = activeState;
        LEDtimer.once_ms(ms, [this]()
                         { this->idle(); });
    }
}

void LED::heartbeatCallback()
{
    if (currentState == onState)
    {
        off();
        LEDBlinkTicker.once_ms(offTime, [this]()
                     { this->heartbeatCallback(); });
    }
    else
    {
        on();
        LEDBlinkTicker.once_ms(onTime, [this]()
                     { this->heartbeatCallback(); });
    }
}

void LED::heartbeatStart(uint64_t onTimeMS, uint64_t offTimeMS)
{
    onTime = onTimeMS;
    offTime = offTimeMS;
    LEDBlinkTicker.once_ms(onTimeMS, [this]()
                     { this->heartbeatCallback(); });
}

void LED::heartbeatStop()
{
    LEDBlinkTicker.detach();
    off();
}
