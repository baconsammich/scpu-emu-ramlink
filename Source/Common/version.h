/*
   SCPU-EMU public build identity.

   Keep this independent of the local kernel8-NNN hardware-test archives:
   those numbers depend on which experimental images exist on one developer's
   machine and are not reproducible from the repository.
*/
#ifndef _scpu_version_h
#define _scpu_version_h

// The RAMLink fork. The suffix is deliberate and load-bearing: this build
// fits a RAMLink by default and reads a RAMCard image off the SD card, so a
// card carrying it does NOT behave like a stock 00.03.00 card. Anyone reading
// a serial log or an HDMI banner has to be able to tell the two apart without
// asking which image is installed.
#define SCPU_EMULATOR_BUILD "00.07.06-RL"

#endif
