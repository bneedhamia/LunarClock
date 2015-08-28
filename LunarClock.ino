/*
 * Net-connected lunar clock.
 * Shows the current phase of the moon,
 * as reported by HM Nautical Almanac Office: Miscellanea,
 * Daily Rise/set and Twilight times for the British Isles,
 * at http://astro.ukho.gov.uk/nao/miscellanea/birs2.html
 *
 * Copyright (c) 2015 Bradford Needham
 * { @bneedhamia , https://www.needhamia.com }
 *
 * Licensed under GPL V2
 * a copy of which should have been supplied with this file.
 */
 
/*
 * This sketch requires an Arduino Mega 2560,
 * a SparkFun WiFi Shield ESP8266 on it,
 * XXX and hardware to be chosen.
 */

/*
 * Include the the modified Sparkfun ESP8266 Shield library,
 * modified to support the Arduino Mega hardware Serial pins.
 * XXX fork the Sparkfun library, branch it properly, submit a pull request,
 * then point to it here.
 */
#include <SoftwareSerial.h>       // Required by SparkfunESP8266WiFi.h
#include <SparkFunESP8266WiFi.h>  //XXX github url of the modified library to be put here.
#include <float.h>                // For DBL_MAX
#include <ESP8266HttpRead.h>      // https://github.com/bneedhamia/ESP8266HttpRead

/*
 * Pins:
 *
 * Wifi ESP8266 Shield related Pins:
 * Note: the shield header pins 8 and 9 must be cut so they don't connect to the Arduino.
 * pin 15 = Mega Serial3 Rx (ESP8266 Tx).  Mega pin 15 must be wired to pin 8 on the shield.
 * pin 14 = Mega Serial3 Tx (ESP8266 Rx).  Mega pin 14 must be wired to pin 9 on the shield.
 *
 * XXX more to come.
 *
 * Pins 10-13 are the Mega SPI bus.
 *  Note: that means that the normal pin 13 LED is not usable as an LED.
 */

// Fill these constants in with values for your network.
char *wifiSsid = "XXXYourSSIDXXX";          // SSID of the network to connect to.
char *wifiPassword = "XXXYourPasswordXXX"; // password of the network.

ESP8266Client client;   // Client for using the ESP8266 WiFi board.

/*
 * The date is received from the Http "Date:" header in the web response we receive.
 * Note: that date is GMT rather than local time, but we don't care
 * because we use the date/time only to decide when to read the web site again.
 */
ESP8266HttpRead::HttpDateTime dateTimeUTC;

double daysSinceNewMoon;  // number of days (0.0 .. 29.53) since the New Moon.
int illuminatedPC;       // percent (0..100) of the moon's surface that's illuminated.

const char HttpServer[] = "astro.ukho.gov.uk";
const int HttpPort = 80;

/*
 * The site to query:
 *   HM Nautical Almanac Office: Miscellanea.
 *   Daily Rise/set and Twilight times for the British Isles.
 * 
 * Notes:
 * - HTTP 1.0 (rather than 1.1) is specified here to prevent
 *   the server from using Transfer-encoding: chunked,
 *   which is difficult to read.
 * - "Connection: close" is used to cause the server to
 *   close the connection when the response is completed.
 * - The page takes several seconds to load, because
 *   the site seems to pause in the middle, and it's a long page.
 * - The current age of the moon and % illumination is given
 *   very near the end of the text of the web page.
 */
const String HttpRequest = "GET /nao/miscellanea/birs2.html HTTP/1.0\n"
                           "Host: astro.ukho.gov.uk\n"
                           "Connection: close\n"
                           "\n";

boolean query(ESP8266HttpRead::HttpDateTime *pDateTimeUTC, double *pDaysSinceNewMoon,
  int *pIlluminatedPC);

void setup() {
  int ret;            // temporary return value.

  Serial.begin(9600);
  Serial.println(F("Reset."));
  
  // Give the developer a chance to start the Serial Monitor
  delay(5000);
  
  Serial.println(F("Starting..."));

  // Setup the WiFi shield
 
  if (!esp8266.begin(9600, ESP8266_HARDWARE_SERIAL3)) {
    Serial.println(F("WiFi init failed."));
    return;
  }
  ret = esp8266.getMode();
  if (ret != ESP8266_MODE_STA) {
    if (esp8266.setMode(ESP8266_MODE_STA) < 0) {
      Serial.println(F("WiFi setMode() failed."));
      return;
    }
  }
  
  //XXX on success, set a state to let the loop know.

  //XXX this wifi activity will eventually move into the 'get lunar phase' function.

  Serial.print(F("Connecting to "));
  Serial.print(wifiSsid);
  Serial.println(F(" ..."));
  
  ret = esp8266.status();
  if (ret > 0 ) {
    // Already connected, but probably messed up, so disconnect then reconnect.
    esp8266.disconnect();
  }
  
  // Not connected; try to connect to our hotspot.

  ret = esp8266.connect(wifiSsid, wifiPassword);
  if (ret < 0) {
    Serial.println(F("Failed to connect"));
    return;
  }

  IPAddress ipAddr = esp8266.localIP();
  Serial.print(F("Connected as "));
  Serial.println(ipAddr);

  // Do a query to get the date and the age of the moon.

  if (!query(&dateTimeUTC, &daysSinceNewMoon, &illuminatedPC)) {
    Serial.println(F("Query Failed."));
    return;
  }

  ret = esp8266.disconnect();
  if (ret < 0) {
    Serial.println(F("WiFi disconnect failed."));
  }
  
  
}

void loop() {
  delay(1000);
}

/*
 * Query the moon phase web site,
 * setting the UTC date and the age of the moon.
 * 
 * This function is designed to read
 *   HM Nautical Almanac Office: Miscellanea.
 *   Daily Rise/set and Twilight times for the British Isles.
 */
boolean query(ESP8266HttpRead::HttpDateTime *pDateTimeUTC, double *pDaysSinceNewMoon,
  int *pIlluminatedPC) {
  int wifiRet;

  Serial.print(F("Querying "));
  Serial.print(HttpServer);
  Serial.println(F(" ..."));
  
  wifiRet = client.connect(HttpServer, HttpPort);
  if (wifiRet < 0) {
    Serial.println(F("WiFi connect failed."));
    return false;
  }

  client.print(HttpRequest);

  ESP8266HttpRead reader;

  reader.begin(client, 15000L);

  if (!reader.findDate(pDateTimeUTC)) {
    Serial.println(F("No Date: in header"));
    client.stop();
    return false;
  }
  Serial.println(F("Found Date: "));
  Serial.print(F("days since Sunday: "));
  Serial.println(pDateTimeUTC->daySinceSunday);
  Serial.print(F("day of month: "));
  Serial.println(pDateTimeUTC->day);
  Serial.print(F("Month: "));
  Serial.println(pDateTimeUTC->month);
  Serial.print(F("Year: "));
  Serial.println(pDateTimeUTC->year);
  Serial.print(F("Hour: "));
  Serial.println(pDateTimeUTC->hour);
  Serial.print(F("Minute: "));
  Serial.println(pDateTimeUTC->minute);
  Serial.print(F("Second: "));
  Serial.println(pDateTimeUTC->second);

  if (!reader.find("age of the Moon is ")) {
    Serial.println(F("No age of moon in response."));
    client.stop();
    return false;
  }
  *pDaysSinceNewMoon = reader.readDouble();
  if (*pDaysSinceNewMoon == DBL_MAX) {
    client.stop();
    return false;
  }
  Serial.print(F("Days since new moon: "));
  Serial.println(*pDaysSinceNewMoon);

  if (!reader.find("fraction is ")) {
    Serial.println(F("No illumination fraction in response."));
    client.stop();
    return false;
  }
  double f = reader.readDouble();
  if (f == DBL_MAX) {
    client.stop();
    return false;
  }
  *pIlluminatedPC = (int) (f + 0.5);  // round the result.

  Serial.print(F("Percent illuminated: "));
  Serial.println(*pIlluminatedPC);

  
  int b;
  while ((b = reader.read()) >= 0) {
    // Serial.write((char) b);
  }
  client.stop();

  Serial.println(F("Query completed."));
  
  return true;
}

//************************************************************************
// From http://www.avr-developers.com/mm/memoryusage.html
//*	http://www.nongnu.org/avr-libc/user-manual/malloc.html
//*	thanks to John O.
void	Ram_TableDisplay(void) 
{
  char stack = 1;
extern char *__data_start;
extern char *__data_end;
extern char *__bss_start;
extern char *__bss_end;
extern char *__heap_start;
extern char *__heap_end;

  int	data_size	=	(int)&__data_end - (int)&__data_start;
  int	bss_size	=	(int)&__bss_end - (int)&__data_end;
  int	heap_end	=	(int)&stack - (int)&__malloc_margin;
  int	heap_size	=	heap_end - (int)&__bss_end;
  int	stack_size	=	RAMEND - (int)&stack + 1;
  int	available	=	(RAMEND - (int)&__data_start + 1);	
  available	-=	data_size + bss_size + heap_size + stack_size;

  Serial.println();
  Serial.print(F("data size     = "));
  Serial.println(data_size);
  Serial.print(F("bss_size      = "));
  Serial.println(bss_size);
  Serial.print(F("heap size     = "));
  Serial.println(heap_size);
  Serial.print(F("stack used    = "));
  Serial.println(stack_size);
  Serial.print(F("stack available     = "));
  Serial.println(available);
  Serial.print(F("Free memory   = "));
  Serial.println(get_free_memory());
  Serial.println();

}
int get_free_memory()
{
extern char __bss_end;
extern char *__brkval;

  int free_memory;

  if((int)__brkval == 0)
    free_memory = ((int)&free_memory) - ((int)&__bss_end);
  else
    free_memory = ((int)&free_memory) - ((int)__brkval);

  return free_memory;
}

