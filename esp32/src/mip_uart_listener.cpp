#include "mip_uart_listener.h"

#include "MiP_commands.h"
#include "robot_status.h"
#include "lib_websocket.h"

extern HardwareSerial MiPSerial;
extern MiP MyMiP;

// -----------------------------------------------------------------------------
// MiP UART packet parser
// -----------------------------------------------------------------------------

static uint8_t g_cmd = 0;
static uint8_t g_payload[8];
static uint8_t g_expected = 0;
static uint8_t g_index = 0;
static bool g_waitingPayload = false;

static int expectedPayloadLength(uint8_t cmd)
{
  switch (cmd)
  {
    case 0x79: return 2; // MIP status: battery + position
    case 0x81: return 1; // weight update
    case 0x83: return 5; // chest LED
    case 0x8B: return 4; // head LEDs
    case 0x85: return 4; // odometer
    case 0x0A: return 1; // gesture detect
    case 0x0C: return 1; // radar response
    case 0x0D: return 1; // radar/gesture mode status
    case 0x0F: return 2; // detection mode status
    case 0x04: return 1; // MiP detected
    case 0x1A: return 0; // shake detected
    case 0x11: return 1; // IR control status
    case 0x16: return 1; // volume
    case 0x82: return 1; // game mode
    case 0x1D: return 1; // clap times
    case 0x1F: return 3; // clap status
    default: return -1;
  }
}

static const char* mipPositionName(uint8_t position)
{
  switch (position)
  {
    case 0x00: return "on_back";
    case 0x01: return "face_down";
    case 0x02: return "upright";
    case 0x03: return "picked_up";
    case 0x04: return "hand_stand";
    case 0x05: return "face_down_on_tray";
    case 0x06: return "on_back_with_kickstand";
    default:   return "unknown";
  }
}

static const char* radarName(uint8_t radar)
{
  switch (radar)
  {
    case 0x01: return "clear";
    case 0x02: return "object_10_30cm";
    case 0x03: return "object_under_10cm";
    default:   return "unknown";
  }
}

static const char* gestureName(uint8_t gesture)
{
  switch (gesture)
  {
    case 0x0A: return "left";
    case 0x0B: return "right";
    case 0x0C: return "center_sweep_left";
    case 0x0D: return "center_sweep_right";
    case 0x0E: return "center_hold";
    case 0x0F: return "forward";
    case 0x10: return "back";
    default:   return "unknown";
  }
}

static void handleMipPacket(uint8_t cmd, const uint8_t* data, uint8_t len)
{
  Serial.printf("[MIP UART RX] cmd=0x%02X len=%u", cmd, len);
  for (uint8_t i = 0; i < len; i++)
  {
    Serial.printf(" %02X", data[i]);
  }
  Serial.println();

  if (cmd == 0x79 && len >= 2)
  {
    const uint8_t batteryRaw = data[0];
    const uint8_t position = data[1];

    // We store battery as raw only for now. We do not need to publish it unless requested.
    static uint8_t lastPosition = 0xFF;

    robotStatusSetMipBatteryRaw(batteryRaw);
    robotStatusSetMipPosition(position, mipPositionName(position));

    if (position != lastPosition)
    {
      lastPosition = position;
      publishRobotState("mip_position_changed");
    }
  }
 
 
 /*
  else if (cmd == 0x0C && len >= 1)
  {
    const uint8_t radar = data[0];
    const bool blocked = (radar == 0x02 || radar == 0x03);

    robotStatusSetRadar(radar, radarName(radar), blocked);
    publishRobotState("radar");
  }


    else if (cmd == 0x0C && len >= 1)
    {
    const uint8_t radar = data[0];
    const bool blocked = (radar == 0x02 || radar == 0x03);

    static uint8_t lastRadar = 0xFF;
    static bool lastBlocked = false;
    static uint32_t lastRadarPublishMs = 0;

    uint32_t now = millis();

    bool changed = (radar != lastRadar) || (blocked != lastBlocked);
    bool periodicRefresh = blocked && (now - lastRadarPublishMs > 1500);

    robotStatusSetRadar(radar, radarName(radar), blocked);

    if (changed || periodicRefresh)
    {
        lastRadar = radar;
        lastBlocked = blocked;
        lastRadarPublishMs = now;

        publishRobotState(changed ? "radar_changed" : "radar_blocked");
    }
    }
*/
  else if (cmd == 0x0C && len >= 1)
  {
    const uint8_t radar = data[0];
    const bool blocked = (radar == 0x02 || radar == 0x03);

    static uint8_t lastRadar = 0xFF;
    static bool lastBlocked = false;
    static uint32_t lastRadarPublishMs = 0;

    const uint32_t now = millis();

    const bool changed = (radar != lastRadar) || (blocked != lastBlocked);

    // While blocked, refresh bridge state fairly quickly.
    // This keeps the server aware without flooding it every UART packet.
    const bool blockedRefresh = blocked && ((now - lastRadarPublishMs) > 700);

    // When clear and unchanged, do not keep spamming clear updates.
    robotStatusSetRadar(radar, radarName(radar), blocked);

    if (changed || blockedRefresh)
    {
      lastRadar = radar;
      lastBlocked = blocked;
      lastRadarPublishMs = now;

      publishRobotState(changed ? "radar_changed" : "radar_blocked");
    }
  }

  else if (cmd == 0x0A && len >= 1)
  {
    const uint8_t gesture = data[0];

    robotStatusSetGesture(gesture, gestureName(gesture));
    publishRobotState("gesture");
  }
  else if (cmd == 0x0D && len >= 1)
  {
    const uint8_t mode = data[0];

    robotStatusSetRadarGestureMode(mode);
    publishRobotState("radar_mode");
  }
  else if (cmd == 0x0F && len >= 2)
  {
    robotStatusSetDetectionStatus(data[0], data[1]);
    publishRobotState("detection_status");
  }
  else if (cmd == 0x04 && len >= 1)
  {
    robotStatusSetMipDetected(data[0]);
    publishRobotState("mip_detected");
  }
  else if (cmd == 0x1A)
  {
    robotStatusSetShakeDetected(true);
    publishRobotState("shake");
  }
}

static void resetParser()
{
  g_cmd = 0;
  g_expected = 0;
  g_index = 0;
  g_waitingPayload = false;
}

static void feedMipByte(uint8_t b)
{
  if (!g_waitingPayload)
  {
    int expected = expectedPayloadLength(b);

    if (expected < 0)
    {
      Serial.printf("[MIP UART RX] unknown byte 0x%02X\n", b);
      return;
    }

    g_cmd = b;
    g_expected = (uint8_t)expected;
    g_index = 0;

    if (g_expected == 0)
    {
      handleMipPacket(g_cmd, nullptr, 0);
      resetParser();
      return;
    }

    g_waitingPayload = true;
    return;
  }

  g_payload[g_index++] = b;

  if (g_index >= g_expected)
  {
    handleMipPacket(g_cmd, g_payload, g_expected);
    resetParser();
  }
}


static bool isHexChar(uint8_t c)
{
  return (c >= '0' && c <= '9') ||
         (c >= 'A' && c <= 'F') ||
         (c >= 'a' && c <= 'f');
}

static uint8_t hexValue(uint8_t c)
{
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  return 0;
}

static void feedMipUartByte(uint8_t b)
{
  static bool haveHighNibble = false;
  static uint8_t highNibble = 0;

  // Your current MiP serial stream appears to be ASCII hex, e.g.
  // "0C03" instead of raw bytes 0x0C 0x03.
  if (isHexChar(b))
  {
    uint8_t nibble = hexValue(b);

    if (!haveHighNibble)
    {
      highNibble = nibble;
      haveHighNibble = true;
    }
    else
    {
      uint8_t decoded = (highNibble << 4) | nibble;
      haveHighNibble = false;

      Serial.printf("[MIP UART HEX] %02X\n", decoded);
      feedMipByte(decoded);
    }

    return;
  }

  // Ignore separators if the library ever emits them.
  if (b == '\r' || b == '\n' || b == ' ' || b == '\t')
  {
    return;
  }

  // Fallback for true binary packets.
  haveHighNibble = false;
  feedMipByte(b);
}

// -----------------------------------------------------------------------------
// Task
// -----------------------------------------------------------------------------

void mipUartListenerTask(void* parameter)
{
  Serial.println("[MIP UART RX] listener task started");

  while (true)
  {
    while (MiPSerial.available() > 0)
    {
    uint8_t b = (uint8_t)MiPSerial.read();
    feedMipUartByte(b);
    }

    vTaskDelay(pdMS_TO_TICKS(2));
  }
}

void setupMipUartListener()
{
  xTaskCreatePinnedToCore(
    mipUartListenerTask,
    "mipUartRx",
    4096,
    nullptr,
    1,
    nullptr,
    1
  );
}

// -----------------------------------------------------------------------------
// Optional request helpers
// -----------------------------------------------------------------------------

void requestMipPositionStatus()
{
  MyMiP.requestStatus();
}

void enableMipRadarMode()
{
  // 0x04 = radar mode on, gesture disabled.
  MyMiP.setGestureRadarMode(0x04);
}

void disableMipRadarGestureMode()
{
  MyMiP.setGestureRadarMode(0x00);
}