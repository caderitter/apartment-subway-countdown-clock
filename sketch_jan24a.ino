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

#define USE_ADAFRUIT_GFX_FONTS

#include <cstring>
#include <map>
#include <avr/pgmspace.h>
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
#include "Fonts/FreeSansBold18pt7b.h"
#include "Fonts/FreeMono9pt7b.h"
#include "Fonts/FreeSans9pt7b.h"
#include "Adafruit_RA8875.h"
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

// get the map lazily so the arduino doesn't crash during initialization
std::map<std::string, std::string> &getStationMap() {
  static std::map<std::string, std::string> stationMap = {
    { "F27", "Church Av" },
    { "F26", "Fort Hamilton Pkwy" },
    { "F25", "15-St Prospect Park" },
    { "F24", "7 Av" },
    { "F23", "4 Av-9 St" },
    { "F22", "Smith-9 Sts" },
    { "F21", "Carroll St" },
    { "F20", "Bergen St" },
    { "G22", "Court Sq" },
    { "G24", "21 St" },
    { "G26", "Greenpoint Av" },
    { "G28", "Nassau Av" },
    { "G29", "Metropolitan Av" },
    { "G30", "Broadway" },
    { "G31", "Flushing Av" },
    { "G32", "Myrtle-Willoughby Avs" },
    { "G33", "Bedford-Nostrand Avs" },
    { "G34", "Classon Av" },
    { "G35", "Clinton-Washington Avs" },
    { "G36", "Fulton St" }
  };
  return stationMap;
}

void drawBullet(int x, int y) {
  tft.fillCircle(x, y, 40, 0x7ca6);
  tft.setTextSize(2);
  tft.setCursor(x - 28, y + 23);
  tft.print("G");
}

void drawTrips(std::vector<std::pair<std::string, int>> trips) {
  for (int i = 0; i < trips.size(); i++) {
    tft.setFont(&FreeSansBold18pt7b);
    std::pair<std::string, int> trip = trips[i];
    std::string terminal_station_name = trip.first;
    int minutes_until = trip.second;

    tft.fillRect(40, (i * 120) + 6, 800 - 30, 110, 0x3166);
    drawBullet(90, (i * 120) + 60);
    tft.setTextSize(1);
    tft.setCursor(7, (i * 120) + 70);
    tft.print(i + 1);
    tft.setCursor(140, (i * 120) + 70);
    tft.print(terminal_station_name.c_str());

    int minutes_until_cursor_x = minutes_until < 10 ? 800 - 75 : 800 - 95;
    tft.setCursor(minutes_until_cursor_x, (i * 120) + 70);
    tft.setTextSize(2);
    // char buffer[8];
    // sprintf(buffer, "%Ld", minutes_until);
    tft.print(minutes_until);
    tft.setFont(&FreeSans9pt7b);
    tft.setTextSize(0);
    tft.setCursor(800 - 72, (i * 120) + 95);
    tft.print("MIN");
  }
}

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

  tft.fillScreen(RA8875_BLACK);

  /* Switch to text mode */
  tft.graphicsMode();

  tft.setFont(&FreeMono9pt7b);
  tft.setCursor(0, 10);
  tft.setTextSize(1);

  matrix.loadSequence(LEDMATRIX_ANIMATION_WIFI_SEARCH);
  matrix.begin();
  matrix.play(true);


  // attempt to connect to WiFi network:
  while (status != WL_CONNECTED) {
    tft.print(F("Attempting to connect to WPA SSID: "));
    tft.print(ssid);
    tft.print("...\n");
    // Connect to WPA/WPA2 network. Change this line if using open or WEP network:
    status = WiFi.begin(ssid, pass);

    // wait for connection
    delay(3000);
  }

  // you're connected now, so print out the data:
  tft.println(F("Connected to WiFi!"));
  printCurrentNet();
  printWifiData();
  matrix.clear();
  setUpRTC();

  tft.println(F("Starting connection to MTA server..."));
  if (!client.connect(g_train_endpoint, 443)) {
    tft.println("Failed.");
  } else {
    tft.println(F("Connected to MTA server!"));
    tft.fillScreen(RA8875_BLACK);
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

#define MAX_TRIPS 10

typedef struct {
  char trip_id[64];
  char terminal_stop_id[32];
  int32_t arrival_time;
  char direction_stop_id[32];
} trip_info_t;

typedef struct {
  // Target stops (northbound and southbound)
  const char *northbound_stop_id;  // "G35N"
  const char *southbound_stop_id;  // "G35S"

  // Results for both directions
  trip_info_t northbound_trips[MAX_TRIPS];
  int northbound_count;

  trip_info_t southbound_trips[MAX_TRIPS];
  int southbound_count;

  // Temporary state for current trip being processed
  char current_last_stop[32];
  bool current_trip_has_northbound;
  bool current_trip_has_southbound;
  char current_trip_id[64];
  int32_t current_northbound_arrival;
  int32_t current_southbound_arrival;
} stop_search_context_t;

void fetchAndDecode() {
  matrix.loadSequence(LEDMATRIX_ANIMATION_INFINITY_LOOP_LOADER);
  matrix.play(true);

  client.println(F("GET /Dataservice/mtagtfsfeeds/nyct%2Fgtfs-g HTTP/1.1"));
  client.println(F("Host: api-endpoint.mta.info"));
  client.println(F("Connection: Keep-Alive"));
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

  stop_search_context_t context;
  memset(&context, 0, sizeof(context));
  context.northbound_stop_id = "G35N";
  context.southbound_stop_id = "G35S";
  context.northbound_count = 0;
  context.southbound_count = 0;

  transit_realtime_FeedMessage feed = transit_realtime_FeedMessage_init_zero;
  feed.entity.funcs.decode = &feed_entity_callback;
  feed.entity.arg = &context;

  // Decode
  if (!pb_decode(&stream, transit_realtime_FeedMessage_fields, &feed)) {
    return;
  }

  RTCTime currentTime;
  RTC.getTime(currentTime);

  std::map<std::string, std::string> stationMap = getStationMap();

  std::vector<std::pair<std::string, int>> northboundTrips;
  std::vector<std::pair<std::string, int>> southboundTrips;

  Serial.println("\n=== NORTHBOUND TRIPS ===");
  Serial.print("Found ");
  Serial.print(context.northbound_count);
  Serial.println(" northbound trips:");

  for (int i = 0; i < context.northbound_count; i++) {
    Serial.print("  Trip ");
    Serial.print(context.northbound_trips[i].trip_id);
    Serial.print(" → ");
    // remove the N or S so we can fetch the stop name from the map
    char *stopId = context.northbound_trips[i].terminal_stop_id;
    int stopIdLen = std::strlen(stopId);
    stopId[stopIdLen - 1] = '\0';
    Serial.print(stationMap.at(stopId).c_str());
    Serial.print(" @ ");
    int64_t arrivalTime = context.northbound_trips[i].arrival_time - (5 * 3600);
    int64_t diffSeconds = arrivalTime - currentTime.getUnixTime();
    int64_t diffMinutes = diffSeconds / 60;
    char buffer[8];
    sprintf(buffer, "%Ld", diffMinutes);
    Serial.println(buffer);
    auto pair = std::make_pair(stationMap.at(stopId), diffMinutes);
    northboundTrips.push_back(pair);
  }

  Serial.println("\n=== SOUTHBOUND TRIPS ===");
  Serial.print("Found ");
  Serial.print(context.southbound_count);
  Serial.println(" southbound trips:");

  for (int i = 0; i < context.southbound_count; i++) {
    Serial.print("  Trip ");
    Serial.print(context.southbound_trips[i].trip_id);
    Serial.print(" → ");
    // remove the N or S so we can fetch the stop name from the map
    char *stopId = context.southbound_trips[i].terminal_stop_id;
    int stopIdLen = std::strlen(stopId);
    stopId[stopIdLen - 1] = '\0';
    Serial.print(stationMap.at(stopId).c_str());
    Serial.print(" @ ");
    int64_t arrivalTime = context.southbound_trips[i].arrival_time - (5 * 3600);
    int64_t diffSeconds = arrivalTime - currentTime.getUnixTime();
    int64_t diffMinutes = diffSeconds / 60;
    char buffer[8];
    sprintf(buffer, "%Ld", diffMinutes);
    Serial.println(buffer);
    auto pair = std::make_pair(stationMap.at(stopId), diffMinutes);
    southboundTrips.push_back(pair);
  }

  // remove negatives, sort, and trim down to 2 trips each
  cleanTrips(northboundTrips);
  cleanTrips(southboundTrips);

  // concat the trips arrays
  northboundTrips.insert(northboundTrips.end(), southboundTrips.begin(), southboundTrips.end());
  drawTrips(northboundTrips);

  matrix.clear();
  Serial.println(F("Decode successful!"));
}

void cleanTrips(std::vector<std::pair<std::string, int>> &trips) {
  // remove trips with negative time until departure
  trips.erase(
    std::remove_if(
      trips.begin(),
      trips.end(),
      [](std::pair<std::string, int> t) {
        return t.second < 0;
      }),
    trips.end());

  // sort
  std::sort(trips.begin(), trips.end(), [](std::pair<std::string, int> a, std::pair<std::string, int> b) {
    return a.second < b.second;
  });

  // trim down to 2
  trips.resize(2);

  return;
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
      tft.fillScreen(RA8875_BLACK);
      tft.setCursor(0, 10);
      tft.setFont(&FreeMono9pt7b);
      Serial.println(F("Client lost connection to server. Trying to reconnect..."));
      if (!client.connect(g_train_endpoint, 443)) {
        Serial.println(F("Failed to reconnect"));
      } else {
        Serial.println(F("Reconnected!"));
      }
    } else {
      fetchAndDecode();
    }
  }
}

void setUpRTC() {
  RTC.begin();
  tft.println(F("Connecting to NTP server..."));
  timeClient.begin();
  if (!timeClient.update()) {
    tft.println("Failed.");
    while (1)
      ;
  }
  auto timezoneOffset = -5;
  auto unixTime = timeClient.getEpochTime() + (timezoneOffset * 3600);
  tft.print("Unix time: ");
  tft.print(std::to_string(unixTime).c_str());
  tft.print("\n");
  RTCTime timeToSet = RTCTime(unixTime);
  RTC.setTime(timeToSet);

  // Retrieve the date and time from the RTC and print them
  RTCTime currentTime;
  RTC.getTime(currentTime);
  Serial.print(F("The RTC was just set to: "));
  Serial.print(String(currentTime));
}

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
  stop_search_context_t *context = (stop_search_context_t *)(*arg);

  char stop_id[32];
  transit_realtime_TripUpdate_StopTimeUpdate stop_time_update =
    transit_realtime_TripUpdate_StopTimeUpdate_init_zero;

  stop_time_update.stop_id.funcs.decode = decode_string_callback;
  stop_time_update.stop_id.arg = stop_id;

  if (!pb_decode(stream, transit_realtime_TripUpdate_StopTimeUpdate_fields, &stop_time_update)) {
    return false;
  }

  // Always update the last stop seen
  strncpy(context->current_last_stop, stop_id, sizeof(context->current_last_stop) - 1);
  context->current_last_stop[sizeof(context->current_last_stop) - 1] = '\0';

  // Check if this is the northbound stop
  if (strcmp(stop_id, context->northbound_stop_id) == 0) {
    context->current_trip_has_northbound = true;

    if (stop_time_update.has_arrival) {
      context->current_northbound_arrival = stop_time_update.arrival.time;
    }
  }

  // Check if this is the southbound stop
  if (strcmp(stop_id, context->southbound_stop_id) == 0) {
    context->current_trip_has_southbound = true;

    if (stop_time_update.has_arrival) {
      context->current_southbound_arrival = stop_time_update.arrival.time;
    }
  }

  return true;
}

bool feed_entity_callback(pb_istream_t *stream, const pb_field_t *field, void **arg) {
  stop_search_context_t *context = (stop_search_context_t *)(*arg);

  transit_realtime_FeedEntity entity = transit_realtime_FeedEntity_init_zero;

  // Reset per-trip state
  context->current_last_stop[0] = '\0';
  context->current_trip_has_northbound = false;
  context->current_trip_has_southbound = false;
  context->current_trip_id[0] = '\0';
  context->current_northbound_arrival = 0;
  context->current_southbound_arrival = 0;

  // Set up string callback for trip_id
  entity.trip_update.trip.trip_id.funcs.decode = decode_string_callback;
  entity.trip_update.trip.trip_id.arg = context->current_trip_id;

  // Set up nested callbacks
  entity.trip_update.stop_time_update.funcs.decode = &stop_time_update_callback;
  entity.trip_update.stop_time_update.arg = context;

  if (!pb_decode(stream, transit_realtime_FeedEntity_fields, &entity)) {
    return false;
  }

  // After processing all stops, save trip info if it had our target stops

  // Save northbound trip info
  if (context->current_trip_has_northbound && context->northbound_count < MAX_TRIPS) {
    trip_info_t *trip = &context->northbound_trips[context->northbound_count];

    strncpy(trip->trip_id, context->current_trip_id, sizeof(trip->trip_id) - 1);
    trip->trip_id[sizeof(trip->trip_id) - 1] = '\0';

    strncpy(trip->terminal_stop_id, context->current_last_stop, sizeof(trip->terminal_stop_id) - 1);
    trip->terminal_stop_id[sizeof(trip->terminal_stop_id) - 1] = '\0';

    strncpy(trip->direction_stop_id, context->northbound_stop_id, sizeof(trip->direction_stop_id) - 1);
    trip->direction_stop_id[sizeof(trip->direction_stop_id) - 1] = '\0';

    trip->arrival_time = context->current_northbound_arrival;

    context->northbound_count++;
  }

  // Save southbound trip info
  if (context->current_trip_has_southbound && context->southbound_count < MAX_TRIPS) {
    trip_info_t *trip = &context->southbound_trips[context->southbound_count];

    strncpy(trip->trip_id, context->current_trip_id, sizeof(trip->trip_id) - 1);
    trip->trip_id[sizeof(trip->trip_id) - 1] = '\0';

    strncpy(trip->terminal_stop_id, context->current_last_stop, sizeof(trip->terminal_stop_id) - 1);
    trip->terminal_stop_id[sizeof(trip->terminal_stop_id) - 1] = '\0';

    strncpy(trip->direction_stop_id, context->southbound_stop_id, sizeof(trip->direction_stop_id) - 1);
    trip->direction_stop_id[sizeof(trip->direction_stop_id) - 1] = '\0';

    trip->arrival_time = context->current_southbound_arrival;

    context->southbound_count++;
  }

  return true;
}

void printWifiData() {
  // print your board's IP address:
  IPAddress ip = WiFi.localIP();
  tft.print("IP Address: ");
  tft.print(ip.toString().c_str());
  tft.print("\n");
}

void printCurrentNet() {
  // print the SSID of the network you're attached to:
  tft.print("SSID: ");
  tft.print(WiFi.SSID());
  tft.print("\n");
}
