/* GPIO-level wiring diagnostic; Bluetooth and I2S are intentionally inactive. */
#include <Arduino.h>
#include <stdio.h>

static const uint8_t outputs[] = {18, 23, 27, 22};
static const uint8_t inputs[] = {26, 25, 35, 19, 33};
static unsigned output_mask;
static bool driving;

static void safe_pins() {
  for (auto pin : outputs) pinMode(pin, INPUT_PULLDOWN);
  // GPIO35 has no internal pull resistor. The verifier tests both logic levels.
  for (auto pin : inputs) pinMode(pin, pin == 35 ? INPUT : INPUT_PULLDOWN);
  output_mask = 0;
  driving = false;
}

static void command(const char *line) {
  if (!strcmp(line, "READ")) {
    unsigned mask = 0;
    for (unsigned i = 0; i < sizeof(inputs); ++i)
      if (digitalRead(inputs[i])) mask |= 1U << i;
    Serial.printf("@STATE ESP I=%02x O=%02x DRIVE=%u\n", mask, output_mask, driving);
  } else if (!strcmp(line, "SAFE")) {
    safe_pins(); Serial.println("@SAFE 0");
  } else {
    unsigned mask;
    char extra;
    if (sscanf(line, "OUT %x %c", &mask, &extra) == 1 && mask <= 15) {
      for (unsigned i = 0; i < sizeof(outputs); ++i) {
        digitalWrite(outputs[i], !!(mask & (1U << i)));
        pinMode(outputs[i], OUTPUT);
      }
      output_mask = mask; driving = true; Serial.println("@OUT 0");
    } else Serial.println("@ERROR unknown command");
  }
}

void setup() {
  safe_pins();
  Serial.begin(115200);
  Serial.println("@READY ESP WIRING_V1");
}

void loop() {
  static char line[64];
  static unsigned used = 0;
  static bool overflow = false;
  while (Serial.available()) {
    char c = Serial.read();
    if (c == '\n') {
      if (!overflow) { line[used] = 0; command(line); }
      else Serial.println("@ERROR line too long");
      used = 0; overflow = false;
    } else if (c != '\r') {
      if (used < sizeof(line) - 1) line[used++] = c;
      else overflow = true;
    }
  }
  delay(1);
}
