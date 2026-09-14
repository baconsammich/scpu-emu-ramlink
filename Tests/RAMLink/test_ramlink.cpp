/*
   SCPU-EMU - CMD RAMLink tests.

   The register map and every behaviour asserted here comes from VICE's
   ramlink.c; see Source/RAMLink/ramlink.h for the reasoning and for what is
   deliberately out of scope.

   The cases that matter most are the ones where a plausible implementation is
   wrong in a way nothing else catches: the on/off gating (because $DF7E and
   $DF7F must stay decoded when everything else is off, or there is no way back
   on), and the RAMCard size decode (because that is how software discovers how
   much is fitted, and a card that aliases where it should read open bus reports
   the wrong size rather than failing).

   Copyright (c) 2026 SCPU-EMU contributors, GPLv3.
*/
#include "../test_framework.h"
#include "../../Source/RAMLink/ramlink.h"
#include "../../Source/RAMLink/ramlink_wiring.h"

// A clock pinned to a known instant, so the RTC assertions are about the BCD
// encoding rather than about what time it happens to be.
class FixedTestClock : public IRAMLinkClock
{
public:
	void ramlinkNow( u32 &year, u32 &month, u32 &day, u32 &weekday,
	                 u32 &hour, u32 &minute, u32 &second ) override
	{
		year = 1994; month = 7; day = 23; weekday = 6;
		hour = 17; minute = 42; second = 9;
	}
};

// Bring a unit up and switch it on, which is what every test but the gating
// ones wants as a starting point.
static void switchOn( CRAMLink &rl )
{
	CHECK( rl.write( RAMLINK_REG_ON, 0x00 ) );
	CHECK( rl.on() );
}

TEST( ramlink_size_selectors_map_to_the_documented_sizes )
{
	struct { u8 sel; u32 mb; } cases[] = {
		{ RLSIZE_NONE,  0 }, { RLSIZE_1MB,  1 }, { RLSIZE_2MB,  2 },
		{ RLSIZE_4MB,   4 }, { RLSIZE_8MB,  8 }, { RLSIZE_16MB, 16 },
	};
	for ( u32 i = 0; i < sizeof cases / sizeof cases[ 0 ]; i++ )
	{
		CRAMLink rl;
		CHECK( rl.init( cases[ i ].sel ) );
		CHECK_EQ( rl.cardSizeMB(), cases[ i ].mb );
	}
}

TEST( ramlink_unknown_selector_is_absent_not_rounded )
{
	// A mistyped selector must not silently fit hardware -- the same rule the
	// REU follows, and for the same reason.
	CRAMLink rl;
	CHECK( rl.init( 99 ) );
	CHECK_EQ( rl.cardSizeMB(), 0u );
	CHECK( !rl.present() );
}

TEST( ramlink_absent_unit_declines_every_access )
{
	CRAMLink rl;
	rl.init( RLSIZE_NONE );
	u8 v = 0;
	CHECK( !rl.read( 0xDE00, v ) );
	CHECK( !rl.read( 0xDFB0, v ) );
	CHECK( !rl.write( RAMLINK_REG_ON, 0 ) );
	CHECK( !rl.on() );
}

TEST( ramlink_starts_switched_off )
{
	CRAMLink rl;
	rl.init( RLSIZE_4MB );
	CHECK( !rl.on() );
	CHECK( !rl.dosMapped() );
	// The power-on window source is deliberately NOT one of the four decoded
	// modes, so an access before the ROM selects one reads open bus.
	CHECK_EQ( rl.io1Mode(), RAMLINK_IO1_UNSET );
}

TEST( ramlink_on_off_strobes_stay_decoded_while_everything_else_is_off )
{
	// This is the one that would brick the device if it were wrong: with the
	// unit off, $DF7E must still be decoded or there is no way to switch it on.
	CRAMLink rl;
	rl.init( RLSIZE_4MB );

	u8 v = 0;
	// The window is the exception: it decodes whether the unit is on or off,
	// because a RAMLink's io1 range is wired to the port, not to the switch.
	// Everything else is gated.
	CHECK( rl.read( 0xDE00, v ) );
	CHECK( !rl.write( 0xDFC0, 0 ) );		// mode select: off
	CHECK( !rl.write( 0xDF60, 0 ) );		// DOS map: off

	CHECK( rl.write( RAMLINK_REG_ON, 0 ) );	// but this one answers
	CHECK( rl.on() );

	CHECK( rl.write( 0xDFC0, 0 ) );			// and now the rest do too
	CHECK( rl.write( RAMLINK_REG_OFF, 0 ) );
	CHECK( !rl.on() );
	CHECK( !rl.write( 0xDFC0, 0 ) );
}

TEST( ramlink_switching_off_drops_the_dos_mapping )
{
	CRAMLink rl;
	rl.init( RLSIZE_4MB );
	switchOn( rl );
	CHECK( rl.write( 0xDF60, 0 ) );
	CHECK( rl.dosMapped() );
	CHECK( rl.write( 0xDF70, 0 ) );
	CHECK( !rl.dosMapped() );

	CHECK( rl.write( 0xDF60, 0 ) );
	CHECK( rl.dosMapped() );
	CHECK( rl.write( RAMLINK_REG_OFF, 0 ) );
	CHECK( !rl.dosMapped() );
}

TEST( ramlink_internal_ram_powers_up_holding_its_own_page_numbers )
{
	// Not decoration: CMD's ROM scans for the device, and VICE seeds exactly
	// this pattern in config_init.
	CRAMLink rl;
	rl.init( RLSIZE_4MB );
	const u8 *ram = rl.internalRAM();
	CHECK_EQ( ram[ 0x0000 ], 0x00 );
	CHECK_EQ( ram[ 0x00FF ], 0x00 );
	CHECK_EQ( ram[ 0x0100 ], 0x01 );
	CHECK_EQ( ram[ 0x1F00 ], 0x1F );
	CHECK_EQ( ram[ 0x1FFF ], 0x1F );
}

TEST( ramlink_io1_window_reaches_internal_ram_and_pages_with_df80 )
{
	CRAMLink rl;
	rl.init( RLSIZE_4MB );
	switchOn( rl );
	CHECK( rl.write( 0xDFC0 + RAMLINK_IO1_RL_RAM, 0 ) );
	CHECK_EQ( rl.io1Mode(), RAMLINK_IO1_RL_RAM );

	// $DF80-$DF9F select which 256-byte page of the 8K appears at $DE00; the
	// page number is the low five bits of the ADDRESS, not the value written.
	CHECK( rl.write( 0xDF80 + 0x07, 0x00 ) );
	CHECK_EQ( rl.ramBase(), 0x0700 );

	CHECK( rl.write( 0xDE40, 0x5A ) );
	u8 v = 0;
	CHECK( rl.read( 0xDE40, v ) );
	CHECK_EQ( v, 0x5A );
	CHECK_EQ( rl.internalRAM()[ 0x0740 ], 0x5A );

	// A different page is a different byte.
	CHECK( rl.write( 0xDF80 + 0x08, 0x00 ) );
	CHECK( rl.read( 0xDE40, v ) );
	CHECK_EQ( v, 0x08 );					// still the power-on page number
}

TEST( ramlink_io1_window_reaches_the_ramcard_at_the_latched_address )
{
	CRAMLink rl;
	rl.init( RLSIZE_4MB );
	switchOn( rl );
	CHECK( rl.write( 0xDFC0 + RAMLINK_IO1_RAMCARD, 0 ) );

	// $DFA0-$DFA2 latch bytes 1, 2 and 3 of the card address; byte 0 is the low
	// 8 bits of the $DE00 access itself.
	CHECK( rl.write( 0xDFA0, 0x34 ) );		// bits 8-15
	CHECK( rl.write( 0xDFA1, 0x12 ) );		// bits 16-23
	CHECK_EQ( rl.cardAddress(), 0x00123400u );
	CHECK_EQ( rl.cardBase(), 0x00123400 );

	CHECK( rl.write( 0xDE56, 0xC3 ) );
	u8 v = 0;
	CHECK( rl.read( 0xDE56, v ) );
	CHECK_EQ( v, 0xC3 );
	CHECK_EQ( rl.cardRAM()[ 0x123456 ], 0xC3 );
}

TEST( ramlink_ramcard_reads_open_bus_beyond_the_fitted_size )
{
	// The size decode IS the detection mechanism, so the gaps have to be real.
	//
	// Note the size chosen. A FOUR megabyte card has no open bus anywhere: the
	// decode mirrors the fitted 4MB across the whole 64MB space, so every
	// granule answers. Eight megabytes is the smallest size that leaves a
	// genuine hole -- granules 8 through 15 read open before the 16MB pattern
	// repeats. Testing this with a 4MB card asserts the opposite of what the
	// hardware does and passes only against a wrong implementation.
	CRAMLink rl;
	rl.init( RLSIZE_8MB );
	switchOn( rl );
	CHECK( rl.write( 0xDFC0 + RAMLINK_IO1_RAMCARD, 0 ) );

	// 3MB: inside the fitted card.
	CHECK( rl.write( 0xDFA0, 0x00 ) );
	CHECK( rl.write( 0xDFA1, 0x30 ) );
	CHECK( rl.cardBase() >= 0 );

	// 12MB: past the end of an 8MB card, below the 16MB repeat.
	CHECK( rl.write( 0xDFA1, 0xC0 ) );
	CHECK_EQ( rl.cardBase(), -1 );
	u8 v = 0;
	CHECK( !rl.read( 0xDE00, v ) );			// declines: open bus, not ours
	// A write there is decoded but latches nothing, rather than aliasing into
	// fitted memory and silently corrupting it.
	CHECK( rl.write( 0xDE00, 0xFF ) );
	CHECK_EQ( rl.cardRAM()[ 0 ], 0x00 );

	// 20MB is granule 20, which mirrors granule 4 -- fitted again.
	CHECK( rl.write( 0xDFA1, 0x40 ) );
	CHECK( rl.write( 0xDFA2, 0x01 ) );
	CHECK( rl.cardBase() >= 0 );
}

TEST( ramlink_ramcard_below_four_megabytes_mirrors_every_four )
{
	// VICE's decode: at 4MB or less the fitted part repeats every 4MB across
	// the whole 64MB card space, so $000000 and $400000 are the same byte.
	CRAMLink rl;
	rl.init( RLSIZE_2MB );
	switchOn( rl );
	CHECK( rl.write( 0xDFC0 + RAMLINK_IO1_RAMCARD, 0 ) );

	CHECK( rl.write( 0xDFA0, 0x00 ) );
	CHECK( rl.write( 0xDFA1, 0x00 ) );
	CHECK( rl.write( 0xDFA2, 0x00 ) );
	CHECK( rl.write( 0xDE10, 0x77 ) );

	// 4MB up is granule 4, which maps to granule 0 on a 2MB card.
	CHECK( rl.write( 0xDFA1, 0x40 ) );
	u8 v = 0;
	CHECK( rl.read( 0xDE10, v ) );
	CHECK_EQ( v, 0x77 );

	// 2MB up is granule 2, which is unfitted and reads open.
	CHECK( rl.write( 0xDFA1, 0x20 ) );
	CHECK_EQ( rl.cardBase(), -1 );
}

TEST( ramlink_ramcard_contents_survive_a_reset )
{
	// The card is battery-backed. A partition table that did not survive a
	// reset would make the device useless, so this is the defining property.
	CRAMLink rl;
	rl.init( RLSIZE_4MB );
	switchOn( rl );
	CHECK( rl.write( 0xDFC0 + RAMLINK_IO1_RAMCARD, 0 ) );
	CHECK( rl.write( 0xDFA0, 0x00 ) );
	CHECK( rl.write( 0xDE20, 0xAB ) );
	CHECK_EQ( rl.cardRAM()[ 0x0020 ], 0xAB );

	rl.reset();

	CHECK( !rl.on() );						// control state does reset
	CHECK_EQ( rl.cardRAM()[ 0x0020 ], 0xAB );	// contents do not
	// The internal 8K is re-seeded, unlike the card.
	CHECK_EQ( rl.internalRAM()[ 0x0300 ], 0x03 );
}

TEST( ramlink_unsourced_window_still_decodes_and_reads_open_bus )
{
	// Nothing is nested in this RAMLink's RAM-Port, so modes 2 and 3 have
	// nothing to answer WITH -- but the window is still the RAMLink's, and it
	// still claims the access. Same for the power-on source, which is not one
	// of the four decoded modes at all.
	//
	// This was pinned the other way round, and it cost a boot: CMD's KERNAL
	// touches $DE00 before it has written $DFC0, so the source is still the
	// power-on 7. Declining sent those reads to the C64 and the RAMLink KERNAL
	// hung at $FAF6 instead of finishing its init. VICE's ramlink_io1_read()
	// returns 255 for every source it does not recognise, and is right to.
	CRAMLink rl;
	rl.init( RLSIZE_4MB );
	switchOn( rl );
	u8 v = 0;

	CHECK_EQ( rl.io1Mode(), RAMLINK_IO1_UNSET );
	CHECK( rl.read( 0xDE00, v ) );
	CHECK_EQ( v, 0xFF );

	CHECK( rl.write( 0xDFC0 + RAMLINK_IO1_RAMPORT, 0 ) );
	v = 0;
	CHECK( rl.read( 0xDE00, v ) );
	CHECK_EQ( v, 0xFF );
	CHECK( rl.write( 0xDE00, 0x11 ) );

	CHECK( rl.write( 0xDFC0 + RAMLINK_IO1_PASSTHRU, 0 ) );
	v = 0;
	CHECK( rl.read( 0xDE00, v ) );
	CHECK_EQ( v, 0xFF );
}

TEST( ramlink_rom_bank_follows_the_ppi_port_c_low_bits )
{
	CRAMLink rl;
	rl.init( RLSIZE_4MB );
	switchOn( rl );
	CHECK_EQ( rl.romBank(), 0u );
	CHECK( rl.write( 0xDF42, 0x01 ) );		// port C = 1
	CHECK_EQ( rl.romBank(), 0x4000u );
	CHECK( rl.write( 0xDF42, 0x02 ) );
	CHECK_EQ( rl.romBank(), 0x8000u );
	CHECK( rl.write( 0xDF42, 0x00 ) );
	CHECK_EQ( rl.romBank(), 0u );
}

TEST( ramlink_rtc_reports_the_clock_as_bcd_digits )
{
	CRAMLink rl;
	rl.init( RLSIZE_4MB );
	FixedTestClock clock;					// 1994-07-23 17:42:09, Saturday
	rl.attachClock( &clock );
	switchOn( rl );

	// Select 24-hour mode first. The chip powers up in TWELVE-hour mode -- see
	// the note above rtcRead() -- so asserting 24-hour digits without this
	// would be asserting the wrong default.
	CHECK( rl.write( 0xDFBF, 0x04 ) );

	u8 v = 0;
	CHECK( rl.read( 0xDFB0, v ) ); CHECK_EQ( v, 9 );	// seconds, units
	CHECK( rl.read( 0xDFB1, v ) ); CHECK_EQ( v, 0 );	// seconds, tens
	CHECK( rl.read( 0xDFB2, v ) ); CHECK_EQ( v, 2 );	// minutes, units
	CHECK( rl.read( 0xDFB3, v ) ); CHECK_EQ( v, 4 );	// minutes, tens
	CHECK( rl.read( 0xDFB4, v ) ); CHECK_EQ( v, 7 );	// hours, units
	CHECK( rl.read( 0xDFB5, v ) ); CHECK_EQ( v, 1 );	// hours, tens
	CHECK( rl.read( 0xDFB6, v ) ); CHECK_EQ( v, 3 );	// day, units
	CHECK( rl.read( 0xDFB7, v ) ); CHECK_EQ( v, 2 );	// day, tens
	CHECK( rl.read( 0xDFB8, v ) ); CHECK_EQ( v, 7 );	// month, units
	CHECK( rl.read( 0xDFB9, v ) ); CHECK_EQ( v, 0 );	// month, tens
	CHECK( rl.read( 0xDFBA, v ) ); CHECK_EQ( v, 4 );	// year, units (94)
	CHECK( rl.read( 0xDFBB, v ) ); CHECK_EQ( v, 9 );	// year, tens
	CHECK( rl.read( 0xDFBC, v ) ); CHECK_EQ( v, 6 );	// weekday
}

TEST( ramlink_rtc_powers_up_in_twelve_hour_mode )
{
	// 17:42 is 5:42 PM. Both hour digits must come from the same 12-hour value:
	// deriving only the tens digit from it -- and leaving the units as hour%10
	// -- yields "7" with the PM flag set, which is not a time.
	CRAMLink rl;
	rl.init( RLSIZE_4MB );
	FixedTestClock clock;
	rl.attachClock( &clock );
	switchOn( rl );

	u8 v = 0;
	CHECK( rl.read( 0xDFB4, v ) ); CHECK_EQ( v, 5 );		// 5
	CHECK( rl.read( 0xDFB5, v ) ); CHECK_EQ( v, 0x04 );		// tens 0, PM set

	// Selecting 24-hour mode switches both digits together.
	CHECK( rl.write( 0xDFBF, 0x04 ) );
	CHECK( rl.read( 0xDFB4, v ) ); CHECK_EQ( v, 7 );
	CHECK( rl.read( 0xDFB5, v ) ); CHECK_EQ( v, 1 );
}

TEST( ramlink_rtc_register_14_reads_back_so_the_clock_can_be_detected )
{
	// VICE's rtc-72421.c says it outright: "RAMLINK writes/reads data to this
	// register to detect the presence of the rtc". If it does not read back,
	// RAMLink concludes it has no clock.
	CRAMLink rl;
	rl.init( RLSIZE_4MB );
	FixedTestClock clock;
	rl.attachClock( &clock );
	switchOn( rl );

	u8 v = 0;
	CHECK( rl.write( 0xDFBE, 0x0A ) );
	CHECK( rl.read( 0xDFBE, v ) );
	CHECK_EQ( v, 0x0A );
	CHECK( rl.write( 0xDFBE, 0x05 ) );
	CHECK( rl.read( 0xDFBE, v ) );
	CHECK_EQ( v, 0x05 );
}

TEST( ramlink_rtc_control_register_read_reports_mode_and_stop )
{
	// Deliberately asymmetric, and faithfully so: a WRITE takes hour24 from
	// bit 2 and stop from bit 1, but a READ reports hour24 in bit 1 and stop
	// in bit 0. That is what VICE does.
	CRAMLink rl;
	rl.init( RLSIZE_4MB );
	FixedTestClock clock;
	rl.attachClock( &clock );
	switchOn( rl );

	u8 v = 0;
	CHECK( rl.read( 0xDFBF, v ) ); CHECK_EQ( v, 0x00 );		// 12h, running

	CHECK( rl.write( 0xDFBF, 0x04 ) );						// bit 2: 24-hour
	CHECK( rl.read( 0xDFBF, v ) ); CHECK_EQ( v, 0x02 );		// reads at bit 1

	CHECK( rl.write( 0xDFBF, 0x02 ) );						// bit 1: stop
	CHECK( rl.read( 0xDFBF, v ) ); CHECK_EQ( v, 0x01 );		// reads at bit 0

	CHECK( rl.write( 0xDFBF, 0x06 ) );						// both
	CHECK( rl.read( 0xDFBF, v ) ); CHECK_EQ( v, 0x03 );
}

TEST( ramlink_df21_reaches_the_reu_only_while_the_trap_is_armed )
{
	// RAMLink owns I/O2, so a program cannot write $DF01 directly. $DF22 arms
	// the trap and $DF21 then forwards to the REU's command register.
	class CountingREU : public IRAMLinkREU
	{
	public:
		u32 calls = 0; u8 last = 0;
		void ramlinkREUCommand( u8 value ) override { calls++; last = value; }
	} reu;

	CRAMLink rl;
	rl.init( RLSIZE_4MB );
	rl.attachREU( &reu );
	switchOn( rl );

	// Not armed yet: the write is decoded but goes nowhere.
	CHECK( rl.write( 0xDF21, 0x91 ) );
	CHECK_EQ( reu.calls, 0u );

	CHECK( rl.write( 0xDF22, 0x00 ) );		// arm
	CHECK( rl.write( 0xDF21, 0x91 ) );
	CHECK_EQ( reu.calls, 1u );
	CHECK_EQ( reu.last, 0x91 );

	CHECK( rl.write( 0xDF20, 0x00 ) );		// disarm
	CHECK( rl.write( 0xDF21, 0x92 ) );
	CHECK_EQ( reu.calls, 1u );

	// Switching the unit off disarms it too.
	CHECK( rl.write( 0xDF22, 0x00 ) );
	CHECK( rl.write( RAMLINK_REG_OFF, 0x00 ) );
	CHECK( rl.write( RAMLINK_REG_ON, 0x00 ) );
	CHECK( rl.write( 0xDF21, 0x93 ) );
	CHECK_EQ( reu.calls, 1u );
}

TEST( ramlink_ignores_addresses_that_are_not_its_own )
{
	CRAMLink rl;
	rl.init( RLSIZE_4MB );
	switchOn( rl );
	u8 v = 0;
	CHECK( !rl.read( 0xD020, v ) );
	CHECK( !rl.write( 0xD020, 0 ) );
	CHECK( !rl.read( 0xDF00, v ) );			// the REU's own base
	CHECK( !rl.write( 0xDF10, 0 ) );
	CHECK( !rl.read( 0xDFD0, v ) );			// above our last register
}

TEST( ramlink_rom_image_is_exposed_with_its_bank_layout )
{
	// The SuperCPU side reads the image rather than having it banked into
	// $8000-$BFFF -- see the scope note in ramlink.h. Check the accessor and
	// the documented bank offsets, since those are what a consumer indexes by.
	static u8 image[ RAMLINK_ROM_SIZE ];
	for ( u32 i = 0; i < RAMLINK_ROM_SIZE; i++ ) image[ i ] = (u8)( i >> 8 );

	CRAMLink rl;
	rl.init( RLSIZE_4MB );
	CHECK( !rl.hasROM() );
	rl.setROM( image, sizeof image );
	CHECK( rl.hasROM() );
	CHECK_EQ( rl.romLength(), RAMLINK_ROM_SIZE );
	CHECK_EQ( rl.romImage()[ RAMLINK_KERNBASE_64 ], 0x80 );
	CHECK_EQ( rl.romImage()[ RAMLINK_KERNBASE_128 ], 0xC0 );
}

TEST( ramlink_oversized_rom_is_clamped_rather_than_trusted )
{
	static u8 image[ RAMLINK_ROM_SIZE * 2 ];
	CRAMLink rl;
	rl.init( RLSIZE_4MB );
	rl.setROM( image, sizeof image );
	CHECK_EQ( rl.romLength(), (u32)RAMLINK_ROM_SIZE );
}

// --- integration: RAMLink inside the SuperCPU's I/O chain --------------------
#include "../../Source/SuperCPU/registers.h"
#include "../../Source/C64/c64_memory.h"
#include "../../Source/Bus/Host/host_bus.h"
#include "../../Source/REU/reu_wiring.h"

TEST( ramlink_strobe_reaches_both_the_accelerator_and_the_device )
{
	// The single most important wiring property, and the reason
	// CIOInterceptorChain documents its ordering rule.
	//
	// $DF7E has to do two things at once: tell the ACCELERATOR that RAMLink
	// hardware is enabled -- which sets $D0BC bit 6 and pulls the interrupt
	// vector reroute on -- and tell the DEVICE to switch itself on. The
	// accelerator is primary and must see it first; it then returns false so
	// the write continues down the chain to RAMLink. A chain that stopped at
	// the first handler, or one that put RAMLink first, would get one of the
	// two and look like it worked.
	CSuperCPURegisters  regs;
	CRAMLink            link;
	CRAMLinkInterceptor linkIO;
	CIOInterceptorChain chain;

	regs.reset();
	link.init( RLSIZE_8MB );
	linkIO.attach( &link );
	chain.attach( &regs, &linkIO );

	CHECK( !link.on() );
	CHECK( !regs.interruptRerouteRequested() );

	CHECK( chain.ioWrite( RAMLINK_REG_ON, 0x00 ) );

	// The device switched on...
	CHECK( link.on() );
	// ...and so did the accelerator's view of it.
	u8 v = 0;
	CHECK( chain.ioRead( SCPU_REG_DOSEXT, v ) );
	CHECK( ( v & SCPU_DOS_RAMLINK ) != 0 );
	CHECK( regs.interruptRerouteRequested() );

	CHECK( chain.ioWrite( RAMLINK_REG_OFF, 0x00 ) );
	CHECK( !link.on() );
	CHECK( chain.ioRead( SCPU_REG_DOSEXT, v ) );
	CHECK( ( v & SCPU_DOS_RAMLINK ) == 0 );
	CHECK( !regs.interruptRerouteRequested() );
}

TEST( ramlink_chain_leaves_the_accelerators_own_registers_alone )
{
	// RAMLink watches a lot of $DFxx. It must not shadow anything the
	// accelerator owns, and the accelerator's $D0Bx/$D07x block must still
	// answer first.
	CSuperCPURegisters  regs;
	CRAMLink            link;
	CRAMLinkInterceptor linkIO;
	CIOInterceptorChain chain;

	regs.reset();
	link.init( RLSIZE_8MB );
	linkIO.attach( &link );
	chain.attach( &regs, &linkIO );
	CHECK( chain.ioWrite( RAMLINK_REG_ON, 0x00 ) );

	u8 v = 0;
	CHECK( chain.ioRead( SCPU_REG_VERSION, v ) );	// $D0B0: the accelerator's
	CHECK( chain.ioWrite( SCPU_REG_HWREGS_ENABLE, 0 ) );
	CHECK( regs.hardwareRegsEnabled() );
	CHECK( chain.ioRead( SCPU_REG_STATUS, v ) );
	CHECK( ( v & SCPU_STATUS_HWREGS ) != 0 );
}

TEST( ramlink_and_the_reu_coexist_in_one_chain )
{
	// A real RAMLink hosts the REU in its RAM-Port and claims I/O2 ahead of
	// it, which is exactly the order the chain uses. The REU's own registers
	// at $DF00-$DF0A are outside every range RAMLink watches, so both remain
	// reachable -- this pins that they do not collide.
	CSuperCPURegisters  regs;
	CRAMLink            link;
	CRAMLinkInterceptor linkIO;
	CREU                reu;
	CREUInterceptor     reuIO;
	CIOInterceptorChain chain;

	regs.reset();
	link.init( RLSIZE_8MB );
	linkIO.attach( &link );
	reu.init( REUSIZE_512K );
	reuIO.attach( &reu );
	chain.attach( &regs, &linkIO, &reuIO );

	CHECK( chain.ioWrite( RAMLINK_REG_ON, 0x00 ) );
	CHECK( link.on() );

	// The REU still answers its own register file through the chain.
	u8 v = 0;
	CHECK( chain.ioRead( 0xDF00, v ) );			// REU status
	CHECK( chain.ioWrite( 0xDF02, 0x34 ) );		// C64 base low
	CHECK( chain.ioRead( 0xDF02, v ) );
	CHECK_EQ( v, 0x34 );

	// And RAMLink still answers its own window.
	CHECK( chain.ioWrite( 0xDFC0 + RAMLINK_IO1_RL_RAM, 0 ) );
	CHECK( chain.ioWrite( 0xDF80 + 0x02, 0 ) );
	CHECK( chain.ioWrite( 0xDE00, 0x99 ) );
	CHECK( chain.ioRead( 0xDE00, v ) );
	CHECK_EQ( v, 0x99 );
}

// --- RAMCard images ---------------------------------------------------------
// The fork's reason for existing: a Pi has no battery, so without an image a
// partitioned card comes up blank on every cold start.

TEST( ramlink_card_image_loads_into_the_card )
{
	CRAMLink rl;
	rl.init( RLSIZE_1MB );

	static u8 image[ 1 << 20 ];
	for ( u32 i = 0; i < sizeof image; i++ ) image[ i ] = (u8)( i * 7 );

	CHECK_EQ( rl.loadCardImage( image, sizeof image ), (u32)( 1 << 20 ) );
	CHECK_EQ( rl.cardRAM()[ 0 ], image[ 0 ] );
	CHECK_EQ( rl.cardRAM()[ 0x4321 ], image[ 0x4321 ] );
	CHECK_EQ( rl.cardRAM()[ ( 1 << 20 ) - 1 ], image[ ( 1 << 20 ) - 1 ] );
}

TEST( ramlink_short_card_image_fills_the_front_and_leaves_the_rest )
{
	// A 64K image on a 1MB card: the partition table is at the front and the
	// card simply has unformatted space after it.
	CRAMLink rl;
	rl.init( RLSIZE_1MB );

	static u8 image[ 0x10000 ];
	for ( u32 i = 0; i < sizeof image; i++ ) image[ i ] = 0xA5;

	CHECK_EQ( rl.loadCardImage( image, sizeof image ), (u32)0x10000 );
	CHECK_EQ( rl.cardRAM()[ 0x0000 ], 0xA5 );
	CHECK_EQ( rl.cardRAM()[ 0xFFFF ], 0xA5 );
	CHECK_EQ( rl.cardRAM()[ 0x10000 ], 0x00 );		// untouched
}

TEST( ramlink_oversized_card_image_is_truncated_not_refused )
{
	// An 8MB image on a 1MB card yields a card holding the first 1MB, which
	// CMD's own tools can still read a partition table out of. Refusing it
	// outright would leave a blank card, which is strictly worse.
	CRAMLink rl;
	rl.init( RLSIZE_1MB );

	static u8 image[ 2 << 20 ];
	for ( u32 i = 0; i < sizeof image; i++ ) image[ i ] = (u8)( i >> 16 );

	CHECK_EQ( rl.loadCardImage( image, sizeof image ), (u32)( 1 << 20 ) );
	CHECK_EQ( rl.cardRAM()[ 0 ], 0x00 );
	CHECK_EQ( rl.cardRAM()[ ( 1 << 20 ) - 1 ], 0x0F );
}

TEST( ramlink_card_image_survives_refitting_the_same_card )
{
	// THE property the boot path depends on. The image is loaded while the SD
	// card is still mounted, which is before CSuperCPU::init() runs -- and
	// init() fits the card again. If that reallocated and zeroed, the image
	// would be gone by the time the machine started, and the only symptom
	// would be a blank RAMCard that the user blames on their image file.
	CRAMLink rl;
	rl.init( RLSIZE_8MB );

	static u8 image[ 0x1000 ];
	for ( u32 i = 0; i < sizeof image; i++ ) image[ i ] = 0x5A;
	rl.loadCardImage( image, sizeof image );
	CHECK_EQ( rl.cardRAM()[ 0x0800 ], 0x5A );

	rl.init( RLSIZE_8MB );					// the same card, again
	CHECK_EQ( rl.cardSizeMB(), 8u );
	CHECK_EQ( rl.cardRAM()[ 0x0800 ], 0x5A );

	// A DIFFERENT size is a different card, and is cleared.
	rl.init( RLSIZE_4MB );
	CHECK_EQ( rl.cardSizeMB(), 4u );
	CHECK_EQ( rl.cardRAM()[ 0x0800 ], 0x00 );
}

TEST( ramlink_card_starts_clean_and_a_guest_write_dirties_it )
{
	// The dirty flag decides whether a button press rewrites 8MB to the SD
	// card. Getting it stuck ON means every session rewrites an image that did
	// not change -- slow, and a chance to corrupt it for nothing. Getting it
	// stuck OFF means writes are silently lost at power down, which is worse.
	CRAMLink rl;
	rl.init( RLSIZE_4MB );
	switchOn( rl );
	CHECK( !rl.cardDirty() );

	CHECK( rl.write( 0xDFC0 + RAMLINK_IO1_RAMCARD, 0 ) );
	CHECK( rl.write( 0xDFA0, 0x00 ) );
	CHECK( rl.write( 0xDFA1, 0x00 ) );
	CHECK( !rl.cardDirty() );			// addressing is not writing

	u8 v = 0;
	CHECK( rl.read( 0xDE10, v ) );
	CHECK( !rl.cardDirty() );			// nor is reading

	CHECK( rl.write( 0xDE10, 0x5A ) );
	CHECK( rl.cardDirty() );
}

TEST( ramlink_internal_ram_writes_do_not_dirty_the_card )
{
	// $DE00 in RL-RAM mode is the unit's own 64K scratch, not the RAMCard, and
	// only the card is persisted. CMD's DOS writes the internal RAM constantly
	// -- treating that as a card change would make the flag meaningless.
	CRAMLink rl;
	rl.init( RLSIZE_4MB );
	switchOn( rl );
	CHECK( rl.write( 0xDFC0 + RAMLINK_IO1_RL_RAM, 0 ) );

	CHECK( rl.write( 0xDE10, 0x5A ) );
	CHECK( !rl.cardDirty() );
}

TEST( ramlink_loading_an_image_marks_the_card_clean )
{
	// A freshly loaded card holds exactly what is on disk. If loading set the
	// flag, the first button press of every session would rewrite the image it
	// had just read.
	static u8 img[ 4096 ];
	for ( u32 i = 0; i < sizeof img; i++ ) img[ i ] = (u8)i;

	CRAMLink rl;
	rl.init( RLSIZE_4MB );
	switchOn( rl );
	CHECK( rl.write( 0xDFC0 + RAMLINK_IO1_RAMCARD, 0 ) );
	CHECK( rl.write( 0xDE10, 0x5A ) );
	CHECK( rl.cardDirty() );

	rl.loadCardImage( img, sizeof img );
	CHECK( !rl.cardDirty() );

	rl.clearCardDirty();
	CHECK( !rl.cardDirty() );
}

TEST( ramlink_dirty_survives_a_reset_but_not_a_resize )
{
	CRAMLink rl;
	rl.init( RLSIZE_4MB );
	switchOn( rl );
	CHECK( rl.write( 0xDFC0 + RAMLINK_IO1_RAMCARD, 0 ) );
	CHECK( rl.write( 0xDE10, 0x5A ) );
	CHECK( rl.cardDirty() );

	// A reset does not reach the card -- it is battery-backed -- so writes
	// still waiting to reach the SD card are still waiting after one.
	rl.reset();
	CHECK( rl.cardDirty() );

	// Re-fitting the SAME size preserves contents, so it must preserve this too.
	rl.init( RLSIZE_4MB );
	CHECK( rl.cardDirty() );

	// A DIFFERENT size is a different card. Whatever was written belongs to a
	// card that no longer exists, and persisting it would write the wrong
	// contents to the image.
	rl.init( RLSIZE_8MB );
	CHECK( !rl.cardDirty() );
}

TEST( ramlink_rewriting_the_same_bytes_is_not_a_change )
{
	// The distinction the whole persistence guard rests on, and it is not
	// hypothetical. Measured on the host: booting CMD's DOS with a RAMLink
	// fitted TOUCHES the card and changes ZERO of its 8388608 bytes -- it
	// writes back values that were already there. Guarding a save on "was it
	// touched" would therefore rewrite 8MB on every session, which is slow and
	// is a chance to damage the image for nothing.
	static u8 img[ 4096 ];
	for ( u32 i = 0; i < sizeof img; i++ ) img[ i ] = (u8)( i * 7 );

	CRAMLink rl;
	rl.init( RLSIZE_4MB );
	switchOn( rl );
	rl.loadCardImage( img, sizeof img );
	CHECK( rl.write( 0xDFC0 + RAMLINK_IO1_RAMCARD, 0 ) );
	CHECK( rl.write( 0xDFA0, 0x00 ) );
	CHECK( rl.write( 0xDFA1, 0x00 ) );

	CHECK( !rl.cardDirty() );
	CHECK( !rl.cardChanged() );

	// Write a byte its own value back. Touched, but nothing has changed.
	const u8 same = rl.cardRAM()[ 0x10 ];
	CHECK( rl.write( 0xDE10, same ) );
	CHECK( rl.cardDirty() );
	CHECK( !rl.cardChanged() );

	// Now write a different one.
	CHECK( rl.write( 0xDE10, (u8)( same ^ 0xFF ) ) );
	CHECK( rl.cardDirty() );
	CHECK( rl.cardChanged() );

	// Putting it back makes the card equal to the image again, and there is
	// then genuinely nothing to save.
	CHECK( rl.write( 0xDE10, same ) );
	CHECK( !rl.cardChanged() );
}

TEST( ramlink_a_saved_card_becomes_the_new_baseline )
{
	static u8 img[ 4096 ];
	for ( u32 i = 0; i < sizeof img; i++ ) img[ i ] = (u8)i;

	CRAMLink rl;
	rl.init( RLSIZE_4MB );
	switchOn( rl );
	rl.loadCardImage( img, sizeof img );
	CHECK( rl.write( 0xDFC0 + RAMLINK_IO1_RAMCARD, 0 ) );
	CHECK( rl.write( 0xDFA0, 0x00 ) );
	CHECK( rl.write( 0xDFA1, 0x00 ) );

	CHECK( rl.write( 0xDE20, 0x99 ) );
	CHECK( rl.cardChanged() );

	// After a successful save the image on disk matches the card, so a second
	// button press must not write it again.
	rl.noteCardSaved();
	CHECK( !rl.cardChanged() );
	CHECK( !rl.cardDirty() );

	// And a further change is still caught.
	CHECK( rl.write( 0xDE21, 0x77 ) );
	CHECK( rl.cardChanged() );
}

TEST( ramlink_a_blank_card_that_gets_written_counts_as_changed )
{
	// Formatting a blank card from RAMLink's own tools and then saving it is a
	// perfectly good way to create ramlink.img in the first place, so "blank"
	// has to be a baseline the detector measures against rather than a case it
	// cannot see.
	CRAMLink rl;
	rl.init( RLSIZE_4MB );
	switchOn( rl );
	CHECK( rl.write( 0xDFC0 + RAMLINK_IO1_RAMCARD, 0 ) );
	CHECK( rl.write( 0xDFA0, 0x00 ) );
	CHECK( rl.write( 0xDFA1, 0x00 ) );
	CHECK( !rl.cardChanged() );

	CHECK( rl.write( 0xDE00, 0x01 ) );
	CHECK( rl.cardChanged() );
}

TEST( ramlink_card_image_is_reachable_through_the_window )
{
	// End to end: an image loaded before the machine starts is what the guest
	// actually reads through $DE00.
	CRAMLink rl;
	rl.init( RLSIZE_1MB );

	static u8 image[ 0x2000 ];
	for ( u32 i = 0; i < sizeof image; i++ ) image[ i ] = (u8)( i ^ 0x3C );
	rl.loadCardImage( image, sizeof image );

	switchOn( rl );
	CHECK( rl.write( 0xDFC0 + RAMLINK_IO1_RAMCARD, 0 ) );
	CHECK( rl.write( 0xDFA0, 0x11 ) );		// card address $001100
	CHECK( rl.write( 0xDFA1, 0x00 ) );

	u8 v = 0;
	CHECK( rl.read( 0xDE22, v ) );
	CHECK_EQ( v, image[ 0x1122 ] );
}

TEST( ramlink_loading_an_image_with_no_card_fitted_does_nothing )
{
	CRAMLink rl;
	rl.init( RLSIZE_NONE );
	static u8 image[ 16 ] = { 1, 2, 3 };
	CHECK_EQ( rl.loadCardImage( image, sizeof image ), 0u );
}

// --- cartridge KERNAL substitution ------------------------------------------
// RAMLink carries two C64 KERNAL images and serves $E000-$FFFF from them.
// Confirmed by inspection of a 2.01 image: $8000-$9FFF is the RAMLink-ON
// image (5.6% identical to stock, correct vectors $FCE2/$FE43/$FF48) and
// $A000-$BFFF the OFF image (97.4% stock plus JiffyDOS patches). The rule is
// VICE's ramlink_romh_read().

static void fillKernalImages( u8 *rom )
{
	for ( u32 i = 0; i < RAMLINK_ROM_SIZE; i++ ) rom[ i ] = 0x00;
	// Distinguishable fill: ON image $A1, OFF image $B2.
	for ( u32 i = 0; i < 0x2000; i++ )
	{
		rom[ 0x8000 + i ] = 0xA1;
		rom[ 0xA000 + i ] = 0xB2;
	}
}

TEST( ramlink_kernal_substitution_is_off_without_a_rom )
{
	CC64Memory mem;
	CHostBus bus;
	mem.attachBus( &bus );
	CHECK( !mem.cartridgeKernalFitted() );
	u8 v = 0;
	CHECK( !mem.cartridgeKernalRead( 0xE000, v ) );
}

TEST( ramlink_kernal_substitution_serves_the_on_image_while_switched_on )
{
	static u8 rom[ RAMLINK_ROM_SIZE ];
	fillKernalImages( rom );
	bool on = true;

	CC64Memory mem;
	CHostBus bus;
	mem.attachBus( &bus );
	mem.setCartridgeKernal( rom, &on );
	mem.m_CartKernalMapped = true;			// an unaccelerated C64
	CHECK( mem.cartridgeKernalFitted() );

	u8 v = 0;
	CHECK( mem.cartridgeKernalRead( 0xE000, v ) ); CHECK_EQ( v, 0xA1 );
	CHECK( mem.cartridgeKernalRead( 0xEFFF, v ) ); CHECK_EQ( v, 0xA1 );
	CHECK( mem.cartridgeKernalRead( 0xFEFF, v ) ); CHECK_EQ( v, 0xA1 );

	// Below $E000 is never ours.
	CHECK( !mem.cartridgeKernalRead( 0xDFFF, v ) );
}

TEST( ramlink_kernal_substitution_serves_the_holes_from_the_off_image )
{
	// $FF00-$FF0F and $FFF0-$FFFF are excluded from the ON image, and they are
	// excluded so they can hold the bank-switch trampoline: diffing a 2.01
	// image against the stock KERNAL shows RAMLink patches EXACTLY the 16 bytes
	// at $FF00-$FF0F. Code that switches banks has to survive the switch, so it
	// must read identically either way -- which means the holes come from the
	// OFF image, NOT from the machine's own KERNAL.
	//
	// This is the case an earlier version got wrong: it declined in the holes,
	// which silently replaced RAMLink's trampoline with whatever the host
	// KERNAL had at that address.
	static u8 rom[ RAMLINK_ROM_SIZE ];
	fillKernalImages( rom );
	bool on = true;

	CC64Memory mem;
	CHostBus bus;
	mem.attachBus( &bus );
	mem.setCartridgeKernal( rom, &on );
	mem.reset();					// $01 comes up with the KERNAL banked in

	// An unaccelerated C64: this window really is the cartridge's.
	mem.m_CartKernalMapped = true;

	u8 v = 0;
	// Outside the holes: the ON image.
	CHECK( mem.cartridgeKernalRead( 0xE000, v ) ); CHECK_EQ( v, 0xA1 );
	CHECK( mem.cartridgeKernalRead( 0xFEFF, v ) ); CHECK_EQ( v, 0xA1 );
	CHECK( mem.cartridgeKernalRead( 0xFF10, v ) ); CHECK_EQ( v, 0xA1 );

	// Inside them: the OFF image, so the trampoline reads the same whichever
	// bank is selected.
	CHECK( mem.cartridgeKernalRead( 0xFF00, v ) ); CHECK_EQ( v, 0xB2 );
	CHECK( mem.cartridgeKernalRead( 0xFF0F, v ) ); CHECK_EQ( v, 0xB2 );
	CHECK( mem.cartridgeKernalRead( 0xFFF0, v ) ); CHECK_EQ( v, 0xB2 );
	CHECK( mem.cartridgeKernalRead( 0xFFFC, v ) ); CHECK_EQ( v, 0xB2 );
}

TEST( ramlink_kernal_never_displaces_an_accelerators_own_kernal )
{
	// The default on a SuperCPU, and it holds whether RAMLink is on or off.
	// VICE's scpu64 config table has no cartridge-ROMH entry for $E000-$FFFF
	// in any configuration with the boot ROM unmapped, so the accelerator's
	// KERNAL shadow owns that window outright.
	//
	// The ON case is the one that bites. CMD's ALT KERNAL switches RAMLink on
	// and then immediately executes the next instruction out of that same
	// window:
	//
	//     $FE70  STA $DF7E / $FE73  JSR $FE98 / $FE76  STA $DF7F
	//
	// Serving the cartridge there means the JSR is fetched from RAMLink's ROM
	// and the machine goes somewhere else -- measured as a boot arriving at
	// $F9D2 where VICE arrives at $FE98.
	static u8 rom[ RAMLINK_ROM_SIZE ];
	fillKernalImages( rom );
	bool on = false;

	CC64Memory mem;
	CHostBus bus;
	mem.attachBus( &bus );
	mem.setCartridgeKernal( rom, &on );
	mem.reset();

	u8 v = 0;
	CHECK( !mem.m_CartKernalMapped );
	CHECK( !mem.cartridgeKernalRead( 0xE000, v ) );
	CHECK( !mem.cartridgeKernalRead( 0xFEFF, v ) );

	on = true;
	CHECK( !mem.cartridgeKernalRead( 0xE000, v ) );
	CHECK( !mem.cartridgeKernalRead( 0xFE73, v ) );

	// Except through the DOS extension's Ultimax overlay, which is how CMD's
	// boot code reads the RAMLink's signature byte in the first place.
	CHECK( mem.cartridgeKernalRead( 0xE000, v, true ) ); CHECK_EQ( v, 0xA1 );
}

TEST( ramlink_kernal_substitution_serves_the_off_image_when_enabled )
{
	// The unaccelerated-C64 rule, kept implemented and tested because it is
	// VICE's actual behaviour and is what a non-SuperCPU build would need.
	static u8 rom[ RAMLINK_ROM_SIZE ];
	fillKernalImages( rom );
	bool on = false;

	CC64Memory mem;
	CHostBus bus;
	mem.attachBus( &bus );
	mem.setCartridgeKernal( rom, &on );
	mem.m_CartKernalMapped = true;
	mem.reset();					// $01 comes up with the KERNAL banked in

	u8 v = 0;
	CHECK( mem.cartridgeKernalRead( 0xE000, v ) ); CHECK_EQ( v, 0xB2 );
	// And a hole falls through to the OFF image rather than passing through,
	// which is what VICE's if/else-if chain does.
	on = true;
	CHECK( mem.cartridgeKernalRead( 0xFFFC, v ) ); CHECK_EQ( v, 0xB2 );
	CHECK( mem.cartridgeKernalRead( 0xE000, v ) ); CHECK_EQ( v, 0xA1 );
}

TEST( ramlink_window_is_reachable_through_the_memory_layer )
{
    // Does a guest READ of $DE00 actually reach the device?
    //
    // Writes were observed reaching it and reads were not, in a whole run of
    // CMD's own firmware -- which is either a real property of the firmware or
    // a hole in the memory layer. It has to be settled, because "the device
    // never sees the read" and "the firmware never issues one" look identical
    // from the outside and have completely different fixes.
    CHostBus            bus;
    CC64Memory          mem;
    CSuperCPURegisters  regs;
    CRAMLink            link;
    CRAMLinkInterceptor linkIO;
    CIOInterceptorChain chain;

    regs.reset();
    link.init( RLSIZE_8MB );
    linkIO.attach( &link );
    chain.attach( &regs, &linkIO );
    mem.attachBus( &bus );
    mem.setIOInterceptor( &chain );
    mem.reset();

    // Switch on and point the window at the internal RAM, through the guest's
    // own memory path rather than by calling the device directly.
    mem.write8( RAMLINK_REG_ON, 0x00 );
    CHECK( link.on() );
    mem.write8( 0xDFC0 + RAMLINK_IO1_RL_RAM, 0x00 );
    mem.write8( 0xDF80 + 0x03, 0x00 );        // page 3
    CHECK_EQ( link.ramBase(), 0x0300 );

    // Write through memory, read back through memory.
    mem.write8( 0xDE44, 0x5A );
    CHECK_EQ( link.internalRAM()[ 0x0344 ], 0x5A );
    CHECK_EQ( mem.read8( 0xDE44 ), 0x5A );

    // And the power-on page-number pattern is visible on an untouched byte.
    CHECK_EQ( mem.read8( 0xDE45 ), 0x03 );

    // The RAMCard window too.
    mem.write8( 0xDFC0 + RAMLINK_IO1_RAMCARD, 0x00 );
    mem.write8( 0xDFA0, 0x12 );
    mem.write8( 0xDE01, 0xC3 );
    CHECK_EQ( mem.read8( 0xDE01 ), 0xC3 );
    CHECK_EQ( link.cardRAM()[ 0x1201 ], 0xC3 );
}
