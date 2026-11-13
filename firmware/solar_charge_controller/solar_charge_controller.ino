#include <Wire.h>
#include <LiquidCrystal_I2C.h>

#define IR_PIN 3

LiquidCrystal_I2C lcd(0x27, 16, 2);

const float DIVIDER_MULT = 9.2;
const char SURNAME[] = "MAKWAKWA";

byte seriesLeft[8]   = { 0b00000, 0b01010, 0b01010, 0b11011, 0b01010, 0b01010, 0b00000, 0b00000 };
byte seriesRight[8]  = { 0b00000, 0b01010, 0b01010, 0b11011, 0b01010, 0b01010, 0b00000, 0b00000 };
byte parallelLeft[8] = { 0b00001, 0b00111, 0b00101, 0b11100, 0b00100, 0b00101, 0b00111, 0b00001 };
byte parallelRight[8]= { 0b10000, 0b11100, 0b10100, 0b00111, 0b00100, 0b10100, 0b11100, 0b10000 };

bool isSeries = false;
float targetVoltage = 0.0;
int pwmValue = 0;

int firstDigit = 0;
float enteredInputVal = 0.0;
unsigned long stateTimer = 0;
float initialA3AtEntry = 0.0;

const float SERIES_DROP_THRESHOLD = 2.0;
const unsigned long PWM_INTERVAL = 25;
const unsigned long SERIAL_INTERVAL = 250;
const int PWM_STEP = 7;

unsigned long lastPWMUpdate = 0;
unsigned long lastSerialUpdate = 0;

enum DisplayState {
  STATE_NORMAL,
  STATE_FIRST_DIGIT,
  STATE_ENTRY_CONFIRM
};

DisplayState currentState = STATE_NORMAL;

char buf[9];

float lastLCD_A3 = -999.0;
float lastLCD_A0 = -999.0;
float lastLCD_A1 = -999.0;
bool lastLCD_Mode = false;

volatile unsigned long lastEdgeTime = 0;
volatile unsigned long irBuffer = 0;
volatile unsigned long irCode = 0;
volatile byte irBitCount = 0;
volatile bool irReceiving = false;
volatile bool irReady = false;

void switchState(DisplayState newState);
void renderDisplay(bool series, float a3, float a0, float a1);
void printTargetHeader(float target);

void irISR() {
  unsigned long now = micros();
  unsigned long width = now - lastEdgeTime;
  lastEdgeTime = now;

  if (digitalRead(IR_PIN) == HIGH) {
    if (width > 7000 && width < 12000) {
      irReceiving = true;
      irBitCount = 0;
      irBuffer = 0;
    }
  } else {
    if (!irReceiving) return;
    if (width > 2000) return;
    irBuffer = (irBuffer << 1) | (width > 1000 ? 1UL : 0UL);
    irBitCount++;
    if (irBitCount >= 32) {
      irCode = irBuffer;
      irReady = true;
      irReceiving = false;
    }
  }
}

int decodeIRPulse() {
  if (!irReady) return -1;

  noInterrupts();
  unsigned long code = irCode;
  irReady = false;
  interrupts();

  byte cmd = (code >> 8) & 0xFF;
  byte cmdInv = code & 0xFF;

  if ((byte)(~cmd) != cmdInv) return -1;

  switch (cmd) {
    case 0x30: return 0;
    case 0x08: return 1;
    case 0x88: return 2;
    case 0x48: return 3;
    case 0x28: return 4;
    case 0xA8: return 5;
    case 0x68: return 6;
    case 0x18: return 7;
    case 0x98: return 8;
    case 0x58: return 9;
  }
  return -1;
}

void setup() {
  pinMode(12, OUTPUT);
  pinMode(5, OUTPUT);
  pinMode(IR_PIN, INPUT_PULLUP);

  digitalWrite(12, LOW);
  pwmValue = 0;
  analogWrite(5, pwmValue);

  Serial.begin(9600);
  attachInterrupt(digitalPinToInterrupt(IR_PIN), irISR, CHANGE);

  lcd.init();
  lcd.backlight();

  lcd.createChar(0, seriesLeft);
  lcd.createChar(1, seriesRight);
  lcd.createChar(2, parallelLeft);
  lcd.createChar(3, parallelRight);

  switchState(STATE_NORMAL);
}

void loop() {
  float a0Pin = analogRead(A0) * (5.0 / 1023.0);
  float a1Pin = analogRead(A1) * (5.0 / 1023.0);
  float a0Real = a0Pin * DIVIDER_MULT;
  float a1Real = a1Pin * DIVIDER_MULT;
  float a3Real = analogRead(A3) * (5.0 / 1023.0);

  isSeries = (digitalRead(12) == HIGH);

  // IR Input Processing
  int digit = decodeIRPulse();
  if (digit != -1) {
    if (currentState == STATE_NORMAL) {
      firstDigit = digit;
      enteredInputVal = (float)digit;
      switchState(STATE_FIRST_DIGIT);
    } 
    else if (currentState == STATE_FIRST_DIGIT) {
      float newValue = firstDigit + (digit / 10.0);
      targetVoltage = constrain(newValue, 0.0, 4.3);
      enteredInputVal = targetVoltage;
      initialA3AtEntry = a3Real;
      
      printTargetHeader(targetVoltage);
      switchState(STATE_ENTRY_CONFIRM);
    }
  }

  // Timeout for first digit entry (5 seconds)
  if (currentState == STATE_FIRST_DIGIT && (millis() - stateTimer >= 5000)) {
    targetVoltage = constrain((float)firstDigit, 0.0, 4.3);
    enteredInputVal = targetVoltage;
    initialA3AtEntry = a3Real;
    
    printTargetHeader(targetVoltage);
    switchState(STATE_ENTRY_CONFIRM);
  }

  // Instant LCD Return Logic
  if (currentState == STATE_ENTRY_CONFIRM) {
    bool voltageMoving = abs(a3Real - initialA3AtEntry) >= 0.02;
    bool safetyTimeout = (millis() - stateTimer >= 250);

    if (voltageMoving || safetyTimeout) {
      switchState(STATE_NORMAL);
    }
  }

  // PWM Regulation Loop
  if (millis() - lastPWMUpdate >= PWM_INTERVAL) {
    lastPWMUpdate = millis();
    float error = targetVoltage - a3Real;

    if (error > 0.02) {
      pwmValue = constrain(pwmValue + PWM_STEP, 0, 255);
    } else if (error < -0.02) {
      pwmValue = constrain(pwmValue - PWM_STEP, 0, 255);
    }

    analogWrite(5, pwmValue);
  }

  // Relay Circuit Switch Logic
  if (!isSeries) {
    if (pwmValue >= 250 && a3Real < targetVoltage - 0.05) {
      digitalWrite(12, HIGH);
      isSeries = true;
    }
  } else {
    if (targetVoltage < SERIES_DROP_THRESHOLD) {
      digitalWrite(12, LOW);
      isSeries = false;
    }
  }

  // Serial Output (A0, A1, A3, TARGET, PWM, MODE)
  if (millis() - lastSerialUpdate >= SERIAL_INTERVAL) {
    lastSerialUpdate = millis();
    Serial.print(F("A0: "));
    Serial.print(a0Real, 2);
    Serial.print(F("V | A1: "));
    Serial.print(a1Real, 2);
    Serial.print(F("V | A3: "));
    Serial.print(a3Real, 2);
    Serial.print(F("V | TARGET: "));
    Serial.print(targetVoltage, 2);
    Serial.print(F("V | PWM: "));
    Serial.print(pwmValue);
    Serial.print(F(" | MODE: "));
    Serial.println(isSeries ? F("SERIES") : F("PARALLEL"));
  }

  renderDisplay(isSeries, a3Real, a0Real, a1Real);
}

void printTargetHeader(float target) {
  Serial.println();
  Serial.println(F("--------------------------------"));
  Serial.print(F("TARGET SET: "));
  Serial.print(target, 2);
  Serial.println(F(" V"));
  Serial.println(F("--------------------------------"));
}

void switchState(DisplayState newState) {
  currentState = newState;
  stateTimer = millis();
  lcd.clear();

  lastLCD_A3 = -999.0;
  lastLCD_A0 = -999.0;
  lastLCD_A1 = -999.0;
}

void renderDisplay(bool series, float a3, float a0, float a1) {
  if (currentState == STATE_FIRST_DIGIT || currentState == STATE_ENTRY_CONFIRM) {
    lcd.setCursor(0, 0);
    if (series) {
      lcd.write(byte(0));
      lcd.write(byte(1));
    } else {
      lcd.write(byte(2));
      lcd.write(byte(3));
    }

    dtostrf(a3, 4, 2, buf);
    lcd.print(buf);
    lcd.print("V");

    lcd.setCursor(8, 0);
    lcd.print(SURNAME);

    lcd.setCursor(0, 1);
    lcd.print("Input = ");
    dtostrf(enteredInputVal, 4, 2, buf);
    lcd.print(buf);
    lcd.print("V");
  } 
  else if (currentState == STATE_NORMAL) {
    bool changed = (abs(a3 - lastLCD_A3) >= 0.01) ||
                   (abs(a0 - lastLCD_A0) >= 0.05) ||
                   (abs(a1 - lastLCD_A1) >= 0.05) ||
                   (series != lastLCD_Mode);

    if (!changed) return;

    lcd.setCursor(0, 0);
    if (series) {
      lcd.write(byte(0));
      lcd.write(byte(1));
    } else {
      lcd.write(byte(2));
      lcd.write(byte(3));
    }

    dtostrf(a3, 4, 2, buf);
    lcd.print(buf);
    lcd.print("V");

    int used = 2 + strlen(buf) + 1;
    for (int i = used; i < 8; i++) {
      lcd.print(' ');
    }

    lcd.setCursor(8, 0);
    lcd.print(SURNAME);

    lcd.setCursor(0, 1);
    dtostrf(a0, 5, 2, buf);
    lcd.print(buf);
    lcd.print("V");

    used = strlen(buf) + 1;
    for (int i = used; i < 8; i++) {
      lcd.print(' ');
    }

    dtostrf(a1, 5, 2, buf);
    int len = strlen(buf) + 1;
    lcd.setCursor(16 - len, 1);
    lcd.print(buf);
    lcd.print("V");

    lastLCD_A3 = a3;
    lastLCD_A0 = a0;
    lastLCD_A1 = a1;
    lastLCD_Mode = series;
  }
}