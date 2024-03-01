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
#define PIN_BUTTON_1 7    // long-press on startup enables slow mode
#define PIN_BUTTON_2 10

#define PIN_JACK_SENSE 8  // whether an external button (pedal) is present
#define PIN_BUTTON_EXT 6  // when present, this becomes the primary button

// Adjustment inputs...
#define PIN_DIP_1 4   // enable programming mode (disables mouse)
#define PIN_DIP_2 5   // select mouse axis (X/Y)
#define PIN_POT_1 A0  // adjust rotation multiplier (speed)

// Status outputs...
#define PIN_LED_1 9         // events enabled (mouse on), needs PWM
#define PIN_LED_BUILTIN 17  // corresponds to RXLED on the Pro Micro

#define BUILTIN_LED_INVERTED true  // LOW means the LED is ON on the Pro Micro

#define SLOW_MS 2000 // enable slow mode if the main button is held for (at least) this long on startup
#define SLOW_PCT 0   // set to zero for maximum accuracy (https://wiki.arcadecontrols.com/?title=Spinner_Turn_Count)

// The active axis is selectable using DIP switch 2 (active-low)...
#define AXIS_X 1
#define AXIS_Y 0
#define MAX_SPEED 50

#define SERIAL_BPS 115600
#define LED_BLINK_MS 150  // user feedback for alternate (slow) mode on boot
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
  pinMode(PIN_BUTTON_2, INPUT_PULLUP);

  pinMode(PIN_JACK_SENSE, INPUT_PULLUP);
  pinMode(PIN_BUTTON_EXT, INPUT_PULLUP);

  pinMode(PIN_DIP_1, INPUT_PULLUP);
  pinMode(PIN_DIP_2, INPUT_PULLUP);
  
  pinMode(PIN_LED_1, OUTPUT);
  pinMode(PIN_LED_BUILTIN, OUTPUT);

  // Conditionally enable slow mode...
  if (!digitalRead(PIN_BUTTON_1)) {  // ...is active-low.
    delay(SLOW_MS);
    speed_percent = !digitalRead(PIN_BUTTON_1) ? SLOW_PCT : 100;
  }

  /*
   * Give the host some time to detect us before writing anything to the serial port, and also
   * ensure the button is no longer pressed when we enter the main loop to avoid accidental clicks.
   */
  uint32_t start_ms = millis();
  uint32_t loop_ts = start_ms;

  while (loop_ts < start_ms + BOOT_DELAY_MS || !digitalRead(PIN_BUTTON_1)) {
    if (speed_percent < 100) {  // ...indicate alternate (slow) mode.
      digitalWrite(PIN_LED_1, HIGH);
      delay(LED_BLINK_MS);
      digitalWrite(PIN_LED_1, LOW);
      delay(LED_BLINK_MS);
    }

    loop_ts = millis();
  }

  Mouse.begin();
  spinner.write(0);

  Serial.begin(SERIAL_BPS);
  Serial.println("ready");
}


void loop() {
  static uint8_t prev_jack_present = 0xFF;
  static uint8_t prev_events_enabled = 0xFF;
  static uint8_t prev_axis = 0xFF;
  static uint8_t prev_speed = 0xFF;

  static uint8_t prev_left_pressed = 0xFF;
  static uint8_t prev_right_pressed = 0xFF;
  static uint8_t prev_middle_pressed = 0xFF;

  uint8_t jack_present = digitalRead(PIN_JACK_SENSE);  // ...is active-low, but also normally-connected.
  uint8_t events_enabled = digitalRead(PIN_DIP_1);
  uint8_t axis = digitalRead(PIN_DIP_2);

  uint8_t speed = map(analogRead(PIN_POT_1), 0, 1023, 1, MAX_SPEED);

  // Physical button mapping depends on the presence of an external button (pedal)...
  uint8_t left_pressed = 0;
  uint8_t right_pressed = 0;
  uint8_t middle_pressed = 0;

  if (Serial.available() > 0) {
    switch (Serial.read() | 0x20) {  // ...as lowercase.
      case 's':
        speed_percent = SLOW_PCT;
        Serial.println("mode=slow");
        break;

      case 'n':
        speed_percent = 100;
        Serial.println("mode=normal");
        break;

      case 'i':
        Serial.println(speed_percent < 100 ? "mode=slow" : "mode=normal");
        break;

      default:
        Serial.println("?");  // ...unknown command.
    }
  }

  if (jack_present != prev_jack_present) {
    Mouse.release(MOUSE_ALL);  // ...buttons will be reassigned.

    Serial.print("jack_");
    Serial.println(jack_present ? "connected" : "disconnected");

    prev_jack_present = jack_present;
  }

  if (events_enabled != prev_events_enabled) {
    if (events_enabled) {
      spinner.write(0);  // ...reset to a known value.
    } else {
      Mouse.release(MOUSE_ALL);
    }

    // We don't need the indicator to be super-bright...
    analogWrite(PIN_LED_1, events_enabled * 64);

    Serial.print("events_");
    Serial.println(events_enabled ? "enabled" : "disabled");

    prev_events_enabled = events_enabled;
  }

  if (axis != prev_axis) {
    Serial.print("axis=");
    Serial.println(axis == AXIS_X ? "x" : "y");

    prev_axis = axis;
  }

  if (speed != prev_speed) {
    Serial.print("speed=");
    Serial.print(speed);
    Serial.print("/");
    Serial.println(MAX_SPEED);

    prev_speed = speed;
  }

  if (jack_present) {
    left_pressed = !digitalRead(PIN_BUTTON_EXT);
    right_pressed = !digitalRead(PIN_BUTTON_1);
    middle_pressed = !digitalRead(PIN_BUTTON_2);
  } else {
    left_pressed = !digitalRead(PIN_BUTTON_1);
    right_pressed = !digitalRead(PIN_BUTTON_2);
    middle_pressed = !digitalRead(PIN_BUTTON_EXT);
  }

  if (left_pressed != prev_left_pressed) {
    digitalWrite(PIN_LED_BUILTIN, BUILTIN_LED_INVERTED ? !left_pressed : left_pressed);
    prev_left_pressed = left_pressed;
  }

  if (right_pressed != prev_right_pressed) {
    digitalWrite(PIN_LED_BUILTIN, BUILTIN_LED_INVERTED ? !right_pressed : right_pressed);    
    prev_right_pressed = right_pressed;
  }

  if (middle_pressed != prev_middle_pressed) {
    digitalWrite(PIN_LED_BUILTIN, BUILTIN_LED_INVERTED ? !middle_pressed : middle_pressed);    
    prev_middle_pressed = middle_pressed;
  }

  if (!events_enabled) {
    return;  // ...don't send any mouse events.
  }

  if (left_pressed && !Mouse.isPressed(MOUSE_LEFT)) {
    Mouse.press(MOUSE_LEFT);
    Serial.println("left_press");
  } else if (!left_pressed && Mouse.isPressed(MOUSE_LEFT)) {
    Mouse.release(MOUSE_LEFT);
    Serial.println("left_release");
  }

  if (right_pressed && !Mouse.isPressed(MOUSE_RIGHT)) {
    Mouse.press(MOUSE_RIGHT);
    Serial.println("right_press");
  } else if (!right_pressed && Mouse.isPressed(MOUSE_RIGHT)) {
    Mouse.release(MOUSE_RIGHT);
    Serial.println("right_release");
  }

  if (middle_pressed && !Mouse.isPressed(MOUSE_MIDDLE)) {
    Mouse.press(MOUSE_MIDDLE);
    Serial.println("middle_press");
  } else if (!middle_pressed && Mouse.isPressed(MOUSE_MIDDLE)) {
    Mouse.release(MOUSE_MIDDLE);
    Serial.println("middle_release");
  }

  int32_t spinner_value = spinner.read();

  if (spinner_value) {
    int8_t delta = constrain_byte(spinner_value * max(1, speed * speed_percent / 100.0));

    if (axis == AXIS_X) {
      Mouse.move(delta, 0, 0);
    } else {
      delta = -delta;  // ...vertical axis is inverted.
      Mouse.move(0, -delta, 0);
    }

    Serial.print(delta >= 0 ? "+" : "-");
    Serial.println(abs(delta));

    spinner.write(0);
  }
}


/* EOF - RotaryController.ino */
