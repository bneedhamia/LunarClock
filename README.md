# LunarClock
Arduino Sketch to display the current phase of the moon,
as reported by
[HM Nautical Almanac Office: Miscellanea, Daily Rise/set and Twilight times for the British Isles]
(http://astro.ukho.gov.uk/nao/miscellanea/birs2.html).

That page provides the time of sunrise/sunset/etc., for many locations in Britain and Ireland.
The end of that page provides the age of the moon (days since the new moon) and
the percentage of its face that is illuminated.

The current Sketch requires an Arduino Mega, a Sparkfun Transmogrishield,
and a Sparkfun CC3000 WiFi Shield.  Details are in the code.

NOTE: This is a work in progress. I'm currently developing it.

State: it reads HM Almanac page to read the date/time (UTC), the current age of the moon in days,
and percent of its face that is illuminated.
