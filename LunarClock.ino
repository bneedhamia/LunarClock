/*
 * Net-connected lunar clock.
 * Rotates a dial showing the current phase of the moon,
 * as reported by HM Nautical Almanac Office: Miscellanea,
 * Daily Rise/set and Twilight times for the British Isles,
 * at http://astro.ukho.gov.uk/nao/miscellanea/birs2.html
 *
 * Note: that web page is Crown Copyright.
 * Read the site Copyright and Licensing page before using it.
 *
 * Copyright (c) 2015 Bradford Needham
 * { @bneedhamia , https://www.needhamia.com }
 *
 * Licensed under GPL V2
 * a copy of which should have been supplied with this file.
 */

/*
 * This sketch requires an Arduino Mega 2560,
 * a Sparkfun Transmogrishield on top of that
 * (https://www.sparkfun.com/products/11469),
 * a Sparkfun CC3000 WiFi Shield on top of that
 * (https://www.sparkfun.com/products/12071),
 * a 28BYJ-48 12V unipolar stepper motor
 * (Datasheet at http://www.emartee.com/product/41757/)
 * controlled by a set of discrete parts (TIP120s),
 * an opto-interrupter for aligning the image of the moon.
 * XXX and more parts.
 *
 * The Transmogrishield is needed because the SPI pins
 * are different on the Arduino Mega than the Uno.
 */

#include <stdlib.h>
#include <float.h>                // For DBL_MAX
#include <SPI.h>
#include <SFE_CC3000.h>
#include <SFE_CC3000_Client.h>
#include <EEPROM.h>
#include <Stepper.h> //XXX ditch the stepper library, because it burns power while not turning. Use your own code.

/*
 * Pins:
 *
 * Wifi CC3000 Shield Pins:
 * PIN_WIFI_INT = interrupt pin for Wifi Shield
 * PIN_WIFI_ENABLE = enable pin for with Wifi Shield
 * PIN_SELECT_WIFI = the CC3000 chip select pin.
 * PIN_SELECT_SD = the SD chip select pin (currently unused)
 *
 * Pins controlling the stepper motor.
 * These pins control the Base voltages of each
 * of the 4 non-common wires of the stepper motor.
 * Active High.  That is, a HIGH value grounds the
 * corresponding wire and coil, energizing that coil.
 * PIN_STEP_ORANGE
 * PIN_STEP_YELLOW
 * PIN_STEP_PINK
 * PIN_STEP_BLUE
 * 
 * PIN_LED_YELLOW = HIGH to light the yellow LED.
 *
 * PIN_OPTO_LIGHT = signal from the opto-interruptor.  HIGH = slot is unobstructed.
 *   That is, part of the lunar wheel slot is in front of the opto-interrupter.
 *   
 * XXX more to come.
 *
 * Pins 10-13 are the Mega SPI bus.
 * Note: that means that the normal pin 13 LED is not usable as an LED.
 */
const int PIN_WIFI_INT = 2;
const int PIN_WIFI_ENABLE = 7;
const int PIN_SELECT_WIFI = 10;
const int PIN_SELECT_SD = 8;

const int PIN_STEP_ORANGE = 28;
const int PIN_STEP_PINK = 26;
const int PIN_STEP_YELLOW = 24;
const int PIN_STEP_BLUE = 22;

const int PIN_LED_YELLOW = 30;

const int PIN_OPTO_LIGHT = 32;

/*
 * The EEPROM layout, starting at START_ADDRESS, is:
 * WiFi SSID = null-terminated string 0
 * WiFi Password = null-terminated string 1
 * EEPROM_END_MARK
 *
 * To write these values, use the Sketch write_eeprom_strings.
 * See https://github.com/bneedhamia/write_eeprom_strings
 */
const int START_ADDRESS = 0;      // First address of EEPROM to write to.
const byte EEPROM_END_MARK = 255; // marks the end of the data we wrote to EEPROM

char *wifiSsid;     // SSID of the network to connect to. Read from EEPROM.
char *wifiPassword; // password of the network. Read from EEPROM.
//XXX should put the security type in the EEPROM as well.
unsigned int wifiSecurity = WLAN_SEC_WPA2; // security type of the network.
unsigned int wifiTimeoutMs = 20 * 1000;  // connection timeout.

/*
 * The Date and Time returned from parseDate().
 * I would have used the C++ struct tm, but that didn't seem to be available in the Arduino library.
 * NOTE: some fields' values differ from the corresponding fields in struct tm.
 */
struct HttpDateTime {
  short daySinceSunday; // 0..6 Sunday = 0; Monday = 1; Saturday = 6
  short year;           // 1900..2100
  short month;          // 1..12 January = 1
  short day;            // 1..31  Day of the month
  short hour;           // 0..23  Midnight = 0; Noon = 12
  short minute;         // 0..59
  short second;         // 0..61 (usually 0..59)
};
boolean findDate(struct HttpDateTime *pDateTimeUTC);
double readDouble();

/*
 * Speed (Revolutions per minute) to run the stepper motor.
 * The stepper documentation 
 * 
 * The datasheet above for the 28BYJ-48 12V motor lists a step angle of 5.625 / 64 degrees.
 * or 360 / 5.625 * 64 = 4096.
 *
 * Putting this into the stepper library's RPM measure:
 * mS/minute   * revolutions/step     * steps/mS = revolutions/minute =
 * mS/minute   / steps per revolution / mS per step =
 * (1000 * 60) / 2046                 / 4 = 7.33 rpm (round down to 7 rpm) XXX old numbers.
 *
 */
const int STEPS_PER_REVOLUTION = 4096;
const int STEPPER_RPM = 5; // the Adafruit docs suggest 5rpm.

/*
 * Because there may be noise as the edge of the slot appears
 * in front of the opto-interrupter, we start turning the stepper motor,
 * and we expect to see the opto-interrupter dark for
 * at least MIN_DARK_STEPS contiguous steps (to get past the end of the slot)
 * before we start looking for the slot.
 * 
 * STEPS_SLOT_TO_MOON = the number of steps between the detected start of the slot
 * and the proper alignment of a lunar image in the clock's window.
 * That is, how much to move after finding the slot.
 * 
 * NUM_MOON_IMAGES = the number of lunar images in the wheel.
 * We assume the images evenly divide one mean synodic month (new moon to new moon),
 * rather than, for example, some image being 4 days long while another is 6 days long.
 * 
 * DAYS_PER_IMAGE = the number of days corresponding to each lunar image.
 *   29.53059 is the length in days of the mean synodic month
 * STEPS_PER_IMAGE = the number of steps to move from one image to the next.
 *   Note: we keep current angle and steps per image in floating point
 *   because NUM_MOON_IMAGES likely doesn't evenly divide STEPS_PER_REVOLUTION,
 *   which would result in the image alignment drifting significantly over a few months.
 *   
 * INITIAL_IMAGE_ANGLE_STEPS = the number of steps from the center of the new moon image
 *   to the initial image.  That is, the initial angle of the wheel relative to the new moon.
 */
const int MIN_DARK_STEPS = STEPS_PER_REVOLUTION / (360 / 5); // 5 degrees
const int STEPS_SLOT_TO_MOON = 114; //XXX need to calibrate this when the clock is finished.
const int NUM_MOON_IMAGES = 8;
const double DAYS_PER_IMAGE = 29.53059 / (double) NUM_MOON_IMAGES;
const double STEPS_PER_IMAGE = ((double) STEPS_PER_REVOLUTION) / NUM_MOON_IMAGES;
const double INITIAL_IMAGE_ANGLE_STEPS = STEPS_PER_IMAGE * 6;  //XXX need to change this when the mech. design is done.

/*
 * Stepper motor pin sequence.XXX update these notes for the right motor.
 *
 * The datasheet shows half-stepping, which the Arduino Stepper library doesn't do.
 *
 * I tested the datasheet's sequence by trying the 6 permutations
 * of pin sequences that begin with the Blue pin.
 */
Stepper stepper(STEPS_PER_REVOLUTION,
    //PIN_STEP_BLUE, PIN_STEP_YELLOW, PIN_STEP_PINK, PIN_STEP_ORANGE);    // weak clockwise
    //PIN_STEP_BLUE, PIN_STEP_YELLOW, PIN_STEP_ORANGE, PIN_STEP_PINK);    // weak clockwise
    PIN_STEP_BLUE, PIN_STEP_PINK, PIN_STEP_YELLOW, PIN_STEP_ORANGE);  // strong clockwise
    //PIN_STEP_BLUE, PIN_STEP_PINK, PIN_STEP_ORANGE, PIN_STEP_YELLOW);  // strong counterclockwise
    //PIN_STEP_BLUE, PIN_STEP_ORANGE, PIN_STEP_YELLOW, PIN_STEP_PINK);  // weak CCW
    //PIN_STEP_BLUE, PIN_STEP_ORANGE, PIN_STEP_PINK, PIN_STEP_YELLOW);  // weaak CCW

// WiFi and WiFi Client control objects.
SFE_CC3000 wifi = SFE_CC3000(PIN_WIFI_INT, PIN_WIFI_ENABLE, PIN_SELECT_WIFI);
SFE_CC3000_Client client = SFE_CC3000_Client(wifi);

/*
 * The date is received from the Http "Date:" header in the web response we receive.
 * Note: that date is GMT rather than local time, but we don't care
 * because we use the date/time only to decide when to read the web site again.
 */
struct HttpDateTime dateTimeUTC;

double daysSinceNewMoon;  // number of days (0.0 .. 29.53) since the New Moon.
int illuminatedPC;       // percent (0..100) of the moon's surface that's illuminated.

/*
 * curentAngleSteps = the current position of the wheel, in (fractional) steps
 * from the center of the new moon.
 * 
 * A floating-point number to avoid accumulating errors in dividing a revolution by the number of images,
 * which would cause the images to drift out of place after a few months of constant running.
 */
double currentAngleSteps;

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
const char HttpServer[] = "astro.ukho.gov.uk";
const int HttpPort = 80;
const String HttpRequest = "GET /nao/miscellanea/birs2.html HTTP/1.0\n"
                           "Host: astro.ukho.gov.uk\n"
                           "Connection: close\n"
                           "\n";

/*
 * Our state machine, that keeps track of what to do next.
 * 
 * STATE_ERROR = encountered and unrecoverable error. Do nothing more.
 * STATE_FIND_SLOT = search for the slot that tells us the wheel position.
 * STATE_WEB_QUERY = query the web site to find the time and moon phase.
 * STATE_TURN_WHEEL = turn the wheel to show the correct moon phase.
 * STATE_WAITING = waiting for the time to query the site again.
 * STATE_DONE = used only for development. Says we're done.
 * 
 * state = the current state of the program.
 * 
 */
const byte STATE_ERROR        = 0;
const byte STATE_FIND_SLOT    = 1;
const byte STATE_WEB_QUERY    = 2;
const byte STATE_TURN_WHEEL   = 3;
const byte STATE_WAITING      = 4;
const byte STATE_DONE         = 5;
byte state;

boolean findWheelSlot();
boolean query(struct HttpDateTime *pDateTimeUTC, double *pDaysSinceNewMoon,
              int *pIlluminatedPC);
boolean turnWheelToPhase(double daysSinceNewMoon);

void setup() {
  Serial.begin(9600);

  pinMode(PIN_LED_YELLOW, OUTPUT);
  digitalWrite(PIN_LED_YELLOW, LOW);
  pinMode(PIN_OPTO_LIGHT, INPUT);

  Serial.println(F("Reset."));

  state = STATE_ERROR; 

  // read the wifi credentials from EEPROM, if they're there.
  wifiSsid = readEEPROMString(START_ADDRESS, 0);
  wifiPassword = readEEPROMString(START_ADDRESS, 1);
  if (wifiSsid == 0 || wifiPassword == 0) {
    Serial.println(F("EEPROM not initialized."));
    state = STATE_ERROR;
    return;
  }

  stepper.setSpeed(STEPPER_RPM);

  // Give the developer a chance to start the Serial Monitor
  // before setting up the WiFi
  delay(5000);

  Serial.println(F("Starting..."));

  // Setup the WiFi shield
  if (!wifi.init()) {
    Serial.println(F("wifi.init() failed."));
    state = STATE_ERROR;
    return;
  }

  // initialization is complete.
  state = STATE_FIND_SLOT;
}

void loop() {
  int i;

  switch (state) {
    
  case STATE_ERROR: // unrecoverable error.  Stop.
    // Blink an led
    if ((millis() % 1000) < 500) {
      digitalWrite(PIN_LED_YELLOW, HIGH);
    } else {
      digitalWrite(PIN_LED_YELLOW, LOW);
    }
    delay(10); // so we don't spend all our time doing digitalWrite()
    break;

  case STATE_FIND_SLOT:  // rotate the wheel to the slot, so we know the wheel position.
    if (!findWheelSlot()) {
      state = STATE_ERROR;
      break;
    }

    // The wheel is in its initial position.  Find the date and phase of the moon.
    state = STATE_WEB_QUERY;
    break;

  case STATE_WEB_QUERY:      // Find the date and the phase of the moon.
    if (!doNetworkWork()) {
      //XXX set how long to wait, say 1 minute.  Keep track of # of fails.
      state = STATE_WAITING;
      break;
    }

    // We now know the date and the phase of the moon.
    state = STATE_TURN_WHEEL;
    break;

  case STATE_TURN_WHEEL:  // turn the wheel to the current phase of the moon
    if (!turnWheelToPhase(daysSinceNewMoon)) {
      state = STATE_ERROR;
      break;
    }
    
    state = STATE_ERROR; //XXX for now.
    break;

  case STATE_WAITING:   // waiting until it's the right time to query again.
    //XXX this state is used to retry a failed query as well as to wait a day to query again.
    //XXX watch out for daylight saving time.  Just do an offset from midnight UTC,
    //XXX knowing that it will be an hour different in daylight time.
    //XXX be aware that millis() will overflow every ~49.7 days.
    //XXX 24 hours = (unsigned long) 1000 * 60 * 60 * 24 = 86,400,000 milliseconds.
    state = STATE_ERROR; //XXX for now.
    break;

  default:
    Serial.print(F("Unknown state, "));
    Serial.println(((int) state) & 0xFF);
    state = STATE_ERROR;
  }

}

/*
 * Turns the stepper motor until the slot appears in front of the opto-interrupter.
 * We need to do this on Reset to move the wheel to a known position.
 * 
 * In case we're already somewhere in the slot,
 * we wait for MIN_DARK_STEPS contiguous steps outside the slot
 * before we start looking for the beginning of the slot.
 */
boolean findWheelSlot() {
  boolean seenMinDark = false;
  int count = 0;
  int i;
    
  // Turn up to 1.5 revolutions to find the slot in the wheel.
  for (i = 0; i < STEPS_PER_REVOLUTION + (STEPS_PER_REVOLUTION / 2); ++i) {
    stepper.step(1);

    if (!seenMinDark) {
      if (digitalRead(PIN_OPTO_LIGHT) == HIGH) {
        continue; // light
      }
      ++count;
      if (count < MIN_DARK_STEPS) {
        continue;
      }

      // Seen MIN_DARK_STEPS - we're now clearly outside the slot.
      seenMinDark = true;
      count = 0;
    } else {
      // Searching for the slot.
      if (digitalRead(PIN_OPTO_LIGHT) == LOW) {
        continue; // still dark
      }
      ++count;
      break; // we're done.
    }
  }
  if (!seenMinDark || count == 0) {
    Serial.println(F("Slot not found."));
    return false;
  }

  /*
   * We are positioned at the slot.
   * Move forward to get to align a lunar image in the window.
   * Note which lunar image that is.
   */

  stepper.step(STEPS_SLOT_TO_MOON);
  currentAngleSteps = INITIAL_IMAGE_ANGLE_STEPS;

  return true;
}

/*
 * Turns the lunar images wheel to show the phase of the moon
 * corresponding to the given age of the moon.
 * 
 * Returns true if successful; false otherwise.
 */
boolean turnWheelToPhase(double daysSinceNewMoon) {
  int desiredIndex = 0;  // index of the lunar image we want to display; new moon = 0.
  double desiredAngleSteps = 0.0; // desired position of the wheel, in fractional steps from the new moon.
  int stepsToMove = 0;   // number of steps required to turn to the desired angle.

  // Find which image we want to move to.
  desiredIndex = (int) ((daysSinceNewMoon + (DAYS_PER_IMAGE / 2)) / DAYS_PER_IMAGE);
  if (desiredIndex < 0) {
    Serial.print("Calculated Index < 0: ");
    Serial.println(desiredIndex);
    return false;
  }
  if (desiredIndex > NUM_MOON_IMAGES - 1) {  // Needed because floating point numbers aren't exact.
    desiredIndex = NUM_MOON_IMAGES - 1;
  }
  
  Serial.print(F("Moving to image "));
  Serial.println(desiredIndex);

  desiredAngleSteps = desiredIndex * STEPS_PER_IMAGE;
  stepsToMove = (int) (desiredAngleSteps - currentAngleSteps + 0.5);
  while (stepsToMove < 0) {
    stepsToMove += STEPS_PER_REVOLUTION;
  }

  stepper.step(stepsToMove);

  // Update the position of the wheel relative to the new moon.
  currentAngleSteps += desiredAngleSteps;

  return true;
}

/*
 * Performs one run of our network activity:
 * Connects to the WiFi access point,
 * performs the lunar phase query,
 * then disconnects.
 *
 * Returns true if successful; false otherwise.
 */
boolean doNetworkWork() {
  Serial.print(F("Connecting to "));
  Serial.print(wifiSsid);
  Serial.println(F(" ..."));

  if (!wifi.connect(wifiSsid, wifiSecurity, wifiPassword, wifiTimeoutMs)) {
    Serial.print(F("Failed to connect to "));
    Serial.println(wifiSsid);
    return false;
  }
  Serial.println("Connected.");

  // Do a query to get the date and the age of the moon.

  if (!query(&dateTimeUTC, &daysSinceNewMoon, &illuminatedPC)) {
    Serial.println(F("Query Failed."));
    client.close();
    wifi.disconnect();
    return false;
  }

  client.close();
  wifi.disconnect();
  return true;
}

/*
 * Query the moon phase web site,
 * setting the UTC date and the age of the moon.
 *
 * This function is designed to read
 *   HM Nautical Almanac Office: Miscellanea.
 *   Daily Rise/set and Twilight times for the British Isles.
 */
boolean query(struct HttpDateTime *pDateTimeUTC, double *pDaysSinceNewMoon,
              int *pIlluminatedPC) {

  Serial.print(F("Querying "));
  Serial.print(HttpServer);
  Serial.println(F(" ..."));

  if (!client.connect(HttpServer, HttpPort)) {
    Serial.println(F("Connect to server failed."));
    return false;
  }
  client.print(HttpRequest);

  if (!findDate(pDateTimeUTC)) {
    Serial.println(F("No Date: in header"));
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

  if (!client.find("age of the Moon is ")) {
    Serial.println(F("No age of moon in response."));
    return false;
  }
  *pDaysSinceNewMoon = readDouble();
  if (*pDaysSinceNewMoon == DBL_MAX) {
    return false;
  }

  Serial.print(F("Days since new moon: "));
  Serial.println(*pDaysSinceNewMoon);

  if (!client.find("fraction is ")) {
    Serial.println(F("No illumination fraction in response."));
    return false;
  }
  double f = readDouble();
  if (f == DBL_MAX) {
    return false;
  }
  *pIlluminatedPC = (int) (f + 0.5);  // round the result.

  Serial.print(F("Percent illuminated: "));
  Serial.println(*pIlluminatedPC);

  Serial.println(F("Query completed."));

  return true;
}

/*
 * Skips to the "Date:" Http header
 * then parses the date header, through the timezone.
 * The Timezone must be GMT
 * Return true if successful, false otherwise.
 *
 * Example date header returned in the HTTP response from a web server:
 * Date: Fri, 21 Aug 2015 22:06:40 GMT
 *
 * To use:
 *   Struct HttpDateTime dateTime;
 *   ...
 *   findDate(&dateTime);
 *   Serial.print(dateTime.year);
 */
boolean findDate(struct HttpDateTime *pDateTimeUTC) {
  uint8_t buf[4];

  pDateTimeUTC->daySinceSunday = -1;
  pDateTimeUTC->year = -1;
  pDateTimeUTC->month = -1;
  pDateTimeUTC->day = -1;
  pDateTimeUTC->hour = -1;
  pDateTimeUTC->minute = -1;
  pDateTimeUTC->second = -1;

  if (!client.find("Date: ")) {
    // No Date header found in response.
    return false;
  }

  // Day of week: Sun Mon Tue Wed Thu Fri Sat
  if (!client.read(buf, 3)) {
    return false;
  }
  if (buf[0] == 'S') {          // Sun or Sat
    if (buf[1] == 'u') {        // Sun
      pDateTimeUTC->daySinceSunday = 0;
    } else if (buf[1] == 'a') { // Sat
      pDateTimeUTC->daySinceSunday = 6;
    } else { // garbled day of week.
      return false;
    }
  } else if (buf[0] == 'M') {   // Mon
    pDateTimeUTC->daySinceSunday = 1;
  } else if (buf[0] == 'T') {   // Tue or Thu
    if (buf[1] == 'u') {        // Tue
      pDateTimeUTC->daySinceSunday = 2;
    } else if (buf[1] == 'h') { // Thu
      pDateTimeUTC->daySinceSunday = 4;
    } else {  // garbled day of week.
      return false;
    }
  } else if (buf[0] == 'W') {   // Wed
    pDateTimeUTC->daySinceSunday = 3;
  } else if (buf[0] == 'F') {   // Fri
    pDateTimeUTC->daySinceSunday = 5;
  } else { // garbled day of week.
    return false;
  }

  // Skip the ", " after the day of the week.
  if (!client.read(buf, 2)) {
    return false;
  }

  // Day of the month: 1..31
  if (!client.read(buf, 2)) {
    return false;
  }
  if (!('0' <= buf[0] && buf[0] <= '9')) {
    return false;  // garbled
  }
  if (!('0' <= buf[1] && buf[1] <= '9')) {
    return false;  // garbled
  }
  pDateTimeUTC->day = (buf[0] - '0') * 10 + (buf[1] - '0');

  // Skip the space before the month.
  if (client.read() < 0) {
    return false;
  }

  // Month: Jan Feb Mar Apr May Jun Jul Aug Sep Oct Nov Dec
  if (!client.read(buf, 3)) {
    return false; // garbled.
  }
  if (buf[0] == 'J') {          // Jan, Jun, or Jul
    if (buf[1] == 'a') {        // Jan
      pDateTimeUTC->month = 1;
    } else if (buf[2] == 'n') { // Jun
      pDateTimeUTC->month = 6;
    } else if (buf[2] == 'l') { // Jul
      pDateTimeUTC->month = 7;
    } else {
      return false; // garbled
    }
  } else if (buf[0] == 'F') {   // Feb
    pDateTimeUTC->month = 2;
  } else if (buf[0] == 'M') {   // Mar or May
    if (buf[2] == 'r') {        // Mar
      pDateTimeUTC->month = 3;
    } else if (buf[2] == 'y') { // May
      pDateTimeUTC->month = 5;
    } else {
      return false; // garbled
    }
  } else if (buf[0] == 'A') {   // Apr or Aug
    if (buf[1] == 'p') {        // Apr
      pDateTimeUTC->month = 4;
    } else if (buf[1] == 'u') { // Aug
      pDateTimeUTC->month = 8;
    } else {
      return false;
    }
  } else if (buf[0] == 'S') {   // Sep
    pDateTimeUTC->month = 9;
  } else if (buf[0] == 'O') {   // Oct
    pDateTimeUTC->month = 10;
  } else if (buf[0] == 'N') {   // Nov
    pDateTimeUTC->month = 11;
  } else if (buf[0] == 'D') {   // Dec
    pDateTimeUTC->month = 12;
  } else {
    return false; // garbled
  }

  // Skip the space before the year
  if (client.read() < 0) {
    return false;
  }

  // Year: 1900..2100 or so.
  if (!client.read(buf, 4)) {
    return false;
  }
  if (!('0' <= buf[0] && buf[0] <= '9')) {
    return false;  // garbled
  }
  if (!('0' <= buf[1] && buf[1] <= '9')) {
    return false;  // garbled
  }
  if (!('0' <= buf[2] && buf[2] <= '9')) {
    return false;  // garbled
  }
  if (!('0' <= buf[3] && buf[3] <= '9')) {
    return false;  // garbled
  }
  pDateTimeUTC->year = (buf[0] - '0') * 1000
                       + (buf[1] - '0') * 100
                       + (buf[2] - '0') * 10
                       + (buf[3] - '0');

  // Skip the space before the hour
  if (client.read() < 0) {
    return false;
  }

  // Hour: 00..23
  if (!client.read(buf, 2)) {
    return false;
  }
  if (!('0' <= buf[0] && buf[0] <= '9')) {
    return false;  // garbled
  }
  if (!('0' <= buf[1] && buf[1] <= '9')) {
    return false;  // garbled
  }
  pDateTimeUTC->hour = (buf[0] - '0') * 10 + (buf[1] - '0');

  // Skip the : before the minute
  if (client.read() < 0) {
    return false;
  }

  // Minute: 00..59
  if (!client.read(buf, 2)) {
    return false;
  }
  if (!('0' <= buf[0] && buf[0] <= '9')) {
    return false;  // garbled
  }
  if (!('0' <= buf[1] && buf[1] <= '9')) {
    return false;  // garbled
  }
  pDateTimeUTC->minute = (buf[0] - '0') * 10 + (buf[1] - '0');

  // Skip the : before the second
  if (client.read() < 0) {
    return false;
  }

  // Second: 00..61 (usually 00..59)
  if (!client.read(buf, 2)) {
    return false;
  }
  if (!('0' <= buf[0] && buf[0] <= '9')) {
    return false;  // garbled
  }
  if (!('0' <= buf[1] && buf[1] <= '9')) {
    return false;  // garbled
  }
  pDateTimeUTC->second = (buf[0] - '0') * 10 + (buf[1] - '0');

  // Skip the space before the Timezone
  if (client.read() < 0) {
    return false;
  }

  // Timezone: GMT hopefully.
  if (!client.read(buf, 3)) {
    return false;
  }
  if (buf[0] != 'G' || buf[1] != 'M' || buf[2] != 'T') {
    // buf[3] = '\0';  // so we can print it.
    // Serial.print(F("TZ not GMT: "));
    // Serial.println(buf);
    return false;    // Timezone not GMT
  }

  return true;
}

/*
 * Read a double-floating-point value from the input,
 * and the character just past that double.
 * For example "11.9X" would return 11.9 and would read
 * the X character following the string "11.9"
 * Note: there must be at least one character following the number.
 * That is, the input mustn't end immediately after the number.
 *
 * Accepts unsigned decimal numbers such as
 * 34
 * 15.
 * 90.54
 * .2
 *
 * Returns either the decimal number, or DBL_MAX (see <float.h>) if an error occurs.
 */
double readDouble() {
  int ch;

  double result = 0.0;

  // Read the integer part of the number (if there is one)

  boolean sawInteger = false;
  ch = client.read();
  while ('0' <= (char) ch && (char) ch <= '9') {
    sawInteger = true;
    result *= 10.0;
    result += (char) ch - '0';

    ch = client.read();
  }
  if (ch < 0) {
    return DBL_MAX;    // early end of file or error.
  }
  if (ch != '.') {
    if (!sawInteger) {
      return DBL_MAX;   // no number was found at all.
    }
    return result;
  }

  // read the fractional part of the number (if there is one)

  double scale = 0.1;
  ch = client.read();
  while ('0' <= (char) ch && (char) ch <= '9') {
    sawInteger = true;
    result += scale * ((char) ch - '0');
    scale /= 10.0;

    ch = client.read();
  }
  if (ch < 0) {
    return DBL_MAX;
  }
  if (!sawInteger) {
    return DBL_MAX;
  }

  return result;
}

/********************************
 * From https://github.com/bneedhamia/write_eeprom_strings example
 */
/*
 * Reads a string from EEPROM.  Copy this code into your program that reads EEPROM.
 *
 * baseAddress = EEPROM address of the first byte in EEPROM to read from.
 * stringNumber = index of the string to retrieve (string 0, string 1, etc.)
 *
 * Assumes EEPROM contains a list of null-terminated strings,
 * terminated by EEPROM_END_MARK.
 *
 * Returns:
 * A pointer to a dynamically-allocated string read from EEPROM,
 * or null if no such string was found.
 */
char *readEEPROMString(int baseAddress, int stringNumber) {
  int start;   // EEPROM address of the first byte of the string to return.
  int length;  // length (bytes) of the string to return, less the terminating null.
  char ch;
  int nextAddress;  // next address to read from EEPROM.
  char *result;     // points to the dynamically-allocated result to return.
  int i;

  nextAddress = START_ADDRESS;
  for (i = 0; i < stringNumber; ++i) {

    // If the first byte is an end mark, we've run out of strings too early.
    ch = (char) EEPROM.read(nextAddress++);
    if (ch == (char) EEPROM_END_MARK || nextAddress >= EEPROM.length()) {
      return (char *) 0;  // not enough strings are in EEPROM.
    }

    // Read through the string's terminating null (0).
    while (ch != '\0' && nextAddress < EEPROM.length()) {
      ch = EEPROM.read(nextAddress++);
    }
  }

  // We're now at the start of what should be our string.
  start = nextAddress;

  // If the first byte is an end mark, we've run out of strings too early.
  ch = (char) EEPROM.read(nextAddress++);
  if (ch == (char) EEPROM_END_MARK) {
    return (char *) 0;  // not enough strings are in EEPROM.
  }

  // Count to the end of this string.
  length = 0;
  while (ch != '\0' && nextAddress < EEPROM.length()) {
    ++length;
    ch = EEPROM.read(nextAddress++);
  }

  // Allocate space for the string, then copy it.
  result = new char[length + 1];
  nextAddress = start;
  for (i = 0; i < length; ++i) {
    result[i] = (char) EEPROM.read(nextAddress++);
  }
  result[i] = '\0';

  return result;

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

  if ((int)__brkval == 0)
    free_memory = ((int)&free_memory) - ((int)&__bss_end);
  else
    free_memory = ((int)&free_memory) - ((int)__brkval);

  return free_memory;
}


