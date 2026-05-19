
#include "wifi.h"
#include "config.h"
#include <WiFi.h>
#include <Arduino.h>

// WiFi credentials
const char *ssid = "eero3917";
const char *password = "falcon3917";

void setupWiFi()
{
    WiFi.disconnect();
    WiFi.mode(WIFI_STA);
    WiFi.begin(ssid, password);

    while (WiFi.status() != WL_CONNECTED)
    {
        delay(1500);
    }

    Serial.println("WiFi connected");
}

void setupWiFiStation()
{

    WiFi.mode(WIFI_AP_STA);
    WiFi.begin(ssid, password);

    Serial.print("Connecting to WiFi");
    while (WiFi.status() != WL_CONNECTED)
    {
        delay(500);
        Serial.print(".");
    }
    Serial.println();

    Serial.print("Connected to WiFi. IP address: ");
    Serial.println(WiFi.localIP());
}

void connectToWiFi()
{
    Serial.println("Connecting to WiFi...");
    WiFi.begin(ssid, password);

    while (WiFi.status() != WL_CONNECTED)
    {
        delay(500);
        Serial.print(".");
    }

    Serial.println("");
    Serial.println("WiFi connected");
    Serial.println("IP address: ");
    Serial.println(WiFi.localIP());
}
