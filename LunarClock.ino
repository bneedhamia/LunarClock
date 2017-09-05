/*
   Net-connected lunar clock.
   Rotates a dial showing the current phase of the moon,
   as reported by HM Nautical Almanac Office: Miscellanea,
   Daily Rise/set and Twilight times for the British Isles,
   at http://astro.ukho.gov.uk/nao/miscellanea/birs2.html

   Note: that web page is Crown Copyright.
   Read the site Copyright and Licensing page before using it.

   Copyright (c) 2015, 2017 Bradford Needham
   { @bneedhamia , https://www.needhamia.com }

   Licensed under GPL V2
   a copy of which should have been supplied with this file.
*/
//345678901234567890123456789312345678941234567895123456789612345678971234567898

/*
   This sketch requires a Sparkfun ESP8266 Thing Dev board
   (https://www.sparkfun.com/products/13804),
   a 28BYJ-48 12V unipolar stepper motor
   (Datasheet at http://www.emartee.com/product/41757/)
   controlled by a set of discrete parts (TIP120s),
   a photo-interrupter for aligning the image of the moon
   (https://www.sparkfun.com/products/9299).
   XXX and more parts.

   See the Sparkfun ESP8266 Thing Dev board page above
   for instructions on installing the Arduino ESP8266 support.

   See BillOfMaterials.ods for all the parts.

   ESP8266 Note: for the WiFi to function properly,
   there must never be a time longer than say 1 second where
   either delay() or loop() is not called.
   That implies that no delay() call can be longer than say 1 second.
*/

#include <float.h>       // For DBL_MAX
#include <ESP8266WiFi.h> // Defines WiFi, the WiFi controller object
#include <ESP8266HTTPClient.h>
#include <EEPROM.h>      // NOTE: ESP8266 EEPROM library differs from Arduino's.

/*
   Pins:

   Pins controlling the stepper motor.
   These pins control the Base voltages of each
   of the 4 non-common wires of the stepper motor.
   The 5th pin (Red) is connected to Vin.

   These pins are Active High.  That is, a HIGH value grounds the
   corresponding wire and coil, energizing that coil.
   PIN_STEP_ORANGE
   PIN_STEP_YELLOW
   PIN_STEP_PINK
   PIN_STEP_BLUE

   PIN_LED_L = LOW to light the yellow LED (and the on-board LED).
     The _L suffix indicates that the pin is Active-Low.
     Active-Low because that's how the ESP8266 Thing Dev board
     on-board LED is wired.

   PIN_LIGHT_DETECTED = input from the photo-interruptor.
     HIGH = the detector is unobstructed (the detector's light is detected).
     That is, part of the lunar wheel's slot is in front
     of the photo-interrupter.
*/

const int PIN_STEP_ORANGE = 12;
const int PIN_STEP_PINK = 13;
const int PIN_STEP_YELLOW = 4;
const int PIN_STEP_BLUE = 0;

const int PIN_LED_L = 5;

const int PIN_LIGHT_DETECTED = 15;

/*
   The EEPROM layout, starting at START_ADDRESS, is:
   WiFi SSID = null-terminated string 0
   WiFi Password = null-terminated string 1
   EEPROM_END_MARK

   To write these values, use the Sketch write_eeprom_strings.
   See https://github.com/bneedhamia/write_eeprom_strings
*/
const int START_ADDRESS = 0;      // The first EEPROM address to read from.
const byte EEPROM_END_MARK = 255; // marks the end of the data we wrote to EEPROM
const int EEPROM_MAX_STRING_LENGTH = 120; // max string length in EEPROM

/*
   States of our state machine that keeps track of what to do inside loop().

   STATE_ERROR = encountered an unrecoverable error. Do nothing more.
   STATE_FIND_SLOT = search for the slot that tells us the wheel position.
   STATE_WEB_QUERY = query the web site to find the time and moon phase.
   STATE_TURN_WHEEL = turn the wheel to show the correct moon phase.
   STATE_WAITING = waiting for the time to query the site again.
   STATE_DONE = used only for development. Says we're done.
*/
const byte STATE_ERROR        = 0;
const byte STATE_FIND_SLOT    = 1;
const byte STATE_WEB_QUERY    = 2;
const byte STATE_TURN_WHEEL   = 3;
const byte STATE_WAITING      = 4;
const byte STATE_DONE         = 5;

/*
   STEPS_PER_REVOLUTION = the number of (integer) steps in one revolution
   of our stepper motor.
   The Adafruit datasheet for the 28BYJ-48 12V motor  at http://www.adafruit.com/products/918
   lists 32 steps per revolution with a further gear ration of 16.025
   which gives 32 * 16.025 = 512.8 steps per revolution.
   Rounding to 513 produces an error of +0.2 steps per revolution
   (that is, one of our revolutions is 0.2 steps larger than the real one).
*/
const int STEPS_PER_REVOLUTION = 513;

/*
   Because there may be noise (uncertainty) as the edge of the slot appears
   in front of the photo-interrupter, we start turning the stepper motor,
   then we expect to see the photo-interrupter dark for
   at least MIN_DARK_STEPS contiguous steps (to get past the end of the slot)
   before we start looking for the slot.

   STEPS_SLOT_TO_MOON = the number of steps between
   the detected start of the slot and the proper alignment of a lunar image
   in the clock's window.
   That is, how much to move after finding the slot.
*/
const int MIN_DARK_STEPS = STEPS_PER_REVOLUTION / (360 / 5); // 5 degrees
const int STEPS_SLOT_TO_MOON = 33; //XXX need to calibrate this when the clock is finished.

/*
   NUM_MOON_IMAGES = the number of lunar images in the wheel.
     We assume the images evenly divide one mean synodic month
     (new moon to new moon), rather than, for example,
     some image being 4 days long while another is 6 days long.
   DAYS_PER_IMAGE = the number of days corresponding to each lunar image.
     29.53059 is the length in days of the mean synodic month.
   STEPS_PER_IMAGE = the number of steps to move from one image to the next.
     Note: we keep current angle and steps per image in floating point
     because NUM_MOON_IMAGES likely doesn't evenly divide STEPS_PER_REVOLUTION,
     which would result in the image alignment drifting significantly
     over a few months.

   INITIAL_IMAGE_ANGLE_STEPS = the number of steps from the center of
     the new moon image to the initial image.
     That is, the initial angle of the wheel relative to the new moon.
*/
const int NUM_MOON_IMAGES = 8;
const double DAYS_PER_IMAGE = 29.53059 / (double) NUM_MOON_IMAGES;
const double STEPS_PER_IMAGE = ((double) STEPS_PER_REVOLUTION) / NUM_MOON_IMAGES;
const double INITIAL_IMAGE_ANGLE_STEPS = STEPS_PER_IMAGE * 7;  //XXX need to change this when the mech. design is done.

/*
   Width (milliseconds) of each (half) step in the stepper motor sequence.
   Experimentation shows 4ms is good; 10 is strong & slow; 2 is fast & weak.

   Calculating RPM from PULSE_WIDTH_MS:
   mS/minute   * revolutions/step     * steps/mS = revolutions/minute =
   mS/minute   / steps per revolution / (2 * PULSE_WIDTH_MS) =
   (1000 * 60) / 512.8                / 20 = about 5.8 RPM
   Experimentally, one revolution took about 10 seconds, which would be 6 rpm.
   So this looks right.

   NOTE: During PULSE_WIDTH_MS, the stepper motor is drawing its full current,
   so we want to choose the smallest value of PULSE_WIDTH_MS that turns the disk.
*/
const int PULSE_WIDTH_MS = 10;

/*
   The Date and Time returned from parseDate().
   I would have used the C++ struct tm,
   but that didn't seem to be available in the Arduino library.
   NOTE: some fields' values differ from the corresponding fields in struct tm.
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

/*
   curentAngleSteps = the current position of the wheel, in (fractional) steps
   from the center of the new moon.

   A floating-point number to avoid accumulating errors
   in dividing a revolution by the number of images,
   which would cause the images to drift out of place
   after a few months of constant running.
*/
double currentAngleSteps;

/*
   28BYJ-48 12V Stepper motor sequence.

   SEQUENCE_STEPS = number of values in sequence[].
   sequence[] = the sequence of pin activation for clockwise rotation
     of the stepper motor.
   curSeq = the index into sequence[] corresponding to the current state
     of the motor. Note: on reset, we don't know the state of the motor,
     but it doesn't matter because we'll rotate the motor to a
     known location (the slot).

   The commented-out sequences are the other 5 possible sequences of the 4 pins.
*/
const int SEQUENCE_STEPS = 4;
const int sequence[SEQUENCE_STEPS] =
{PIN_STEP_BLUE, PIN_STEP_YELLOW, PIN_STEP_PINK, PIN_STEP_ORANGE};    // strong clockwise
//{PIN_STEP_BLUE, PIN_STEP_YELLOW, PIN_STEP_ORANGE, PIN_STEP_PINK};    // quiver (no movement)
//{PIN_STEP_BLUE, PIN_STEP_PINK, PIN_STEP_YELLOW, PIN_STEP_ORANGE};    // quiver
//{PIN_STEP_BLUE, PIN_STEP_PINK, PIN_STEP_ORANGE, PIN_STEP_YELLOW};    // quiver
//{PIN_STEP_BLUE, PIN_STEP_ORANGE, PIN_STEP_YELLOW, PIN_STEP_PINK};    // quiver
//{PIN_STEP_BLUE, PIN_STEP_ORANGE, PIN_STEP_PINK, PIN_STEP_YELLOW};    // strong counterclockwise
int curSeq;

// WiFi Client control object. SAY MORE ABOUT THIS.
HTTPClient httpGet;
WiFiClient *pHttpStream = 0; // stream of data from the Http Get

/*
   The date is received from the Http "Date:" header in the web response we receive.
   Note: that date is GMT rather than local time, but we don't care
   because we use the date/time only to decide when to read the web site again.
*/
struct HttpDateTime dateTimeUTC;

/*
 * Moon data from the web site:
 * 
 * daysSinceNewMoon = number of days (0.0 .. 29.53) since the New Moon.
 * illuminatedPC = percent (0..100) of the moon's surface that's illuminated (unused).
 */
double daysSinceNewMoon;
int illuminatedPC;

/*
   The site to query:
     HM Nautical Almanac Office: Miscellanea.
     Daily Rise/set and Twilight times for the British Isles.

   Notes:
   - HTTP 1.0 (rather than 1.1) is specified here to prevent
     the server from using Transfer-encoding: chunked,
     which is difficult to read.
   - "Connection: close" is used to cause the server to
     close the connection when the response is completed.
   - The page takes several seconds to load, because
     the site seems to pause in the middle, and it's a long page.
   - The current age of the moon and % illumination is given
     very near the end of the text of the web page.
*/
//const char HttpServer[] = "astro.ukho.gov.uk";
//const String HttpRequest = "GET /nao/miscellanea/birs2.html HTTP/1.0\n"
//                           "Host: astro.ukho.gov.uk\n"
//                           "Connection: close\n"
//                           "\n";
const char PageUrl[] = "http://astro.ukho.gov.uk/nao/miscellanea/birs2.html";


/*
 * The current state of the state machine that runs the loop().
 * See STATE_* above.
 */
byte state;

/*
   WiFi access point parameters.

   wifiSsid = SSID of the network to connect to. Read from EEPROM.
   wifiPassword = Password of the network. Read from EEPROM.
   wifiSecurity = security mode of the network.
   wifiTimeoutMs = timeout (in milliseconds) to wait before deciding
    the connection to the network has failed.
*/
char *wifiSsid;
char *wifiPassword;
//XXX should put the security type in the EEPROM as well.
//unsigned int wifiSecurity = WLAN_SEC_WPA2; // security type of the network.
unsigned int wifiTimeoutMs = 20 * 1000;  // connection timeout.

/*
 * Declarations of our functions. See their definitions (code)
 * for how they're used.
 */
boolean doNetworkWork();
boolean query(struct HttpDateTime *pDateTimeUTC, double *pDaysSinceNewMoon,
              int *pIlluminatedPC);
boolean findDate(struct HttpDateTime *pDateTimeUTC);
double readDouble();
boolean findWheelSlot();
boolean turnWheelToPhase(double daysSinceNewMoon);
void step(int steps);
char *readEEPROMString(int baseAddress, int stringNumber);
void  Ram_TableDisplay(void);
boolean findWheelSlot();
boolean query(struct HttpDateTime *pDateTimeUTC, double *pDaysSinceNewMoon,
              int *pIlluminatedPC);
boolean turnWheelToPhase(double daysSinceNewMoon);

// Called once automatically on Reset.
void setup() {
  Serial.begin(9600);

  // Set up all our pins.

  pinMode(PIN_LED_L, OUTPUT);
  digitalWrite(PIN_LED_L, HIGH); // ESP8266 Thing Dev LED is Active Low

  pinMode(PIN_LIGHT_DETECTED, INPUT);

  pinMode(PIN_STEP_ORANGE, OUTPUT);
  digitalWrite(PIN_STEP_ORANGE, LOW);
  pinMode(PIN_STEP_PINK, OUTPUT);
  digitalWrite(PIN_STEP_PINK, LOW);
  pinMode(PIN_STEP_YELLOW, OUTPUT);
  digitalWrite(PIN_STEP_YELLOW, LOW);
  pinMode(PIN_STEP_BLUE, OUTPUT);
  digitalWrite(PIN_STEP_BLUE, LOW);

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

  Serial.println(F("Starting..."));
  /*
     Initialization is complete.
     Next, we rotate the lunar wheel to a known starting place.
  */
  state = STATE_FIND_SLOT;
}

void loop() {
  int i;

  switch (state) {

    case STATE_ERROR: // unrecoverable error.  Stop.
      // Blink an led
      if ((millis() % 1000) < 500) {
        digitalWrite(PIN_LED_L, HIGH);
      } else {
        digitalWrite(PIN_LED_L, LOW);
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
   Turns the stepper motor until the edge of the slot appears in front of the
   photo-interrupter. We need to do this on Reset to move the wheel
   to a known position.

   In case we're already somewhere in the slot,
   we wait for MIN_DARK_STEPS contiguous steps outside the slot
   before we start looking for the beginning of the slot.
*/
boolean findWheelSlot() {
  boolean seenMinDark = false;
  int count = 0;
  int i;

  Serial.println(F("Finding the disk slot"));

  // Turn up to 1 and 1/4 revolutions to find the slot in the wheel.
  for (i = 0; i < STEPS_PER_REVOLUTION + (STEPS_PER_REVOLUTION / 4); ++i) {
    step(1);

    if (!seenMinDark) {
      if (digitalRead(PIN_LIGHT_DETECTED) == HIGH) {
        continue; // light. (still) in the slot.
      }

      // Dark. Possibly outside the slot.
      ++count;
      if (count < MIN_DARK_STEPS) {
        continue; // not yet safely outside the slot.
      }

      // Seen MIN_DARK_STEPS - we're now clearly outside the slot.
      seenMinDark = true;
      count = 0;
    } else {
      // Searching for the slot.
      if (digitalRead(PIN_LIGHT_DETECTED) == LOW) {
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
     We are positioned at the slot.
     Move forward to get to align a lunar image in the window.
     Note where the wheel is relative to the new moon.
  */

  step(STEPS_SLOT_TO_MOON);
  currentAngleSteps = INITIAL_IMAGE_ANGLE_STEPS;

  return true;
}

/*
   Turns the lunar images wheel to show the phase of the moon
   corresponding to the given age of the moon.

   Returns true if successful; false otherwise.
*/
boolean turnWheelToPhase(double daysSinceNewMoon) {
  int desiredIndex = 0;  // index of the lunar image we want to display; new moon = 0.
  double desiredAngleSteps = 0.0; // desired position of the wheel, in fractional steps from the new moon.
  int stepsToMove = 0;   // number of steps required to turn to the desired angle.

  // Find which image we want to move to (rounded to an integer).
  desiredIndex = (int) ((daysSinceNewMoon + (DAYS_PER_IMAGE / 2)) / DAYS_PER_IMAGE);
  if (desiredIndex < 0) {
    // Error.
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

  step(stepsToMove);

  // Update the position of the wheel relative to the new moon.
  currentAngleSteps += desiredAngleSteps;

  return true;
}

/*
   Performs one run of our network activity:
   Connects to the WiFi access point,
   performs the lunar phase query,
   then disconnects.

   Returns true if successful; false otherwise.
*/
boolean doNetworkWork() {
  Serial.print(F("Connecting to "));
  Serial.println(wifiSsid);

  WiFi.begin(wifiSsid, wifiPassword);
  // Wait for the connection or timeout. Put this in the state machine.
  int wifiStatus = WiFi.status();
  while (wifiStatus != WL_CONNECTED)
  {
    delay(100);
    Serial.print(".");

    wifiStatus = WiFi.status();
  }

  Serial.println();

  Serial.print("Connected, IP address: ");
  Serial.println(WiFi.localIP());


  // Do a query to get the date and the age of the moon.

  if (!query(&dateTimeUTC, &daysSinceNewMoon, &illuminatedPC)) {
    Serial.println(F("Query Failed."));
    httpGet.end();
    // wifi.disconnect(); How to disconnect the ESP8266 WiFi?
    return false;
  }

  httpGet.end();
  // wifi.disconnect();
  return true;
}

/*
   Query the moon phase web site,
   setting the UTC date and the age of the moon.

   This function is designed to read
     HM Nautical Almanac Office: Miscellanea.
     Daily Rise/set and Twilight times for the British Isles.
*/
boolean query(struct HttpDateTime *pDateTimeUTC, double *pDaysSinceNewMoon,
              int *pIlluminatedPC) {

  Serial.print(F("Querying "));
  Serial.print(PageUrl);
  Serial.println(F(" ..."));

  httpGet.useHTTP10(true); // to prevent chunking, which adds garbage characters.
  httpGet.begin(PageUrl);
  int httpCode = httpGet.GET();
  if (!(200 <= httpCode && httpCode < 300)) {
    Serial.print("HTTP Get failed. Code = ");
    Serial.println(httpCode);
    return false;
  }

  pHttpStream = httpGet.getStreamPtr();

  //ESP8266 lib doesn't return headers this way.
  //  if (!findDate(pDateTimeUTC)) {
  //    Serial.println(F("No Date: in header"));
  //    return false;
  //  }
  //
  //  Serial.println(F("Found Date: "));
  //  Serial.print(F("days since Sunday: "));
  //  Serial.println(pDateTimeUTC->daySinceSunday);
  //  Serial.print(F("day of month: "));
  //  Serial.println(pDateTimeUTC->day);
  //  Serial.print(F("Month: "));
  //  Serial.println(pDateTimeUTC->month);
  //  Serial.print(F("Year: "));
  //  Serial.println(pDateTimeUTC->year);
  //  Serial.print(F("Hour: "));
  //  Serial.println(pDateTimeUTC->hour);
  //  Serial.print(F("Minute: "));
  //  Serial.println(pDateTimeUTC->minute);
  //  Serial.print(F("Second: "));
  //  Serial.println(pDateTimeUTC->second);

  if (!pHttpStream->find("age of the Moon is ")) {
    Serial.println(F("No age of moon in response."));
    return false;
  }
  *pDaysSinceNewMoon = readDouble();
  if (*pDaysSinceNewMoon == DBL_MAX) {
    return false;
  }

  Serial.print(F("Days since new moon: "));
  Serial.println(*pDaysSinceNewMoon);

  if (!pHttpStream->find("fraction is ")) {
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
   Skips to the "Date:" Http header
   then parses the date header, through the timezone.
   The Timezone must be GMT
   Return true if successful, false otherwise.

   Example date header returned in the HTTP response from a web server:
   Date: Fri, 21 Aug 2015 22:06:40 GMT

   To use:
     Struct HttpDateTime dateTime;
     ...
     findDate(&dateTime);
     Serial.print(dateTime.year);
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

  if (!pHttpStream->find("Date: ")) {
    // No Date header found in response.
    return false;
  }

  // Day of week: Sun Mon Tue Wed Thu Fri Sat
  if (!pHttpStream->read(buf, 3)) {
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
  if (!pHttpStream->read(buf, 2)) {
    return false;
  }

  // Day of the month: 1..31
  if (!pHttpStream->read(buf, 2)) {
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
  if (pHttpStream->read() < 0) {
    return false;
  }

  // Month: Jan Feb Mar Apr May Jun Jul Aug Sep Oct Nov Dec
  if (!pHttpStream->read(buf, 3)) {
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
  if (pHttpStream->read() < 0) {
    return false;
  }

  // Year: 1900..2100 or so.
  if (!pHttpStream->read(buf, 4)) {
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
  if (pHttpStream->read() < 0) {
    return false;
  }

  // Hour: 00..23
  if (!pHttpStream->read(buf, 2)) {
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
  if (pHttpStream->read() < 0) {
    return false;
  }

  // Minute: 00..59
  if (!pHttpStream->read(buf, 2)) {
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
  if (pHttpStream->read() < 0) {
    return false;
  }

  // Second: 00..61 (usually 00..59)
  if (!pHttpStream->read(buf, 2)) {
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
  if (pHttpStream->read() < 0) {
    return false;
  }

  // Timezone: GMT hopefully.
  if (!pHttpStream->read(buf, 3)) {
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
   Read a double-floating-point value from the input,
   and the character just past that double.
   For example "11.9X" would return 11.9 and would read
   the X character following the string "11.9"
   Note: there must be at least one character following the number.
   That is, the input mustn't end immediately after the number.

   Accepts unsigned decimal numbers such as
   34
   15.
   90.54
   .2

   Returns either the decimal number, or DBL_MAX (see <float.h>) if an error occurs.
*/
double readDouble() {
  int ch;

  double result = 0.0;

  // Read the integer part of the number (if there is one)

  boolean sawInteger = false;
  ch = pHttpStream->read();
  while ('0' <= (char) ch && (char) ch <= '9') {
    sawInteger = true;
    result *= 10.0;
    result += (char) ch - '0';

    ch = pHttpStream->read();
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
  ch = pHttpStream->read();
  while ('0' <= (char) ch && (char) ch <= '9') {
    sawInteger = true;
    result += scale * ((char) ch - '0');
    scale /= 10.0;

    ch = pHttpStream->read();
  }
  if (ch < 0) {
    return DBL_MAX;
  }
  if (!sawInteger) {
    return DBL_MAX;
  }

  return result;
}

/*
   Move the given number of whole steps clockwise.
   steps = number of steps to move. positive is clockwise; negative is counterclockwise.
   Note: steps can't be larger than +/-32767
*/
void step(int steps) {
  int nextIdx;
  int number;  // positive number of steps to perform.

  if (steps >= 0) {
    number = steps;
  } else {
    number = -steps;
  }

  for (int i = 0; i < number; ++i) {

    // find the next index into sequence[], based on the direction
    if (steps >= 0) {
      nextIdx = curSeq + 1;
      if (nextIdx >= SEQUENCE_STEPS) {
        nextIdx = 0;
      }
    } else {
      nextIdx = curSeq - 1;
      if (nextIdx < 0) {
        nextIdx = SEQUENCE_STEPS - 1;
      }
    }

    /*
       Perform the step:
       1) activate the current and next coils (1/2 step).
       2) give the motor time to turn
       3) activate just the next coil (another 1/2 step).
       4) give the motor time to turn
       5) turn everything off so we don't waste power.
    */

    digitalWrite(sequence[curSeq], HIGH);
    digitalWrite(sequence[nextIdx], HIGH);
    delay(PULSE_WIDTH_MS);
    digitalWrite(sequence[curSeq], LOW);
    delay(PULSE_WIDTH_MS);
    digitalWrite(sequence[nextIdx], LOW);

    curSeq = nextIdx;
  }
}

/********************************
   From https://github.com/bneedhamia/write_eeprom_strings example
*/
/*
   Reads a string from EEPROM.  Copy this code into your program that reads EEPROM.

   baseAddress = EEPROM address of the first byte in EEPROM to read from.
   stringNumber = index of the string to retrieve (string 0, string 1, etc.)

   Assumes EEPROM contains a list of null-terminated strings,
   terminated by EEPROM_END_MARK.

   Returns:
   A pointer to a dynamically-allocated string read from EEPROM,
   or null if no such string was found.
*/
char *readEEPROMString(int baseAddress, int stringNumber) {
  int start;   // EEPROM address of the first byte of the string to return.
  int length;  // length (bytes) of the string to return, less the terminating null.
  char ch;
  int nextAddress;  // next address to read from EEPROM.
  char *result;     // points to the dynamically-allocated result to return.
  int i;


#if defined(ESP8266)
  EEPROM.begin(512);
#endif

  nextAddress = START_ADDRESS;
  for (i = 0; i < stringNumber; ++i) {

    // If the first byte is an end mark, we've run out of strings too early.
    ch = (char) EEPROM.read(nextAddress++);
    if (ch == (char) EEPROM_END_MARK) {
#if defined(ESP8266)
      EEPROM.end();
#endif
      return (char *) 0;  // not enough strings are in EEPROM.
    }

    // Read through the string's terminating null (0).
    int length = 0;
    while (ch != '\0' && length < EEPROM_MAX_STRING_LENGTH - 1) {
      ++length;
      ch = EEPROM.read(nextAddress++);
    }
  }

  // We're now at the start of what should be our string.
  start = nextAddress;

  // If the first byte is an end mark, we've run out of strings too early.
  ch = (char) EEPROM.read(nextAddress++);
  if (ch == (char) EEPROM_END_MARK) {
#if defined(ESP8266)
    EEPROM.end();
#endif
    return (char *) 0;  // not enough strings are in EEPROM.
  }

  // Count to the end of this string.
  length = 0;
  while (ch != '\0' && length < EEPROM_MAX_STRING_LENGTH - 1) {
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
int get_free_memory();

void	Ram_TableDisplay(void)
{
  Serial.println("No Ram display on ESP8266");
  // ESP8266 does have "ESP.getFreeHeap() and doesn't have __malloc_margin.
  //  char stack = 1;
  //  extern char *__data_start;
  //  extern char *__data_end;
  //  extern char *__bss_start;
  //  extern char *__bss_end;
  //  extern char *__heap_start;
  //  extern char *__heap_end;
  //
  //  int	data_size	=	(int)&__data_end - (int)&__data_start;
  //  int	bss_size	=	(int)&__bss_end - (int)&__data_end;
  //  int	heap_end	=	(int)&stack - (int)&__malloc_margin; Unsupported on ESP8266
  //  int	heap_size	=	heap_end - (int)&__bss_end;
  //  int	stack_size	=	RAMEND - (int)&stack + 1;
  //  int	available	=	(RAMEND - (int)&__data_start + 1);
  //  available	-=	data_size + bss_size + heap_size + stack_size;
  //
  //  Serial.println();
  //  Serial.print(F("data size     = "));
  //  Serial.println(data_size);
  //  Serial.print(F("bss_size      = "));
  //  Serial.println(bss_size);
  //  Serial.print(F("heap size     = "));
  //  Serial.println(heap_size);
  //  Serial.print(F("stack used    = "));
  //  Serial.println(stack_size);
  //  Serial.print(F("stack available     = "));
  //  Serial.println(available);
  //  Serial.print(F("Free memory   = "));
  //  Serial.println(get_free_memory());
  //  Serial.println();

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


