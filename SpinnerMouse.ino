/*
 * SpinnerMouse - Spinner game controller (as a mouse) for arcade emulators.
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
 * Rotary encoder used: PEC16-4020F-N0024 (incremental, 96 steps per turn)
 *                      https://eu.mouser.com/datasheet/2/54/pec16-245034.pdf
 *
 * Adjustment pot used: 10KΩ trimmer potentiometer (multi-turn)
 *                      https://eu.mouser.com/datasheet/2/54/3296-776415.pdf
 */


#include <Mouse.h>
#include <Encoder.h>


// Incremental (quadrature) rotary encoder (interrupt driven)...
#define PIN_ENCODER_1A 2  // pulse pin "A" on the encoder
#define PIN_ENCODER_1B 3  // pulse pin "B" on the encoder

// All buttons are active-low (have pull-up resistors)...
#define PIN_BUTTON_1 7    // primary button (long-press on startup for slow mode)
#define PIN_BUTTON_2 10

#define PIN_JACK_SENSE 8  // whether an external button (pedal) is present
#define PIN_BUTTON_EXT 6  // when present, this becomes the primary button

#define PIN_DIP_1 4   // disables mouse events (useful for development)
#define PIN_DIP_2 5   // select mouse axis (X/Y)
#define PIN_POT_1 A0  // rotation multiplier (speed)

#define PIN_LED_1 9         // mouse events enabled indicator, needs PWM (for dimming)
#define PIN_LED_BUILTIN 17  // button state indicator (RXLED on the Pro Micro, change to pin 13 on most other boards)

#define SLOW_TRIGGER_MS 2000  // enable slow mode if the main button is held for (at least) this long on startup
#define SLOW_PCT 20           // 0% for maximum accuracy (https://wiki.arcadecontrols.com/?title=Spinner_Turn_Count)
#define PCT_ADJUST 5          // when moving the controller between two machines, adjusting the speed may be needed

// The active axis is selectable using DIP switch 2 (active-low)...
#define AXIS_X 0
#define AXIS_Y 1
#define MAX_SPEED 50

#define SERIAL_BPS 115200
#define BOOT_DELAY_MS 2000  // wait for button release after slow mode is triggered

#define LED_BLINK_MS 50      // external feedback for mode changes over serial
#define LED_FEEDBACK_MS 250  // blink for how long (warning: pauses main loop)
#define LED_INTENSITY 32     // avoid piercing retinas with blue light

#define EVENT_INTERVAL_MS 4  // output mouse events at roughly 250Hz (more than enough for 60Hz games)

#define BUILTIN_LED_INVERTED true  // LOW means the LED is *ON* on the Pro Micro
#define PEDAL_JACK_AVAILABLE true  // must be false if there's no jack built into on your specific device


const char* mouse_button_names[] = {"left", "right", "middle"};

// The encoder pins are reversed because that's what's in the datasheet...
Encoder spinner(PIN_ENCODER_1B, PIN_ENCODER_1A);

uint8_t speed_percent_default = 100;
uint8_t speed_percent = speed_percent_default;  // ...can be changed over serial.


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


// Blink the external LED for the specified amount of time...
void blink_led_ms(uint16_t ms, bool leave_on) {
    int32_t stop_ms = millis() + max(1, ms);

    for (int32_t now = 0; now < stop_ms; now = millis()) {
      digitalWrite(PIN_LED_1, HIGH);
      delay(LED_BLINK_MS);
      digitalWrite(PIN_LED_1, LOW);
      delay(LED_BLINK_MS);
    }

    analogWrite(PIN_LED_1, leave_on * LED_INTENSITY);
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
    delay(SLOW_TRIGGER_MS);
    speed_percent = !digitalRead(PIN_BUTTON_1) ? SLOW_PCT : 100;
    speed_percent_default = speed_percent;  // TODO: Store this in EEPROM?
  }

  /*
   * Give the host some time to detect us before writing anything to the serial port, and also
   * ensure the button is no longer pressed when we enter the main loop to avoid accidental clicks.
   */
  uint32_t start_ms = millis();
  uint32_t loop_ts = start_ms;

  while (loop_ts < start_ms + BOOT_DELAY_MS || !digitalRead(PIN_BUTTON_1)) {
    if (speed_percent < 100) {  // ...indicate alternate (slow) mode.
      analogWrite(PIN_LED_1, LED_INTENSITY);
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
  uint32_t loop_start_ms = millis();

  // Remember previous input states to detect changes...
  static uint8_t prev_jack_present = 0xFF;
  static uint8_t prev_events_enabled = 0xFF;
  static uint8_t prev_axis = 0xFF;
  static uint8_t prev_speed = 0xFF;
  static uint8_t prev_buttons_pressed[] = {0xFF, 0xFF, 0xFF};

  uint8_t jack_present = PEDAL_JACK_AVAILABLE && digitalRead(PIN_JACK_SENSE);  // ...active-low and normally-closed.
  uint8_t events_enabled = !digitalRead(PIN_DIP_1);
  uint8_t axis = digitalRead(PIN_DIP_2);
  uint8_t speed = map(analogRead(PIN_POT_1), 0, 1023, 1, MAX_SPEED);

  // Mouse button mapping depends on the presence of an external button (pedal)...
  uint8_t buttons_pressed[] = {0, 0, 0};

  // Pedals can be normally-closed or normally-open...
  static bool button_ext_nc = false;

  // Some games have their main function on the secondary button and need the left/right buttons to be swapped...
  static bool buttons_swapped = false;


  // Let the host switch between slow/normal mode at will...
  if (Serial.available() > 0) {
    char serial_cmd = Serial.read() | 0x20;  // ...as lowercase.

    switch (serial_cmd) {
      case 's':  // (s)low speed
        speed_percent = SLOW_PCT;
        blink_led_ms(LED_FEEDBACK_MS, events_enabled);
        Serial.println("speed=slow");
        break;

      case 'n':  // (n)ormal speed
        speed_percent = 100;
        blink_led_ms(LED_FEEDBACK_MS, events_enabled);
        Serial.println("speed=normal");
        break;

      case 'w':  // s(w)ap left/right buttons
        buttons_swapped = !buttons_swapped;
        blink_led_ms(LED_FEEDBACK_MS, events_enabled);
        Serial.print("buttons=");
        Serial.println(buttons_swapped ? "swapped" : "normal");
        break;

      case 'r':  // (r)estore default settings
        speed_percent = speed_percent_default;
        buttons_swapped = false;
        blink_led_ms(LED_FEEDBACK_MS, events_enabled);
      case 'c':  // (c)urrent settings
        Serial.print("speed=");

        if (speed_percent == SLOW_PCT) {
          Serial.print("slow");
        } else if (speed_percent == 100) {
          Serial.print("normal");
        } else {
          Serial.print("custom");
        }

        Serial.print("(");
        Serial.print(speed_percent);
        Serial.println("%)");

        Serial.print("buttons=");
        Serial.println(buttons_swapped ? "swapped" : "normal");
        break;

      case '+':  // increment speed
      case '-':  // decrement speed
        if (speed_percent == SLOW_PCT || speed_percent == 100) {
          blink_led_ms(LED_FEEDBACK_MS, events_enabled);  // ...moving away from a pre-defined mode.
        }

        speed_percent = constrain(speed_percent + (serial_cmd == '+' ? 1 : -1) * PCT_ADJUST, 1, 255);
        Serial.println(serial_cmd);
        break;

      default:
        Serial.println("?");  // ...unknown command.
    }
  }

  // When a jack is inserted/removed, button positions will be reassigned...
  if (jack_present != prev_jack_present) {
    Mouse.release(MOUSE_ALL);
    button_ext_nc = !digitalRead(PIN_BUTTON_EXT);  // ...support both NO and NC pedals.

    Serial.print("jack_");

    if (jack_present) {
      Serial.print("connected");
      Serial.println(button_ext_nc ? "[normally-closed]" : "[normally-open]");
    } else {
      Serial.println("disconnected");
    }

    blink_led_ms(LED_FEEDBACK_MS, events_enabled);
    prev_jack_present = jack_present;
  }

  // Disabling mouse events is useful while testing new code...
  if (events_enabled != prev_events_enabled) {
    if (events_enabled) {
      spinner.write(0);  // ...reset to a known value.
    } else {
      Mouse.release(MOUSE_ALL);
    }

    // We don't need the indicator to be super-bright, dim it...
    analogWrite(PIN_LED_1, events_enabled * LED_INTENSITY);

    Serial.print("events_");
    Serial.println(events_enabled ? "enabled" : "disabled");

    prev_events_enabled = events_enabled;
  }

  // The X axis is usually what we want, but having the option to switch is nice...
  if (axis != prev_axis) {
    Serial.print("axis=");
    Serial.println(axis == AXIS_X ? "x" : "y");

    prev_axis = axis;
  }

  // Adjusting the speed in hardware is useful for FinalBurn Neo, for example,
  // where we cannot have the option to adjust dial sensitivity in software...
  if (speed != prev_speed) {
    Serial.print("speed=");
    Serial.print(speed);
    Serial.print("/");
    Serial.println(MAX_SPEED);

    prev_speed = speed;
  }

  // As mentioned above, the presence of an external button (pedal) remaps the buttons, and so does left/right swapping...
  if (jack_present) {
    if (buttons_swapped) {
      buttons_pressed[0] = !digitalRead(PIN_BUTTON_1);
      buttons_pressed[1] = button_ext_nc ? digitalRead(PIN_BUTTON_EXT) : !digitalRead(PIN_BUTTON_EXT);
    } else {
      buttons_pressed[0] = button_ext_nc ? digitalRead(PIN_BUTTON_EXT) : !digitalRead(PIN_BUTTON_EXT);
      buttons_pressed[1] = !digitalRead(PIN_BUTTON_1);
    }

    buttons_pressed[2] = !digitalRead(PIN_BUTTON_2);
  } else {
    if (buttons_swapped) {
      buttons_pressed[0] = !digitalRead(PIN_BUTTON_2);
      buttons_pressed[1] = !digitalRead(PIN_BUTTON_1);
    } else {
      buttons_pressed[0] = !digitalRead(PIN_BUTTON_1);
      buttons_pressed[1] = !digitalRead(PIN_BUTTON_2);
    }

    buttons_pressed[2] = 0;
  }

  // Mostly for debugging, as the builtin LED won't be visible from outside the case...
  for (uint8_t i = 0; i < 3; i++) {
    if (buttons_pressed[i] != prev_buttons_pressed[i]) {
      digitalWrite(PIN_LED_BUILTIN, BUILTIN_LED_INVERTED ? !buttons_pressed[i] : buttons_pressed[i]);
      prev_buttons_pressed[i] = buttons_pressed[i];
    }
  }

  if (!events_enabled) {
    return;  // ...don't send any mouse events.
  }

  //
  // Trigger mouse button events:
  //

  for (uint8_t i = 0; i < 3; i++) {
    uint8_t mouse_button = 0x1 << i;

    if (buttons_pressed[i] && !Mouse.isPressed(mouse_button)) {
      Serial.print(mouse_button_names[i]);
      Serial.println("_press");

      Mouse.press(mouse_button);
      continue;
    }

    if (!buttons_pressed[i] && Mouse.isPressed(mouse_button)) {
      Serial.print(mouse_button_names[i]);
      Serial.println("_release");

      Mouse.release(mouse_button);
      continue;
    }
  }

  //
  // Trigger mouse movement events:
  //

  int32_t spinner_value = spinner.read();

  if (spinner_value) {
    int8_t delta = constrain_byte(spinner_value * max(1, speed * speed_percent / 100.0));

    if (axis == AXIS_X) {
      Mouse.move(delta, 0, 0);
    } else {
      delta = -delta;  // ...so the Y axis points upwards.
      Mouse.move(0, delta, 0);
    }

    Serial.print(delta >= 0 ? "+" : "-");
    Serial.println(abs(delta));

    spinner.write(0);
  }

  delay(EVENT_INTERVAL_MS - min(0, millis() - loop_start_ms));
}


/* EOF - SpinnerMouse.ino */
