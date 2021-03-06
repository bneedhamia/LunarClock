![The Project so far](https://github.com/bneedhamia/LunarClock/blob/master/Project.jpg)
# LunarClock
An ESP8266 Thing Arduino Sketch to display the current phase of the moon,
as reported by
[HM Nautical Almanac Office: Miscellanea, Daily Rise/set and Twilight times for the British Isles](http://astro.ukho.gov.uk/nao/miscellanea/birs2.html). It does this by rotating a disk
that has 8 lunar images on it, to align the appropriate image with a window in the clock.

That page provides the time of sunrise/sunset/etc., for many locations in Britain and Ireland.
The end of that page provides the age of the moon (days since the new moon) and
the percentage of its face that is illuminated. Of all that information, only the age of the moon
is used by this Sketch.

The current Sketch requires a Sparkfun EPS8266 Thing Dev board, a Photo-interrupter,
and an AdaFruit 28BYJ-48 12V stepper motor, among other things.
Details are in the .ino Sketch PIN_ definitions and in BillOfMaterials.ods.

NOTE: This is a work in progress.

State of the project: On reset, the Sketch turns the lunar disk wheel to find the slot, then turns it to align
the first lunar image in the (to be built) clock window. It then connects to the local WiFi,
reads and parses HM Almanac page mentioned above, and rotates the wheel to show the corresponding
lunar image. It then stops. Further major changes await the design of the clock face and box.

Next steps: design the 3D printed clock face and box;
finish writing the state machine code, to retry wifi failures and query the site once every day.

## Files
* BillOfMaterials.ods = the parts list
* LICENSE = the project GPL2 license file
* LunarClock.ino = the ESP8266 Thing Dev board Arduino Sketch
* LunarClockDiary.odt = my project diary.  All of the little details of the path from idea to reality (so far)
* lunarDisk.FCStd = FreeCAD model file for the 3D printed lunar images disk and holder for stepper motor and photo interupter.
* LunarDisk.stl = 3D printable file of the disk of lunar images. Swap filament in mid-print to print light lunar images on a dark disk.
* LunarWheel.svg = (obsolete) Inkscape (laser cutting) file for the wheel of lunar images.
* Project.jpg = a photo of the project so far
* README.md = this file
* StepperHolder.stl = 3D Printable file for the plate that holds the stepper motor and photo interrupter.
* XXXXX to add as I create them: fritzing diagram; mechanical and assembly notes; other files for 3D printing.
