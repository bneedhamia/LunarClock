# LunarClock
Arduino Sketch to display the current phase of the moon,
as reported by
[HM Nautical Almanac Office: Miscellanea, Daily Rise/set and Twilight times for the British Isles]
(http://astro.ukho.gov.uk/nao/miscellanea/birs2.html).

That page provides the time of sunrise/sunset/etc., for many locations in Britain.
The end of that page provides the age of the moon (days since the new moon) and
the percentage of its face that is illuminated.

XXX Not ready to use yet.  I'm currently developing it.
in particular, it uses a modified version of the Sparkfun ESP8266 WiFi
Shield library, and I'm waiting to commit that until I learn how
to create a good pull request.  The mods to the library are to support
the Serial1, Serial2, and Serial3 pins of the Arduino Mega.

State: it reads HM Almanac page to read the date/time (UTC), the current age of the moon,
and percent of its face that is illuminated.
