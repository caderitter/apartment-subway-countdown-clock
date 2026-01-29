/******************************************************************
 This is an example for the Adafruit RA8875 Driver board for TFT displays
 ---------------> http://www.adafruit.com/products/1590
 The RA8875 is a TFT driver for up to 800x480 dotclock'd displays
 It is tested to work with displays in the Adafruit shop. Other displays
 may need timing adjustments and are not guanteed to work.

 Adafruit invests time and resources providing this open
 source code, please support Adafruit and open-source hardware
 by purchasing products from Adafruit!

 Written by Limor Fried/Ladyada for Adafruit Industries.
 BSD license, check license.txt for more information.
 All text above must be included in any redistribution.
 ******************************************************************/


#include <SPI.h>
#include <pb_encode.h>
#include <pb_decode.h>
#include <Print.h>
#include <Stream.h>
#include <NTPClient.h>
#include <WiFiUdp.h>
#include "RTC.h"
#include "WiFiS3.h"
#include "arduino_secrets.h"
#include "Adafruit_GFX.h"
#include "Adafruit_RA8875.h"
#include "Fonts/FreeSansBold18pt7b.h"
#include "gtfs-realtime.pb.h"
#include "gtfs-realtime-NYCT.pb.h"

#include "Arduino_LED_Matrix.h"

static bool pb_print_write(pb_ostream_t *stream, const pb_byte_t *buf, size_t count) {
  Print *p = reinterpret_cast<Print *>(stream->state);
  size_t written = p->write(buf, count);
  return written == count;
};

pb_ostream_s as_pb_ostream(Print &p) {
  return { pb_print_write, &p, SIZE_MAX, 0 };
};

static bool pb_stream_read(pb_istream_t *stream, pb_byte_t *buf, size_t count) {
  Stream *s = reinterpret_cast<Stream *>(stream->state);
  size_t read = s->readBytes(buf, count);
  if (read == 0) {
    stream->bytes_left = 0;
  }
  return read == count;
};

pb_istream_s as_pb_istream(Stream &s) {
#ifndef PB_NO_ERRMSG
  return { pb_stream_read, &s, SIZE_MAX, 0 };
#else
  return { pb_stream_read, &s, SIZE_MAX };
#endif
};

// Library only supports hardware SPI at this time
// Connect SCLK to UNO Digital #13 (Hardware SPI clock)
// Connect MISO to UNO Digital #12 (Hardware SPI MISO)
// Connect MOSI to UNO Digital #11 (Hardware SPI MOSI)
#define RA8875_INT 3
#define RA8875_CS 10
#define RA8875_RESET 9

Adafruit_RA8875 tft = Adafruit_RA8875(RA8875_CS, RA8875_RESET);
uint16_t tx, ty;

char ssid[] = SECRET_SSID;
char pass[] = SECRET_PASS;
int status = WL_IDLE_STATUS;

char g_train_endpoint[] = "api-endpoint.mta.info";
char g_target_stop_id[] = "G35N";

unsigned long currentMillis;
unsigned long previousMillis = 0;

WiFiSSLClient client;
WiFiUDP Udp;
NTPClient timeClient(Udp);

ArduinoLEDMatrix matrix;

void setup() {
  Serial.begin(9600);
  while (!Serial)
    ;

  /* Initialize the display using 'RA8875_480x80', 'RA8875_480x128', 'RA8875_480x272' or 'RA8875_800x480' */
  if (!tft.begin(RA8875_800x480)) {
    Serial.println("RA8875 Not Found!");
    while (1)
      ;
  }

  tft.displayOn(true);
  tft.GPIOX(true);                               // Enable TFT - display enable tied to GPIOX
  tft.PWM1config(true, RA8875_PWM_CLK_DIV1024);  // PWM output for backlight
  tft.PWM1out(255);

  // writeReg(0x22, 1 << 4);
  // writeReg(0x20, 1 << 2);

  tft.fillScreen(RA8875_BLACK);

  /* Switch to text mode */
  tft.textMode();
  tft.cursorBlink(32);

  /* Set the cursor location (in pixels) */
  tft.textSetCursor(10, 10);
  tft.textTransparent(RA8875_WHITE);

  matrix.loadSequence(LEDMATRIX_ANIMATION_WIFI_SEARCH);
  matrix.begin();
  matrix.play(true);
  // attempt to connect to WiFi network:
  while (status != WL_CONNECTED) {
    tft.textWrite("Attempting to connect to WPA SSID: ");
    tft.textWrite(ssid);
    tft.textWrite("... ");
    // Connect to WPA/WPA2 network. Change this line if using open or WEP network:
    status = WiFi.begin(ssid, pass);

    // wait for connection
    delay(1000);
  }

  // you're connected now, so print out the data:
  matrix.clear();
  tft.textWrite("Connected to WiFi! ");
  printCurrentNet();
  printWifiData();
  setUpRTC();

  tft.textWrite("Starting connection to MTA server... ");
  if (!client.connect(g_train_endpoint, 443)) {
    tft.textWrite("Failed. ");
  } else {
    tft.textWrite("Connected to MTA server! ");

    fetchAndDecode();
  }
  previousMillis = millis();
}

void drawStaticUI() {
  tft.graphicsMode();
  tft.fillScreen(RA8875_BLACK);
  tft.fillRect(20, 0, 680, 75, 0xc618);
  tft.fillRect(20, 85, 680, 75, 0xc618);
}

void fetchAndDecode() {
  matrix.loadSequence(LEDMATRIX_ANIMATION_INFINITY_LOOP_LOADER);
  matrix.play(true);

  client.println("GET /Dataservice/mtagtfsfeeds/nyct%2Fgtfs-g HTTP/1.1");
  client.println("Host: api-endpoint.mta.info");
  client.println("Connection: Keep-Alive");
  client.println();

  // Skip HTTP headers
  while (client.connected()) {
    String line = client.readStringUntil('\n');
    Serial.println(line.c_str());
    if (line == "\r") {
      break;  // Headers done
    }
  }

  pb_istream_t stream = as_pb_istream(client);

  transit_realtime_FeedMessage feed = transit_realtime_FeedMessage_init_zero;
  feed.entity.funcs.decode = &feed_entity_callback;

  // Decode
  if (!pb_decode(&stream, transit_realtime_FeedMessage_fields, &feed)) {
    Serial.println("Decode failed");
    Serial.println(PB_GET_ERROR(&stream));
    Serial.println("Bytes left in stream:");
    Serial.println(stream.bytes_left);
    return;
  }

  matrix.clear();
  Serial.println("Decode successful!");
}

void loop() {
  // Continuously update the current time
  currentMillis = millis();

  // Check if 30 seconds have passed
  if (currentMillis - previousMillis >= 30000) {
    Serial.println("loop!");
    // Save the time the function ran last
    // Use `previousMillis = currentMillis;` to avoid time slippage
    previousMillis = currentMillis;

    if (!client.connected()) {
      Serial.println("Client lost connection to server. Trying to reconnect...");
      if (!client.connect(g_train_endpoint, 443)) {
        Serial.println("Failed to reconnect");
      } else {
        Serial.println("Reconnected!");
      }
    } else {
      fetchAndDecode();
    }
  }
}

void setUpRTC() {
  RTC.begin();
  tft.textWrite("Connecting to NTP server... ");
  timeClient.begin();
  timeClient.update();
  auto timezoneOffset = -5;
  auto unixTime = timeClient.getEpochTime() + (timezoneOffset * 3600);
  tft.textWrite("Unix time: ");
  tft.textWrite(std::to_string(unixTime).c_str());
  tft.textWrite(" ");
  RTCTime timeToSet = RTCTime(unixTime);
  RTC.setTime(timeToSet);

  // Retrieve the date and time from the RTC and print them
  RTCTime currentTime;
  RTC.getTime(currentTime);
  Serial.println("The RTC was just set to: " + String(currentTime));
}

typedef struct {
  bool found_target_in_current_trip;
  char terminal_stop_id[32];
} stop_search_context_t;

bool decode_string_callback(pb_istream_t *stream, const pb_field_t *field, void **arg) {
  char *dest = (char *)(*arg);

  int strlen = stream->bytes_left;

  dest[strlen] = '\0';

  if (!pb_read(stream, (uint8_t *)dest, strlen)) {
    return false;
  }

  return true;
}

bool stop_time_update_callback(pb_istream_t *stream, const pb_field_t *field, void **arg) {
  transit_realtime_TripUpdate_StopTimeUpdate stop_time_update =
    transit_realtime_TripUpdate_StopTimeUpdate_init_zero;

  char stop_id_buffer[32];
  stop_time_update.stop_id.funcs.decode = &decode_string_callback;
  stop_time_update.stop_id.arg = stop_id_buffer;

  if (!pb_decode(stream, transit_realtime_TripUpdate_StopTimeUpdate_fields, &stop_time_update)) {
    Serial.println("stop_time_update_callback failed");
    Serial.println("Bytes left in stream:");
    Serial.println(stream->bytes_left);
    return false;
  }

  Serial.println(stop_id_buffer);

  if (g_target_stop_id && strcmp(stop_id_buffer, g_target_stop_id) == 0) {
    int64_t arrivalTime = stop_time_update.arrival.time - (5 * 3600);
    RTCTime currentTime;
    RTC.getTime(currentTime);
    int64_t diffSeconds = arrivalTime - currentTime.getUnixTime();
    int64_t diffMinutes = diffSeconds / 60;

    char buffer[8];
    sprintf(buffer, "%Ld", diffMinutes);

    tft.textWrite(stop_id_buffer);
    tft.textWrite(" ");
    tft.textWrite(" ");
    tft.textWrite("Expected arrival in ");
    tft.textWrite(buffer);
    tft.textWrite(" minutes. ");
    Serial.println(diffMinutes);
  }

  return true;
}

bool feed_entity_callback(pb_istream_t *stream, const pb_field_t *field, void **arg) {
  transit_realtime_FeedEntity entity = transit_realtime_FeedEntity_init_zero;

  entity.trip_update.stop_time_update.funcs.decode = &stop_time_update_callback;

  if (!pb_decode(stream, transit_realtime_FeedEntity_fields, &entity)) {
    Serial.println("feed_entity_callback failed");
    Serial.println("Bytes left in stream:");
    Serial.println(stream->bytes_left);
    return false;
  }

  return true;
}

void printWifiData() {
  // print your board's IP address:
  IPAddress ip = WiFi.localIP();
  tft.textWrite("IP Address: ");
  tft.textWrite(ip.toString().c_str());
  tft.textWrite(" ");
}

void printCurrentNet() {
  // print the SSID of the network you're attached to:
  tft.textWrite("SSID: ");
  tft.textWrite(WiFi.SSID());
  tft.textWrite(" ");
}

void writeReg(uint8_t reg, uint8_t val) {
  writeCommand(reg);
  writeData(val);
}
// *--*--*--*--*--*--*--*--*--*--*--*--*--*--*--*--*--*--*--*--*--*--*--*--*
void writeData(uint8_t d) {
  uint32_t spi_speed = 12000000;  //!< 12MHz
  digitalWrite(RA8875_CS, LOW);
  //spi_begin();
  SPI.beginTransaction(SPISettings(spi_speed, MSBFIRST, SPI_MODE0));
  SPI.transfer(RA8875_DATAWRITE);
  SPI.transfer(d);
  //spi_end();
  SPI.endTransaction();
  digitalWrite(RA8875_CS, HIGH);
}
// *--*--*--*--*--*--*--*--*--*--*--*--*--*--*--*--*--*--*--*--*--*--*--*--*
void writeCommand(uint8_t d) {
  uint32_t spi_speed = 12000000;  //!< 12MHz
  digitalWrite(RA8875_CS, LOW);
  //spi_begin();
  SPI.beginTransaction(SPISettings(spi_speed, MSBFIRST, SPI_MODE0));

  SPI.transfer(RA8875_CMDWRITE);
  SPI.transfer(d);
  //spi_end();
  SPI.endTransaction();
  digitalWrite(RA8875_CS, HIGH);
}
