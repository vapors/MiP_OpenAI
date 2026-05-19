/*
  MiP Command Runner (ESP32-S3 friendly)
  - Interactive serial menu to exercise common commands.
  - Tested with Waveshare ESP32-S3-Touch-LCD-3.5 on UART1:
      RX = GPIO47  (ESP32 RX  <- MiP TX)
      TX = GPIO48  (ESP32 TX  -> MiP RX)

  Notes:
  - UART_Select_S / UART_Select_R are optional. If you are NOT using the ProMini/Proto pack
    select lines, set them to -1 (this library version safely ignores -1).
*/

#include <Arduino.h>
#include "MiP_commands.h"

// ====== USER CONFIG ======
static const int MIP_RX_PIN = 47;
static const int MIP_TX_PIN = 48;

// Set to -1 if not used (safe in this patched library)
static const int UART_SELECT_S = -1;
static const int UART_SELECT_R = -1;

// MiP uses 115200 on UART
static const uint32_t MIP_BAUD = 115200;

// =========================
HardwareSerial MiPSerial(1);
MiP mip(MiPSerial, UART_SELECT_S, UART_SELECT_R);

static void printMenu();
static void flushLine();
static int readIntBlocking();
static char readCharBlocking();

void setup() {
  Serial.begin(115200);
  delay(300);

  Serial.println();
  Serial.println("MiP Command Runner starting...");

  MiPSerial.begin(MIP_BAUD, SERIAL_8N1, MIP_RX_PIN, MIP_TX_PIN);
  delay(50);

  mip.init(MIP_BAUD);

  Serial.println("MiP init sent. If you have UART wiring correct, commands should work now.");
  Serial.println("Wiring: ESP32 TX48 -> MiP RX, ESP32 RX47 <- MiP TX, GND <-> GND");
  printMenu();
}

void loop() {
  if (!Serial.available()) return;

  char c = (char)Serial.read();
  if (c == '\r' || c == '\n') return;

  switch (c) {
    case 'h':
    case '?':
      printMenu();
      break;

    case '1': { // sound sweep
      Serial.println("Playing a few sounds...");
      mip.playSingleSound(BURP);
      delay(600);
      //mip.playSingleSound(GIGGLE);
      //delay(600);
      //mip.playSingleSound(HICUP);
      //delay(800);
      Serial.println("Done.");
      break;
    }

    case '2': { // chest LED RGB cycle
      Serial.println("Chest LED RGB cycle...");
      mip.setChestLED(255, 0, 0); delay(300);
      mip.setChestLED(0, 255, 0); delay(300);
      mip.setChestLED(0, 0, 255); delay(300);
      mip.setChestLED(0, 0, 0);   delay(200);
      Serial.println("Done.");
      break;
    }

    case '3': { // head LEDs pattern
      Serial.println("Head LEDs pattern...");
      mip.setHeadLEDs(1,0,0,0); delay(250);
      mip.setHeadLEDs(0,1,0,0); delay(250);
      mip.setHeadLEDs(0,0,1,0); delay(250);
      mip.setHeadLEDs(0,0,0,1); delay(250);
      mip.setHeadLEDs(1,1,1,1); delay(250);
      mip.setHeadLEDs(0,0,0,0); delay(250);
      Serial.println("Done.");
      break;
    }

    case '4': { // set volume
      Serial.println("Enter volume 0..7 then press Enter:");
      int v = readIntBlocking();
      if (v < 0) v = 0;
      if (v > 7) v = 7;
      mip.setVolume((int8_t)v);
      delay(100);
      int got = mip.getVolume();
      Serial.print("Set volume to "); Serial.print(v);
      Serial.print(", getVolume() returned "); Serial.println(got);
      break;
    }

    case '5': { // stand up / get up
      Serial.println("StandUp(1)...");
      mip.standUp(1);
      break;
    }

    case '6': { // fall down
      Serial.println("StandUp(0)...");
      mip.standUp(0);
      break;
    }

    case '7': { // drive distance
      Serial.println("Enter distance (cm-ish, depends on MiP firmware) then Enter:");
      int dist = readIntBlocking();
      Serial.println("Enter angle (-180..180) then Enter:");
      int ang = readIntBlocking();
      Serial.print("distanceDrive("); Serial.print(dist); Serial.print(", "); Serial.print(ang); Serial.println(")");
      mip.distanceDrive((int16_t)dist, (int16_t)ang);
      break;
    }

    case '8': { // turn angle
      Serial.println("Turn: direction (0=left, 1=right) then Enter:");
      int dir = readIntBlocking();
      Serial.println("Speed (0..32 typical) then Enter:");
      int spd = readIntBlocking();
      Serial.println("Angle (0..180) then Enter:");
      int ang = readIntBlocking();
      mip.turnAngle((int8_t)dir, (int8_t)spd, (uint16_t)ang);
      break;
    }

    case 's': { // stop
      Serial.println("Stop");
      mip.stop();
      break;
    }

    default:
      Serial.println("Unknown. Press 'h' for help.");
      break;
  }

  // Eat remainder of line if user typed quickly
  flushLine();
}

static void printMenu() {
  Serial.println();
  Serial.println("=== MiP Command Runner Menu ===");
  Serial.println("h or ? : help");
  Serial.println("1      : play a few sounds");
  Serial.println("2      : chest LED RGB cycle");
  Serial.println("3      : head LEDs sweep");
  Serial.println("4      : set volume (0..7) + read back");
  Serial.println("5      : stand up");
  Serial.println("6      : fall down");
  Serial.println("7      : distance drive (distance, angle)");
  Serial.println("8      : turnAngle (dir, speed, angle)");
  Serial.println("s      : stop");
  Serial.println();
}

static void flushLine() {
  while (Serial.available()) {
    char c = (char)Serial.peek();
    if (c == '\n') { Serial.read(); break; }
    if (c == '\r') { Serial.read(); continue; }
    // If user typed a lot, just clear it.
    Serial.read();
  }
}

static char readCharBlocking() {
  while (!Serial.available()) delay(1);
  return (char)Serial.read();
}

static int readIntBlocking() {
  // Read until newline; parse int.
  String s;
  while (true) {
    char c = readCharBlocking();
    if (c == '\r') continue;
    if (c == '\n') break;
    s += c;
    // Also echo back for clarity
    Serial.write(c);
  }
  Serial.println();
  s.trim();
  if (s.length() == 0) return 0;
  return s.toInt();
}
