# LunarClock
Arduino Sketch to display the current phase of the moon,
as reported by
[HM Nautical Almanac Office: Miscellanea, Daily Rise/set and Twilight times for the British Isles]
(http://astro.ukho.gov.uk/nao/miscellanea/birs2.html).

That page provides the time of sunrise/sunset/etc., for many locations in Britain and Ireland.
The end of that page provides the age of the moon (days since the new moon) and
the percentage of its face that is illuminated.

The current Sketch requires an Arduino Mega, a Sparkfun Transmogrishield,
a Sparkfun CC3000 WiFi Shield, and an obsolete stepper motor.  Details are in the code.

NOTE: This is a work in progress. I'm currently developing it.

State: it turns an AdaFruit 28BYJ-48 12V stepper motor using the Stepper Arduino library;
it reads HM Almanac page to read the date/time (UTC), the current age of the moon in days,
and percent of its face that is illuminated; the state machine to run the whole thing is
defined and partially implemented. The code to position the wheel based on the opto-interrupter is in place,
and is awaiting the mechanical design.

Next steps: replace the Arduino Stepper library with my own code, that shuts off the motor when not in use.
start sketching the rest of the to-be-laser-cut mechanical parts.

## Files
* LunarClock.ino = the Arduino Sketch
* BillOfMaterials.ods = the parts list
* LunarWheel.svg = Inkscape (laser cutting) file for the wheel of lunar images.
* LunarClockDiary.odt = my project diary.  All of the little details of the path from idea to reality (so far)
* LICENSE = the project GPL2 license file
* README.md = this file
* XXXXX to add as I create them: fritzing diagram; mechanical notes; files for laser cutting.
