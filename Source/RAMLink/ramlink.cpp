/*
   SCPU-EMU - CMD SuperCPU emulation for the C64/C128 using a RAD Expansion Unit
   Copyright (c) 2026 SCPU-EMU contributors

   CMD RAMLink. Register layout and behaviour per VICE; see ramlink.h.

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
#include "ramlink.h"

#ifdef SCPU_HOST_BUILD
	#include <stdlib.h>
	#include <string.h>
#else
	#include <circle/util.h>
	#include <stdlib.h>
#endif

CRAMLink::CRAMLink()
	: m_Clock( 0 ), m_REU( 0 ),
	  m_Card( 0 ), m_CardSize( 0 ), m_CardSizeMB( 0 ), m_Fitted( false ),
	  m_ROM( 0 ), m_ROMLength( 0 ),
	  m_On( false ), m_DOSMapped( false ), m_REUTrap( false ),
	  m_IO1Mode( RAMLINK_IO1_UNSET ), m_RAMBase( 0 ),
	  m_CardAddr( 0 ), m_CardBase( -1 ), m_ROMBase( 0 ),
	  m_PPIControl( 0 ), m_RTCHour24( false ), m_RTCStop( false ),
	  m_SizeSelector( RLSIZE_NONE ),
	  m_RegWrites( 0 )
{
	for ( u32 i = 0; i < RAMLINK_RAM_SIZE; i++ ) m_RAM[ i ] = 0;
	for ( u32 i = 0; i < 64; i++ ) m_MemMap[ i ] = -1;
	for ( u32 i = 0; i < 3; i++ ) { m_PPIOut[ i ] = 0xFF; m_PPIIn[ i ] = 0xFF; }
	for ( u32 i = 0; i < 16; i++ ) m_RTCRegs[ i ] = 0;
}

CRAMLink::~CRAMLink()
{
	if ( m_Card ) free( m_Card );
}

// The RAMCard address space is 64MB regardless of what is fitted, and the
// decode table is how software finds the edge. Below 4MB the fitted part
// repeats every 4MB; from 4 to 16MB the unfitted slots read open bus and the
// whole 16MB pattern repeats to 64MB. Faithful to VICE's set_size().
void CRAMLink::setCardSizeMB( u32 mb )
{
	m_CardSizeMB = mb;
	m_CardSize   = mb << 20;

	for ( u32 i = 0; i < 64; i++ ) m_MemMap[ i ] = (s8)i;

	if ( mb <= 4 )
	{
		for ( u32 i = mb; i < 4; i++ ) m_MemMap[ i ] = -1;
		for ( u32 i = 4; i < 64; i++ ) m_MemMap[ i ] = m_MemMap[ i & 3 ];
	}
	else if ( mb <= 16 )
	{
		for ( u32 i = mb; i < 16; i++ ) m_MemMap[ i ] = -1;
		// A card built from a 4MB SIMM plus a 1MB one shows the odd megabyte
		// repeated across the rest of its group rather than reading open.
		if ( mb & 3 )
			for ( u32 i = mb; i < ( ( mb / 4 ) + 1 ) * 4; i++ )
				m_MemMap[ i ] = (s8)( mb - 1 );
		for ( u32 i = 16; i < 64; i++ ) m_MemMap[ i ] = m_MemMap[ i & 15 ];
	}
	else
	{
		for ( u32 i = mb; i < 64; i++ ) m_MemMap[ i ] = -1;
	}
}

bool CRAMLink::init( u8 sizeSelector )
{
	// Fitting the SAME card again is not a wipe -- it is the same card. This
	// matters for a real reason rather than tidiness: the boot path has to
	// allocate the RAMCard and load its image while the SD card is still
	// mounted, which is BEFORE CSuperCPU::init() runs, and init() would
	// otherwise free the buffer and memset the image away. Preserving contents
	// here is also the honest model of battery-backed RAM.
	if ( m_Fitted && m_Card && sizeSelector == m_SizeSelector )
	{
		reset();
		return true;
	}

	// A different size means a different card. Whatever was written to the old
	// one belongs to a card that no longer exists, so the dirty flag goes with
	// it -- persisting it would write the wrong card's contents to the image.
	if ( m_Card ) { free( m_Card ); m_Card = 0; }
	m_CardSize = 0;
	m_CardSizeMB = 0;
	m_CardDirty = false;
	m_CardLoadedSum = 0;
	m_Fitted = false;
	m_SizeSelector = sizeSelector;

	u32 mb;
	switch ( sizeSelector )
	{
	case RLSIZE_1MB:  mb =  1; break;
	case RLSIZE_2MB:  mb =  2; break;
	case RLSIZE_4MB:  mb =  4; break;
	case RLSIZE_8MB:  mb =  8; break;
	case RLSIZE_16MB: mb = 16; break;
	default:
		// RLSIZE_NONE, and anything unrecognised. Not rounded to a nearby size:
		// a mistyped selector must not silently fit hardware.
		for ( u32 i = 0; i < 64; i++ ) m_MemMap[ i ] = -1;
		reset();
		return true;
	}

	m_Card = (u8 *)malloc( mb << 20 );
	if ( !m_Card )
	{
		for ( u32 i = 0; i < 64; i++ ) m_MemMap[ i ] = -1;
		reset();
		return false;
	}

	// A real RAMCard is battery-backed and comes up holding what it held. There
	// is no battery here, so zero is the reproducible choice; an image loaded
	// over the top through cardRAM() is what gives a card real persistence.
	memset( m_Card, 0, mb << 20 );

	m_Fitted = true;
	setCardSizeMB( mb );
	// Baseline for cardChanged(). A card with no image loaded starts blank, and
	// formatting one from RAMLink's own tools and then saving it is a perfectly
	// good way to create ramlink.img -- so "blank" has to be a state the change
	// detector can measure against, not a special case.
	m_CardLoadedSum = cardChecksum();
	reset();
	return true;
}

// Copy a saved RAMCard image in. Returns how many bytes were taken.
//
// A short image fills the front of the card and leaves the rest as it was,
// which is what you want when a 1MB image is dropped onto a 4MB card: the
// partition table is at the front and the card simply has unformatted space
// after it. A long image is truncated rather than refused, for the same
// reason in reverse -- an 8MB image on a 4MB card yields a card holding the
// first 4MB, which CMD's own tools can still read a partition table out of.
// Either way the caller is told the real number so it can say so.
u32 CRAMLink::loadCardImage( const u8 *data, u32 length )
{
	if ( !m_Card || !m_CardSize || !data || !length )
		return 0;

	const u32 n = ( length > m_CardSize ) ? m_CardSize : length;
	memcpy( m_Card, data, n );
	// The card now holds exactly what is on disk, which is the definition of
	// not dirty. Setting it here instead would make the first save after every
	// boot rewrite 8MB for nothing.
	m_CardDirty = false;
	m_CardLoadedSum = cardChecksum();
	return n;
}

void CRAMLink::setROM( const u8 *image, u32 length )
{
	m_ROM = image;
	m_ROMLength = ( length > RAMLINK_ROM_SIZE ) ? RAMLINK_ROM_SIZE : length;
}

void CRAMLink::reset()
{
	// NOT the RAMCard: it is battery-backed, and a partition table that did not
	// survive a reset would make the device useless. m_CardDirty is left alone
	// for the same reason -- writes made before a reset are still writes that
	// have not reached the SD card.
	m_On         = false;
	m_DOSMapped  = false;
	m_REUTrap    = false;
	m_IO1Mode    = RAMLINK_IO1_UNSET;
	m_RAMBase    = 0;
	m_CardAddr   = 0;
	m_ROMBase    = 0;
	m_PPIControl = 0;
	m_RegWrites  = 0;
	// The chip powers up in 12-hour mode; see the note above rtcRead().
	m_RTCHour24  = false;
	m_RTCStop    = false;
	recomputeCardBase();

	// The 8K internal RAM powers up with every page holding its own page
	// number. This is not decoration: it is a recognisable pattern, and VICE
	// seeds it in config_init because CMD's ROM scans for the device.
	for ( u32 i = 0; i < RAMLINK_RAM_SIZE; i++ )
		m_RAM[ i ] = (u8)( i >> 8 );

	for ( u32 i = 0; i < 3; i++ ) { m_PPIOut[ i ] = 0xFF; m_PPIIn[ i ] = 0xFF; }
	for ( u32 i = 0; i < 16; i++ ) m_RTCRegs[ i ] = 0;
}

void CRAMLink::recomputeCardBase()
{
	const s8 granule = m_MemMap[ ( m_CardAddr >> 20 ) & 63 ];
	if ( granule < 0 || !m_Card )
		m_CardBase = -1;					// open bus
	else
		m_CardBase = ( (s64)granule << 20 ) | ( m_CardAddr & 0x0FFFFF );
}

// --- RTC 72421 --------------------------------------------------------------
// Sixteen 4-bit registers. 0-12 are the clock, read live from the injected
// source so it tracks real time; 13-15 are control.
//
// Two details here are easy to get wrong and both come straight from VICE's
// rtc-72421.c:
//
//   * The chip powers up in TWELVE-hour mode (`retval->hour24 = 0`), and bit 2
//     of the control register selects TWENTY-FOUR hour mode when SET. Getting
//     the polarity backwards puts every timestamp out by up to twelve hours
//     while still looking like a plausible clock.
//
//   * Register 14 is a plain read/write latch with no clock meaning at all, and
//     VICE's comment says why: "RAMLINK writes/reads data to this register to
//     detect the presence of the rtc". If it does not read back, RAMLink
//     decides it has no clock.
//
// Register 15 is deliberately asymmetric: a write takes hour24 from bit 2 and
// stop from bit 1, but a read reports hour24 in bit 1 and stop in bit 0. That
// is what VICE does, and VICE is the implementation CMD's ROM has actually been
// run against, so it is reproduced rather than tidied.
u8 CRAMLink::rtcRead( u8 reg )
{
	switch ( reg )
	{
	case 13: return 0;						// CTRL0: no read path
	case 14: return (u8)( m_RTCRegs[ 14 ] & 0x0F );	// CTRL1: the presence probe
	case 15: return (u8)( ( m_RTCHour24 ? 2 : 0 ) | ( m_RTCStop ? 1 : 0 ) );
	default: break;
	}

	if ( !m_Clock )
		return 0;

	u32 year = 0, month = 1, day = 1, weekday = 0, hour = 0, minute = 0, second = 0;
	m_Clock->ramlinkNow( year, month, day, weekday, hour, minute, second );

	// The 12-hour form, computed once so both hour digits agree. Midnight and
	// noon are 12, not 0.
	const u32 hour12 = ( hour % 12 == 0 ) ? 12 : ( hour % 12 );
	const bool pm = hour >= 12;

	switch ( reg )
	{
	case 0:  return (u8)( second % 10 );
	case 1:  return (u8)( second / 10 );
	case 2:  return (u8)( minute % 10 );
	case 3:  return (u8)( minute / 10 );
	case 4:  return (u8)( m_RTCHour24 ? ( hour % 10 ) : ( hour12 % 10 ) );
	case 5:  return (u8)( m_RTCHour24
	                      ? ( hour / 10 )
	                      : ( ( hour12 / 10 ) | ( pm ? 0x04 : 0x00 ) ) );
	case 6:  return (u8)( day % 10 );
	case 7:  return (u8)( day / 10 );
	case 8:  return (u8)( month % 10 );
	case 9:  return (u8)( month / 10 );
	case 10: return (u8)( ( year % 100 ) % 10 );
	case 11: return (u8)( ( year % 100 ) / 10 );
	case 12: return (u8)( weekday % 7 );
	default: return 0;
	}
}

void CRAMLink::rtcWrite( u8 reg, u8 value )
{
	const u8 data = (u8)( value & 0x0F );
	m_RTCRegs[ reg & 15 ] = data;

	if ( ( reg & 15 ) == 15 )
	{
		m_RTCHour24 = ( data & 0x04 ) != 0;
		m_RTCStop   = ( data & 0x02 ) != 0;
	}

	// Writes to the clock registers themselves are accepted and remembered so
	// they read back, but they do not move the host's time. A device whose
	// clock guest software could wind backwards would make file timestamps
	// worse, not better, and nothing in RAMLink's DOS needs to set the time in
	// order to work.
}

// ---------------------------------------------------------------------------

bool CRAMLink::read( u16 addr, u8 &value )
{
	if ( !m_Fitted && !m_ROMLength )
		return false;						// no unit at all

	// $DE00-$DEFF: the window.
	//
	// Decoded whenever the unit is FITTED -- not only while it is switched on,
	// and not only once a source has been selected. VICE registers its io1
	// device at attach time and never looks at rl_on, and ramlink_io1_read()
	// answers 255 for any source it does not recognise rather than declining.
	//
	// That last part is not a detail. $DFC0-$DFC3 has not been written when
	// CMD's KERNAL first touches this window, so the source is still the
	// power-on value 7 -- "unused". Declining there sent those reads to the
	// C64, and the RAMLink KERNAL wedged in a loop at $FAF6 rather than
	// finishing its init.
	if ( addr >= RAMLINK_IO1_FIRST && addr <= RAMLINK_IO1_LAST )
	{
		if ( m_IO1Mode == RAMLINK_IO1_RL_RAM )
		{
			value = m_RAM[ ( m_RAMBase | ( addr & 0xFF ) ) & ( RAMLINK_RAM_SIZE - 1 ) ];
			return true;
		}
		if ( m_IO1Mode == RAMLINK_IO1_RAMCARD && m_Card )
		{
			if ( m_CardBase < 0 )
				return false;				// open bus, deliberately not ours
			value = m_Card[ ( (u32)m_CardBase | ( addr & 0xFF ) ) % m_CardSize ];
			return true;
		}
		// No source selected, or modes 2 and 3 -- the RAM-Port pass-through,
		// with nothing nested inside this RAMLink. The window is still ours
		// and still decodes; there is simply nothing behind it.
		value = 0xFF;
		return true;
	}

	// $DF7E/$DF7F are write-only strobes but are always decoded.
	if ( addr == RAMLINK_REG_ON || addr == RAMLINK_REG_OFF )
		return false;

	if ( !m_On )
		return false;						// everything else is switched off

	if ( addr >= 0xDF40 && addr <= 0xDF43 )
	{
		// i8255A. Port C's top two bits are the CMD parallel bus's PREADY and
		// PCLK, which idle high with no drive attached.
		//
		// A port configured as an OUTPUT reads back its own latch, not the
		// pins -- that is what the latch is for, and it is how software does a
		// read-modify-write on a port it drives. CMD's DOS relies on it: at
		// $8960 it reads $DF41, clears a bit and writes it back, and returning
		// the input state ($FF) there makes it drive $FD where the hardware
		// drives $E4. Ports configured as inputs read the pins.
		//
		// Control word bits, per the 8255 and VICE's i8255a_read():
		//   $10 group 1 port A in    $08 group 1 port C high nibble in
		//   $02 group 2 port B in    $01 group 2 port C low nibble in
		const u8 port = (u8)( addr & 3 );
		if ( port == 3 ) { value = m_PPIControl; return true; }
		if ( port == 0 )
		{
			value = ( m_PPIControl & 0x10 ) ? m_PPIIn[ 0 ] : m_PPIOut[ 0 ];
			return true;
		}
		if ( port == 1 )
		{
			value = ( m_PPIControl & 0x02 ) ? m_PPIIn[ 1 ] : m_PPIOut[ 1 ];
			return true;
		}
		// Port C is split: each nibble follows its own group's direction.
		//
		// Bits 7-6 are the CMD parallel bus's handshake lines, and they arrive
		// INVERTED -- VICE reads them as (cmdbus.bus ^ 0xff) & 0xc0. The bus
		// idles high with no drive attached, so those two bits read as 0, not
		// 1. Reading them as 1 made $DF42 answer $F0 where the hardware
		// answers $30, and CMD's DOS took the wrong branch at $8946.
		u8 c = 0xFF;
		if ( m_PPIControl & ( 0x08 | 0x01 ) )
			c = (u8)( m_PPIIn[ 2 ] & 0x3F ) | cmdBusHandshakeBits();
		if ( !( m_PPIControl & 0x01 ) )
			c = (u8)( ( c & 0xF0 ) | ( m_PPIOut[ 2 ] & 0x0F ) );
		if ( !( m_PPIControl & 0x08 ) )
			c = (u8)( ( c & 0x0F ) | ( m_PPIOut[ 2 ] & 0xF0 ) );
		value = c;
		return true;
	}

	if ( addr >= 0xDFB0 && addr <= 0xDFBF )
	{
		value = rtcRead( (u8)( addr & 15 ) );
		return true;
	}

	// The remaining register groups are write-only on real hardware.
	if ( ( addr >= 0xDF20 && addr <= 0xDF22 )
	  || ( addr >= 0xDF60 && addr <= 0xDF60 )
	  || ( addr >= 0xDF70 && addr <= 0xDF70 )
	  || ( addr >= 0xDF80 && addr <= 0xDF9F )
	  || ( addr >= 0xDFA0 && addr <= 0xDFA3 )
	  || ( addr >= 0xDFC0 && addr <= 0xDFC3 ) )
	{
		value = 0xFF;
		return true;
	}

	return false;
}

bool CRAMLink::write( u16 addr, u8 value )
{
	if ( !m_Fitted && !m_ROMLength )
		return false;

	// The on/off strobes first, and unconditionally: they are the only thing
	// decoded while the unit is off, so they cannot be gated on being on.
	if ( addr == RAMLINK_REG_ON )
	{
		m_RegWrites++;
		m_On = true;
		return true;
	}
	if ( addr == RAMLINK_REG_OFF )
	{
		m_RegWrites++;
		if ( m_On )
		{
			m_On = false;
			m_DOSMapped = false;
			m_REUTrap = false;
		}
		return true;
	}

	// The window -- see read(). Same decode, same reason.
	if ( addr >= RAMLINK_IO1_FIRST && addr <= RAMLINK_IO1_LAST )
	{
		if ( m_IO1Mode == RAMLINK_IO1_RL_RAM )
		{
			m_RAM[ ( m_RAMBase | ( addr & 0xFF ) ) & ( RAMLINK_RAM_SIZE - 1 ) ] = value;
			return true;
		}
		if ( m_IO1Mode == RAMLINK_IO1_RAMCARD && m_Card )
		{
			if ( m_CardBase < 0 )
				return true;				// decoded, but nothing latches it
			m_Card[ ( (u32)m_CardBase | ( addr & 0xFF ) ) % m_CardSize ] = value;
			m_CardDirty = true;
			return true;
		}
		return true;						// decoded, nothing behind it
	}

	if ( !m_On )
		return false;

	m_RegWrites++;

	if ( addr >= 0xDF20 && addr <= 0xDF22 )
	{
		switch ( addr & 3 )
		{
		case 0: m_REUTrap = false; break;
		case 2: m_REUTrap = true;  break;
		case 1:
			// Forwards to the REU's command register, which is what makes a
			// transfer start while RAMLink owns I/O2.
			if ( m_REUTrap && m_REU ) m_REU->ramlinkREUCommand( value );
			break;
		}
		return true;
	}

	if ( addr >= 0xDF40 && addr <= 0xDF43 )
	{
		const u8 port = (u8)( addr & 3 );
		if ( port == 3 )
		{
			m_PPIControl = value;
			// Bit 7 clear is the i8255A's bit set/reset command for port C
			// rather than a mode word.
			if ( !( value & 0x80 ) )
			{
				const u8 bit = (u8)( ( value >> 1 ) & 7 );
				if ( value & 1 ) m_PPIOut[ 2 ] |= (u8)( 1 << bit );
				else             m_PPIOut[ 2 ] &= (u8)~( 1 << bit );
				m_ROMBase = (u32)( m_PPIOut[ 2 ] & 3 ) * 0x4000;
			}
			return true;
		}
		m_PPIOut[ port ] = value;
		// Port C bits 1-0 are the DOS ROM's bank select lines.
		if ( port == 2 )
			m_ROMBase = (u32)( value & 3 ) * 0x4000;
		return true;
	}

	if ( addr == 0xDF60 ) { m_DOSMapped = true;  return true; }
	if ( addr == 0xDF70 ) { m_DOSMapped = false; return true; }

	if ( addr >= 0xDF80 && addr <= 0xDF9F )
	{
		m_RAMBase = (u16)( ( addr & 0x1F ) << 8 );
		return true;
	}

	if ( addr >= 0xDFA0 && addr <= 0xDFA3 )
	{
		// The latch is byte 1, 2 and 3 of a 32-bit address; byte 0 comes from
		// the low 8 bits of the $DE00 access itself.
		switch ( addr & 3 )
		{
		case 0: m_CardAddr = ( m_CardAddr & 0xFFFF0000u ) | ( (u32)value <<  8 ); break;
		case 1: m_CardAddr = ( m_CardAddr & 0xFF00FF00u ) | ( (u32)value << 16 ); break;
		case 2: m_CardAddr = ( m_CardAddr & 0x00FFFF00u ) | ( (u32)value << 24 ); break;
		default: return true;				// $DFA3 decodes but does nothing
		}
		recomputeCardBase();
		return true;
	}

	if ( addr >= 0xDFB0 && addr <= 0xDFBF )
	{
		rtcWrite( (u8)( addr & 15 ), value );
		return true;
	}

	if ( addr >= 0xDFC0 && addr <= 0xDFC3 )
	{
		m_IO1Mode = (u8)( addr & 3 );
		return true;
	}

	m_RegWrites--;							// not ours after all
	return false;
}
