/*
 * RotaryController - Rotary controller for Arkanoid, simulating a mouse.
 *
 * Copyright (c) 2024 Carlos Rodrigues <cefrodrigues@gmail.com>
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 */

/*
 * Rotary encoder used: PEC16-4020F-N0024 (incremental, 24 pulses per 360° rotation)
 *                      https://eu.mouser.com/datasheet/2/54/pec16-245034.pdf
 *
 * Adjustment pot used: 10KΩ trimmer potentiometer (multi-turn)
 *                      https://eu.mouser.com/datasheet/2/54/3296-776415.pdf
 */


#include <Mouse.h>
#include <Encoder.h>


// User inputs (interrupt driven)...
#define PIN_ENCODER_1A 2  // pulse pin "A" from the rotary encoder
#define PIN_ENCODER_1B 3  // pulse pin "B" from the rotary encoder
#define PIN_BUTTON_1 7    // long-press on startup enables debug mode

// Adjustment inputs...
#define PIN_DIP_1 4   // enable programming mode (disables mouse)
#define PIN_DIP_2 5   // select mouse axis (X/Y)
#define PIN_POT_1 A0  // adjust rotation multiplier

// Status outputs...
#define PIN_LED_1 9         // mouse mode (on), needs PWM.
#define PIN_LED_BUILTIN 17  // RXLED on the Pro Micro.

#define BUILTIN_LED_INVERTED true  // LOW means ON on the Pro Micro.

// Enable slow mode if the user holds down the button during startup...
#define SLOW_MS 2000
#define SLOW_PCT 50

// The active axis is selectable using DIP switch 2 (active-low)...
#define AXIS_X 1
#define AXIS_Y 0
#define MAX_SPEED 100

#define SERIAL_BPS 9600
#define BOOT_DELAY_MS 2000


// The pins are reversed because that's what's in the datasheet...
Encoder spinner(PIN_ENCODER_1B, PIN_ENCODER_1A);
uint8_t speed_percent = 100;


// Needed because Arduino's "constrain()" chokes with negative bounds...
int8_t constrain_byte(int32_t value) {
  if (value < -127) {
    return -127;
  }

  if (value > 127) {
    return 127;
  }

  return value;
}


void setup() {
  pinMode(PIN_BUTTON_1, INPUT_PULLUP);

  pinMode(PIN_DIP_1, INPUT_PULLUP);
  pinMode(PIN_DIP_2, INPUT_PULLUP);
  
  pinMode(PIN_LED_1, OUTPUT);
  pinMode(PIN_LED_BUILTIN, OUTPUT);

  Serial.begin(SERIAL_BPS);

  // Conditionally enable slow mode (useful for some games)...
  if (!digitalRead(PIN_BUTTON_1)) {  // ...is active-low.
    delay(SLOW_MS);
    speed_percent = !digitalRead(PIN_BUTTON_1) ? SLOW_PCT : 100;
  }

  spinner.write(0);
  Mouse.begin();

  /*
   * Give the host some time to detect us before writing anything to the serial port, and also
   * ensure the button is *not* pressed when we enter the main loop to avoid accidental clicks.
   */
  uint32_t start_ms = millis();
  uint32_t loop_ts = start_ms;
  uint16_t blink_ms = speed_percent < 100 ? 150 : 50;  // ...blink slower to indicate slow mode.

  while (loop_ts < start_ms + BOOT_DELAY_MS || !digitalRead(PIN_BUTTON_1)) {
    digitalWrite(PIN_LED_1, HIGH);
    delay(blink_ms);

    digitalWrite(PIN_LED_1, LOW);
    delay(blink_ms);

    loop_ts = millis();
  }

  Serial.println(F("#"));
  Serial.println(F("# Spinner Game Controller - Carlos Rodrigues, 2024 (cefrodrigues@gmail.com)"));
  Serial.println(F("#"));
  Serial.println(F("# DIP SWITCH 1: mouse ON/OFF"));
  Serial.println(F("# DIP SWITCH 2: set axis X/Y"));
  Serial.println(F("#"));
  Serial.print(F("# Small potentiometer adjusts the SPEED from 1 to ")); Serial.print(MAX_SPEED); Serial.println(F("."));
  Serial.print(F("# Hold fire when connecting for SLOW MODE (")); Serial.print(SLOW_PCT); Serial.println(F("% speed)."));
  Serial.println(F("#"));

  Serial.println(speed_percent < 100 ? F("mode=slow") : F("mode=normal"));
}


void loop() {
  static uint8_t prev_enabled = 0xFF;
  static uint8_t prev_axis = 0xFF;
  static uint8_t prev_speed = 0xFF;
  static uint8_t prev_fire_pressed = 0xFF;

  uint8_t curr_enabled = digitalRead(PIN_DIP_1);
  uint8_t curr_axis = digitalRead(PIN_DIP_2);
  uint8_t curr_speed = map(analogRead(PIN_POT_1), 0, 1023, 1, MAX_SPEED);
  uint8_t curr_fire_pressed = !digitalRead(PIN_BUTTON_1);

  if (curr_enabled != prev_enabled) {
    Serial.print("mouse=");
    Serial.println(curr_enabled ? "on" : "off");

    if (curr_enabled) {
      spinner.write(0);  // ...reset to a known value.
    }

    // We don't need the indicator to be super-bright...
    analogWrite(PIN_LED_1, curr_enabled * 64);

    prev_enabled = curr_enabled;
  }

  if (curr_axis != prev_axis) {
    Serial.print("axis=");
    Serial.println(curr_axis == AXIS_X ? "x" : "y");

    prev_axis = curr_axis;
  }

  if (curr_speed != prev_speed) {
    Serial.print("speed=");
    Serial.println(curr_speed);

    prev_speed = curr_speed;
  }

  if (curr_fire_pressed != prev_fire_pressed) {
    // Just the LED. Only outputs to serial when the mouse is on... 
    digitalWrite(PIN_LED_BUILTIN, BUILTIN_LED_INVERTED ? !curr_fire_pressed : curr_fire_pressed);
    
    prev_fire_pressed = curr_fire_pressed;
  }

  if (!curr_enabled) {
    return;
  }

  int32_t spinner_value = spinner.read();

  if (curr_fire_pressed && !Mouse.isPressed()) {
    Mouse.press();
    Serial.println("fire=on");
  } else if (!curr_fire_pressed && Mouse.isPressed()) {
    Mouse.release();
    Serial.println("fire=off");
  }

  if (spinner_value) {
    int8_t delta = constrain_byte(spinner_value * curr_speed * (speed_percent / 100.0));

    Serial.print("delta(");
    
    if (curr_axis == AXIS_X) {
      Mouse.move(delta, 0, 0);
      Serial.print("x");
    } else {
      Mouse.move(0, -delta, 0);  // ...axis is inverted.
      Serial.print("y");
    }

    Serial.print(")=");
    Serial.println(delta);

    spinner.write(0);
  }
}


/* EOF - RotaryController.ino */
