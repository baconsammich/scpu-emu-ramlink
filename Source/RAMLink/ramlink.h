/*
   SCPU-EMU - CMD SuperCPU emulation for the C64/C128 using a RAD Expansion Unit
   Copyright (c) 2026 SCPU-EMU contributors

   CMD RAMLink.

   A cartridge-port box with battery-backed RAM, its own DOS in a 64K EPROM, a
   real-time clock, a pass-through "RAM-Port" for another cartridge, and a
   parallel bus to a CMD hard drive. To the C64 it is a disk device that happens
   to be made of RAM.

   SOURCE OF TRUTH: VICE's `vice/src/c64/cart/ramlink.c`, the same methodology
   the SuperCPU register file was written against (see registers.h). The
   documentation for RAMLink's register block is thin and partly wrong; VICE's
   implementation is driven by CMD's own ROM and is the only description that has
   been run against real software.

   THE REGISTER MAP

     $DE00-$DEFF   a 256-byte window. WHAT it is a window onto depends on the
                   mode selected at $DFC0-$DFC3:
                     mode 0  RAMLink's own 8K static RAM, paged by $DF80-$DF9F
                     mode 1  the RAMCard, at the address latched at $DFA0-$DFA3
                     mode 2  the RAM-Port pass-through (a GEORAM/RAMDrive)
                     mode 3  pass-through to whatever else decodes $DE00
     $DF20-$DF22   RAM-Port REU trap: $DF22 arms, $DF20 disarms, $DF21 forwards
     $DF40-$DF43   i8255A PPI -- the parallel bus to a CMD HD
     $DF60 / $DF70 map / unmap the RAMLink DOS ROM
     $DF7E / $DF7F turn RAMLink on / off. THESE TWO ARE ALWAYS DECODED, even
                   when everything else is switched off -- otherwise there
                   would be no way back on.
     $DF80-$DF9F   select which 256-byte page of the 8K RAM appears at $DE00
     $DFA0-$DFA3   RAMCard address latch, bits 8..31
     $DFB0-$DFBF   RTC 72421
     $DFC0-$DFC3   select the $DE00 window source (the low two address bits)

   WHAT THIS IMPLEMENTS, AND WHAT IT DOES NOT

   The register file, the two memory windows, the RAMCard size decode and the
   clock are all here and tested. Three things are deliberately out of scope,
   and each has a reason rather than being unfinished:

     * The DOS ROM is NOT banked into $8000-$BFFF. That is not a simplification
       -- it is what the hardware does. VICE's `ramlink_roml_read()` and
       `ramlink_a000_bfff_read()` both begin "do not map this for super cpu" and
       return CART_READ_THROUGH when `machine_class == VICE_MACHINE_SCPU64`.
       On an accelerated machine the SuperCPU's own DOS carries the RAMLink
       support, which is exactly why the SuperCPU has a "RAMLink registers" bit
       at $D0BC and counts RAMLink in its interrupt-vector reroute predicate.
       The image is still loaded and readable through romImage() for the
       SuperCPU side to use.

     * The CMD parallel bus has no device on the far end. The i8255A's port
       latches behave correctly, and a probe for a CMD HD therefore fails
       cleanly and quickly, which is the honest answer when no drive is
       attached.

     * The RAM-Port modes (2 and 3) decline the access rather than forwarding it
       to a nested cartridge. The REU in this project sits on the machine's own
       I/O, not inside RAMLink's pass-through, so there is nothing to forward
       to. The $DF21 trap is wired to whatever IRAMLinkREU is attached, because
       that path is how RAMLink's DOS drives an REU it has claimed I/O2 from.

   This program is free software: you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation, either version 3 of the License, or
   (at your option) any later version.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/
#ifndef _scpu_ramlink_h
#define _scpu_ramlink_h

#include "../Common/types.h"

// --- RLSIZE config values ---------------------------------------------------
// A selector, not a size, for the same reason REUSIZE is: the list can grow
// without renumbering, and 1 means "no RAMLink" so a missing or mistyped key is
// distinguishable from a deliberate "off".
//
//   RLSIZE 1   no RAMLink       (default)
//   RLSIZE 2   1 MB RAMCard
//   RLSIZE 3   2 MB
//   RLSIZE 4   4 MB
//   RLSIZE 5   8 MB
//   RLSIZE 6   16 MB            the largest a standard RAMLink takes
#define RLSIZE_NONE   1
#define RLSIZE_1MB    2
#define RLSIZE_2MB    3
#define RLSIZE_4MB    4
#define RLSIZE_8MB    5
#define RLSIZE_16MB   6

// The 8K of static RAM inside RAMLink itself, and the 64K EPROM.
#define RAMLINK_RAM_SIZE   0x2000
#define RAMLINK_ROM_SIZE   0x10000

// ROM bank offsets. Banks 0 and 1 are the RAMLink DOS, selected through the
// i8255A's port C; bank 2 is the C64 KERNAL replacement (it carries JiffyDOS
// 6.01) and bank 3 the C128 one. Confirmed by inspection of a 2.01 image.
#define RAMLINK_KERNBASE_64   ( 2 * 0x4000 )
#define RAMLINK_KERNBASE_128  ( 3 * 0x4000 )

// Register addresses, for callers deciding whether an access is ours.
#define RAMLINK_IO1_FIRST   0xDE00
#define RAMLINK_IO1_LAST    0xDEFF
#define RAMLINK_REG_FIRST   0xDF20
#define RAMLINK_REG_LAST    0xDFC3
#define RAMLINK_REG_ON      0xDF7E
#define RAMLINK_REG_OFF     0xDF7F

// The $DE00 window source, as latched by $DFC0-$DFC3.
#define RAMLINK_IO1_RL_RAM    0
#define RAMLINK_IO1_RAMCARD   1
#define RAMLINK_IO1_RAMPORT   2
#define RAMLINK_IO1_PASSTHRU  3
// Power-on value. VICE starts at 7 -- deliberately not one of the four decoded
// modes -- so an IO1 access before the ROM has selected a source reads open bus
// rather than silently picking one.
#define RAMLINK_IO1_UNSET     7

// Wall-clock source for the RTC. Injected so tests can pin a time rather than
// asserting against whatever the host clock happens to say.
class IRAMLinkClock
{
public:
	virtual ~IRAMLinkClock() {}
	// Broken-down local time. Year is the full year, month 1-12, weekday 0-6
	// with 0 = Sunday.
	virtual void ramlinkNow( u32 &year, u32 &month, u32 &day, u32 &weekday,
	                         u32 &hour, u32 &minute, u32 &second ) = 0;
};

// The RAM-Port REU trap. RAMLink claims I/O2 for itself, so a program that
// wants to run an REU transfer while RAMLink is on cannot reach $DF01 directly;
// $DF22 arms a trap and $DF21 then forwards to the REU's command register.
class IRAMLinkREU
{
public:
	virtual ~IRAMLinkREU() {}
	virtual void ramlinkREUCommand( u8 value ) = 0;
};

class CRAMLink
{
public:
	CRAMLink();
	~CRAMLink();

	// Allocate the RAMCard. Takes an RLSIZE_* selector, not a size. An unknown
	// selector is treated as RLSIZE_NONE rather than guessed at. Returns false
	// only if a requested allocation failed, in which case the unit reports
	// itself absent.
	bool init( u8 sizeSelector );

	// The 64K EPROM. Without one the unit still answers its registers -- the
	// hardware does -- but nothing can boot from it.
	void setROM( const u8 *image, u32 length );
	bool hasROM() const { return m_ROMLength != 0; }
	const u8 *romImage() const { return m_ROM; }
	u32  romLength() const { return m_ROMLength; }

	void attachClock( IRAMLinkClock *clock ) { m_Clock = clock; }
	void attachREU( IRAMLinkREU *reu ) { m_REU = reu; }

	bool present() const       { return m_CardSize != 0 || m_Fitted; }
	u32  cardSizeBytes() const { return m_CardSize; }
	u32  cardSizeMB() const    { return m_CardSize >> 20; }

	// Copy a saved RAMCard image into the card, returning the number of bytes
	// actually taken. A short image fills the front and leaves the rest; a long
	// one is truncated. See the note on the definition.
	u32 loadCardImage( const u8 *data, u32 length );

	// Power-on / reset. The RAMCard is battery-backed and is NOT cleared --
	// that is the entire point of a RAMLink, and software relies on a partition
	// table surviving a reset. The 8K internal RAM IS re-seeded, with each page
	// holding its own page number, which is what VICE's config_init does.
	void reset();

	// --- register file ----------------------------------------------------
	// Return false when the address is not ours, so a caller chaining several
	// devices can pass it on.
	bool read( u16 addr, u8 &value );
	bool write( u16 addr, u8 value );

	// --- state, for the SuperCPU side and for diagnostics -----------------
	// on() is what $D0BC bit 6 reports and what the interrupt-vector reroute
	// predicate consumes.
	bool on() const        { return m_On; }
	// Live pointer to the on/off state. The KERNAL substitution is decided per
	// read, and $DF7E/$DF7F can flip between two instructions, so a snapshot
	// taken at init would be wrong for the rest of the run.
	const bool *onStatePtr() const { return &m_On; }
	bool dosMapped() const { return m_DOSMapped; }
	const bool *dosMappedStatePtr() const { return &m_DOSMapped; }
	const u32  *romBaseStatePtr() const { return &m_ROMBase; }
	u8   io1Mode() const   { return m_IO1Mode; }
	u16  ramBase() const   { return m_RAMBase; }
	u32  cardAddress() const { return m_CardAddr; }
	// -1 when the latched card address decodes to open bus, which is how
	// software discovers the fitted size.
	s64  cardBase() const  { return m_CardBase; }
	u32  romBank() const   { return m_ROMBase; }

	// Direct access for tests and for image load/save. Masked into range: a
	// real card decodes a fixed number of address lines.
	u8  *cardRAM()     { return m_Card; }

	// Has anything written to the RAMCard since it was loaded?
	//
	// A TOUCH flag: set by any guest write through the $DE00 window, whatever
	// the value. loadCardImage() clears it, because a freshly loaded image is
	// by definition what is already on disk.
	//
	// On its own this is the wrong question to ask before persisting. Measured:
	// booting CMD's DOS with a RAMLink fitted touches the card and changes
	// ZERO of its 8388608 bytes -- it writes values that were already there.
	// So a save guarded on this alone would rewrite 8MB on every single
	// session, which is slow and is a chance to damage the image for no gain.
	// It is kept as a cheap short-circuit: not touched means certainly
	// unchanged, and skips the scan below.
	bool cardDirty() const  { return m_CardDirty; }
	void clearCardDirty()   { m_CardDirty = false; }

	// Has the RAMCard's CONTENT actually changed since it was loaded?
	//
	// This is the question worth asking, and the one persistence uses. The
	// touch flag gates it so an untouched card costs nothing.
	bool cardChanged() const
	{
		if ( !m_Card || !m_CardSize || !m_CardDirty ) return false;
		return cardChecksum() != m_CardLoadedSum;
	}

	// FNV-1a over the whole card. Not a security hash -- the only requirement
	// is that two different card images are overwhelmingly unlikely to agree,
	// because agreeing means silently skipping a save the user wanted.
	u64 cardChecksum() const
	{
		if ( !m_Card || !m_CardSize ) return 0;
		u64 h = 1469598103934665603ULL;
		for ( u32 i = 0; i < m_CardSize; i++ )
		{
			h ^= m_Card[ i ];
			h *= 1099511628211ULL;
		}
		return h;
	}

	// Call after a successful save: the image on disk now matches the card.
	void noteCardSaved()
	{
		m_CardLoadedSum = cardChecksum();
		m_CardDirty = false;
	}
	u8  *internalRAM() { return m_RAM; }

	u64  registerWrites() const { return m_RegWrites; }

private:
	CRAMLink( const CRAMLink & ) = delete;
	CRAMLink &operator=( const CRAMLink & ) = delete;

	void recomputeCardBase();
	void setCardSizeMB( u32 mb );
	u8   rtcRead( u8 reg );

	// Port C bits 7-6: the CMD parallel bus's two handshake lines, as the
	// i8255A sees them. They are inverted on the way in, and nothing drives
	// the bus here -- no CMD drive is modelled -- so it idles high and both
	// bits read back as 0. Named rather than inlined because "the idle value
	// of these two bits is zero" is the surprising part.
	static u8 cmdBusHandshakeBits() { return 0x00; }
	void rtcWrite( u8 reg, u8 value );

	IRAMLinkClock *m_Clock;
	IRAMLinkREU   *m_REU;

	u8  *m_Card;			// the RAMCard; battery-backed, survives reset
	bool m_CardDirty = false;	// a guest write has touched the card
	u64  m_CardLoadedSum = 0;	// checksum of the card as loaded/saved
	u32  m_CardSize;		// 0 when absent
	u32  m_CardSizeMB;
	bool m_Fitted;			// a unit is present even with a 0MB card

	u8   m_RAM[ RAMLINK_RAM_SIZE ];		// RAMLink's own 8K
	const u8 *m_ROM;
	u32  m_ROMLength;

	// 1MB granules of the 64MB card address space onto the fitted card.
	// -1 means nothing decodes there and the bus is open. This IS the size
	// detection mechanism, so it is a table rather than a mask.
	s8   m_MemMap[ 64 ];

	bool m_On;				// $DF7E / $DF7F
	bool m_DOSMapped;		// $DF60 / $DF70
	bool m_REUTrap;			// $DF22 / $DF20
	u8   m_IO1Mode;			// $DFC0-$DFC3
	u16  m_RAMBase;			// $DF80-$DF9F, a page number << 8
	u32  m_CardAddr;		// $DFA0-$DFA3
	s64  m_CardBase;		// decoded m_CardAddr, or -1 for open bus
	u32  m_ROMBase;			// i8255A port C bits 1-0, * $4000

	// i8255A port latches. Output values as written, input values as the
	// pull-ups present them with no device on the parallel bus.
	u8   m_PPIOut[ 3 ];
	u8   m_PPIIn[ 3 ];
	u8   m_PPIControl;

	// RTC 72421 write-latched registers. The clock itself is read from the
	// injected source; these hold what software stored so it reads back --
	// register 14 in particular, which is how RAMLink detects the clock exists.
	u8   m_RTCRegs[ 16 ];
	bool m_RTCHour24;		// control bit 2. Powers up FALSE: 12-hour mode.
	bool m_RTCStop;

	// What init() was last asked for, so re-fitting the same card can preserve
	// its contents rather than reallocating and wiping them.
	u8   m_SizeSelector;

	u64  m_RegWrites;
};

#endif
