/*
   SCPU-EMU - what does CMD's own ROM ask of the hardware, and do we answer?

   The question "is there anything we are missing in our SuperCPU emulation"
   has been answered so far by reading VICE's implementation -- registers.cpp
   cites it fourteen times. That is a good oracle for semantics, but it says
   nothing about COVERAGE: which registers CMD's firmware actually exercises,
   and whether every one of those accesses is answered by something we modelled
   rather than falling through to a default.

   This uses the ROM as a 128KB test suite without reading a line of it. The
   accelerator's own I/O interceptor already reports, per access, whether it
   handled the access -- ioRead/ioWrite return bool for exactly that. So wrap
   it, run CMD's firmware, and record the verdict for every access.

   The output is a coverage table:

     addr    reads  writes  claimed  values seen
     $D0B3      41       2      yes  C7 47

   An UNCLAIMED access is the interesting one. It means CMD's own code touched
   something we do not model, and whatever it read came from the bus or from a
   default -- which is precisely "something we are missing", named and counted
   rather than guessed at.

   Deliberately NOT a diff against VICE. VICE's monitor tracepoints print to a
   console that a headless run cannot capture, so no reference sequence exists
   to diff against; that was checked before building this. Coverage is what can
   be established honestly here, and it is the half that answers the question.

     scpu_trace [--rom <path>] [--frames N] [--all]
*/
#include "../../Source/Bus/Host/host_bus.h"
#include "../../Source/C64/c64_memory.h"
#include "../../Source/SuperCPU/supercpu.h"
#include "../../Source/SuperCPU/registers.h"
#include "../../Source/Common/rom_paths.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// Logical names, resolved through Source/Common/rom_paths.h so the canonical
// part-number dumps work as well as the SD-card spellings -- the same reason
// the test suite uses it.
static bool loadFile( const char *logical, u8 *dst, u32 expected )
{
	return scpuRomLoad( logical, dst, expected );
}

// Some callers want a literal path (a --rom override), not a logical name.
static bool loadPath( const char *path, u8 *dst, u32 expected )
{
	FILE *f = fopen( path, "rb" );
	if ( !f ) return false;
	const size_t got = fread( dst, 1, expected, f );
	fclose( f );
	return got == expected;
}

// One row of the coverage table. Values are kept as a small set rather than a
// count so a register that only ever reads back one thing is distinguishable
// from one the firmware is actually driving.
struct Coverage
{
	u32 reads, writes;
	u32 claimedReads, claimedWrites;
	u8  values[ 8 ];
	u32 valueCount;

	void noteValue( u8 v )
	{
		for ( u32 i = 0; i < valueCount; i++ ) if ( values[ i ] == v ) return;
		if ( valueCount < 8 ) values[ valueCount++ ] = v;
	}
};

// Records the verdict the real interceptor gives, and forwards everything
// unchanged. It must not alter behaviour: the point is to observe CMD's
// firmware running against the shipping register layer, not against a variant.
class CRecordingInterceptor : public IIOInterceptor
{
public:
	IIOInterceptor *inner = 0;
	Coverage cov[ 0x100 ];			// $D000-$D0FF, the accelerator's window
	Coverage wide[ 4 ];				// $D200-$D3FF, $DE00 and $DF00 buckets
	// Per-address detail for the cartridge I/O pages. The bucket rows answer
	// "was it touched"; this answers "which register, and did we claim it",
	// which is the only form useful for a device that is a register file.
	Coverage cart[ 0x200 ];			// $DE00-$DFFF
	Coverage sysram[ 0x100 ];		// $D200-$D2FF, the accelerator's scratch

	// A trace of cartridge I/O with the PC that caused it. "Which code touches
	// RAMLink, and when" is not answerable from a coverage table, and it is the
	// question that decides whether the KERNAL substitution can fire during
	// ordinary disk access.
	struct CartAccess { u32 pc; u16 addr; u8 value; bool write, claimed; };
	static const u32 CART_LOG_MAX = 4096;
	CartAccess cartLog[ CART_LOG_MAX ];
	u32 cartLogCount = 0;
	ICpu *cpu = 0;

	void noteCart( u16 addr, u8 value, bool write, bool claimed )
	{
		if ( cartLogCount >= CART_LOG_MAX ) return;
		CartAccess &a = cartLog[ cartLogCount++ ];
		a.pc = cpu ? (u32)cpu->pc() : 0;
		a.addr = addr; a.value = value; a.write = write; a.claimed = claimed;
	}
	u64 totalReads = 0, totalWrites = 0;

	void reset()
	{
		memset( cov, 0, sizeof cov );
		memset( wide, 0, sizeof wide );
		memset( cart, 0, sizeof cart );
		memset( sysram, 0, sizeof sysram );
		cartLogCount = 0;
		totalReads = totalWrites = 0;
	}

	bool ioRead( u16 addr, u8 &value ) override
	{
		const bool claimed = inner ? inner->ioRead( addr, value ) : false;
		if ( addr >= 0xDE00 )
		{
			Coverage &d = cart[ addr - 0xDE00 ];
			d.reads++; if ( claimed ) { d.claimedReads++; d.noteValue( value ); }
			noteCart( addr, value, false, claimed );
		}
		if ( addr >= 0xD200 && addr <= 0xD2FF )
		{
			Coverage &d = sysram[ addr - 0xD200 ];
			d.reads++; if ( claimed ) { d.claimedReads++; d.noteValue( value ); }
		}
		Coverage *c = slot( addr );
		if ( c )
		{
			c->reads++;
			if ( claimed ) { c->claimedReads++; c->noteValue( value ); }
			totalReads++;
		}
		return claimed;
	}

	bool ioWrite( u16 addr, u8 value ) override
	{
		const bool claimed = inner ? inner->ioWrite( addr, value ) : false;
		if ( addr >= 0xDE00 )
		{
			Coverage &d = cart[ addr - 0xDE00 ];
			d.writes++; d.noteValue( value ); if ( claimed ) d.claimedWrites++;
			noteCart( addr, value, true, claimed );
		}
		if ( addr >= 0xD200 && addr <= 0xD2FF )
		{
			Coverage &d = sysram[ addr - 0xD200 ];
			d.writes++; d.noteValue( value ); if ( claimed ) d.claimedWrites++;
		}
		Coverage *c = slot( addr );
		if ( c )
		{
			c->writes++;
			c->noteValue( value );
			if ( claimed ) c->claimedWrites++;
			totalWrites++;
		}
		return claimed;
	}

	// Everything below is pure forwarding. Changing any of it would change the
	// machine being observed.
	bool ioAccessNeedsStretch( u16 a, bool w ) const override
		{ return inner ? inner->ioAccessNeedsStretch( a, w ) : w; }
	bool ioAccessUsesWriteBuffer( u16 a, bool w ) const override
		{ return inner ? inner->ioAccessUsesWriteBuffer( a, w ) : false; }
	bool dosExtensionEnabled() const override
		{ return inner ? inner->dosExtensionEnabled() : false; }
	// EVERY query has to be forwarded, not just the ones this tool reads.
	// The memory layer caches these two POINTERS once, at setIOInterceptor();
	// a wrapper that inherits the default null implementation does not make
	// the DOS extension look disabled to this tool -- it makes it disabled in
	// the machine under test, silently, for the whole run. That is how the
	// $E0AB RAMLink probe came to read the boot ROM instead of the cartridge.
	const bool *dosExtensionStatePtr() const override
		{ return inner ? inner->dosExtensionStatePtr() : 0; }
	const bool *hardwareRegsStatePtr() const override
		{ return inner ? inner->hardwareRegsStatePtr() : 0; }
	bool simmWindowWritesEnabled() const override
		{ return inner ? inner->simmWindowWritesEnabled() : true; }
	bool interruptRerouteRequested() const override
		{ return inner ? inner->interruptRerouteRequested() : false; }

	// Is this address one the ACCELERATOR is supposed to answer?
	//
	// This distinction is the whole report. $D000-$D03F are the VIC's own
	// registers and $D400-$D41F the SID's: declining those is correct, because
	// they belong to the C64 and must reach it. Flagging them as "not modelled"
	// buries the real signal under sixty-four false positives -- which is
	// exactly what the first run of this tool did.
	//
	// The accelerator's own windows are $D071-$D07F (control strobes),
	// $D0B0-$D0BF (status/optimisation/DOS-extension) and $D200-$D2FF (DOS
	// scratch RAM). $D300-$D3FF and $DF00-$DFFF are watched too: the first is
	// the sysram mirror region, the second carries RAMLink's strobes.
	static bool isAcceleratorWindow( u16 addr )
	{
		return ( addr >= 0xD071 && addr <= 0xD07F )
		    || ( addr >= SCPU_REG_STATUS_FIRST && addr <= SCPU_REG_STATUS_LAST )
		    || ( addr >= SCPU_SYSRAM_BASE
		         && addr < SCPU_SYSRAM_BASE + SCPU_SYSRAM_SIZE );
	}

private:
	Coverage *slot( u16 addr )
	{
		if ( addr >= 0xD000 && addr <= 0xD0FF ) return &cov[ addr - 0xD000 ];
		if ( addr >= 0xD200 && addr <= 0xD2FF ) return &wide[ 0 ];
		if ( addr >= 0xD300 && addr <= 0xD3FF ) return &wide[ 1 ];
		if ( addr >= 0xDF00 && addr <= 0xDFFF ) return &wide[ 2 ];
		if ( addr >= 0xDE00 && addr <= 0xDEFF ) return &wide[ 3 ];
		return 0;
	}
};

static void printRow( const char *label, const Coverage &c )
{
	if ( !c.reads && !c.writes ) return;
	const bool anyClaimed = c.claimedReads || c.claimedWrites;
	const bool allClaimed = c.claimedReads == c.reads
	                     && c.claimedWrites == c.writes;
	printf( "  %-10s %6u %7u   %-9s ", label, c.reads, c.writes,
	        allClaimed ? "yes" : ( anyClaimed ? "PARTIAL" : "NO" ) );
	for ( u32 i = 0; i < c.valueCount; i++ ) printf( "%02X ", c.values[ i ] );
	if ( !allClaimed )
		printf( "  <-- %u read(s), %u write(s) NOT modelled",
		        c.reads - c.claimedReads, c.writes - c.claimedWrites );
	printf( "\n" );
}

int main( int argc, char **argv )
{
	const char *romPath = 0;
	u32 frames = 400;
	bool showAll = false;
	u8  ramLink = RLSIZE_NONE;
	bool openDevice16 = false;
	int  basicLine = 1;				// which canned BASIC line --open16 runs
	bool noBootmap = false;			// boot the machine's own KERNAL, not CMD's
	u32  pcTrace = 0;				// step this many instructions and log PCs
	bool rlKernalAlways = false;	// the unaccelerated-C64 mapping rule
	const char *rlImgPath = 0;		// a configured RAMCard image
	const char *rlSavePath = 0;		// write the card out at the end
	static u8 rlImgOrigBuf[ 16u << 20 ];	// pristine copy, to diff against
	const u8 *rlImgOrig = 0;
	u32  rlImgOrigLen = 0;
	bool kernalFromRL = false;		// run RAMLink's OFF KERNAL as the machine's
	bool rlDosWindows = false;		// map RAMLink's DOS at $8000-$BFFF too
	int  rlFlags = -1;				// force the $D203/$D204 RAMLink flags
	bool forceDosExt = false;		// hold the DOS extension on from reset
	// Full 24-bit, not low-16: under bootmap the same low word appears in the
	// accelerator's ROM (bank $F8) and in bank 0, and matching on 16 bits
	// reports one as the other. That mistake cost a diagnosis.
	long traceFrom = -1;
	u32  cartLogShow = 48;
	u32  runFrames = 240;			// frames given to the canned BASIC line
	u8   reuSel = REUSIZE_NONE;		// an REU in the RAMLink's RAM-Port
	u32  traceLen = 240;
	const char *rlROMPath = "ROMs/ramlink201.bin";
	for ( int i = 1; i < argc; i++ )
	{
		if ( !strcmp( argv[ i ], "--all" ) ) showAll = true;
		else if ( !strcmp( argv[ i ], "--rom" ) && i + 1 < argc ) romPath = argv[ ++i ];
		else if ( !strcmp( argv[ i ], "--frames" ) && i + 1 < argc )
			frames = (u32)atoi( argv[ ++i ] );
		else if ( !strcmp( argv[ i ], "--ramlink" ) && i + 1 < argc )
			ramLink = (u8)atoi( argv[ ++i ] );
		else if ( !strcmp( argv[ i ], "--ramlink-rom" ) && i + 1 < argc )
			rlROMPath = argv[ ++i ];
		else if ( !strcmp( argv[ i ], "--open16" ) ) openDevice16 = true;
		else if ( !strcmp( argv[ i ], "--basic" ) && i + 1 < argc )
			{ basicLine = atoi( argv[ ++i ] ); openDevice16 = true; }
		else if ( !strcmp( argv[ i ], "--no-bootmap" ) ) noBootmap = true;
		else if ( !strcmp( argv[ i ], "--pctrace" ) && i + 1 < argc )
			pcTrace = (u32)atoi( argv[ ++i ] );
		else if ( !strcmp( argv[ i ], "--rl-kernal-always" ) ) rlKernalAlways = true;
		else if ( !strcmp( argv[ i ], "--kernal-from-ramlink" ) ) kernalFromRL = true;
		else if ( !strcmp( argv[ i ], "--rl-dos" ) ) rlDosWindows = true;
		else if ( !strcmp( argv[ i ], "--force-dosext" ) ) forceDosExt = true;
		else if ( !strcmp( argv[ i ], "--rl-flags" ) && i + 1 < argc )
			rlFlags = (int)strtol( argv[ ++i ], 0, 16 );
		else if ( !strcmp( argv[ i ], "--run-frames" ) && i + 1 < argc )
			runFrames = (u32)atoi( argv[ ++i ] );
		else if ( !strcmp( argv[ i ], "--save-card" ) && i + 1 < argc )
			rlSavePath = argv[ ++i ];
		else if ( !strcmp( argv[ i ], "--reu" ) && i + 1 < argc )
			reuSel = (u8)atoi( argv[ ++i ] );
		else if ( !strcmp( argv[ i ], "--cartlog" ) && i + 1 < argc )
			cartLogShow = (u32)atoi( argv[ ++i ] );
		else if ( !strcmp( argv[ i ], "--trace-from" ) && i + 1 < argc )
			traceFrom = strtol( argv[ ++i ], 0, 16 );
		else if ( !strcmp( argv[ i ], "--trace-len" ) && i + 1 < argc )
			traceLen = (u32)atoi( argv[ ++i ] );
		else if ( !strcmp( argv[ i ], "--ramlink-img" ) && i + 1 < argc )
			rlImgPath = argv[ ++i ];
	}

	static u8 kernal[ 8192 ], basic[ 8192 ], chargen[ 4096 ], rom[ 131072 ];
	if ( !loadFile( "kernal.rom", kernal, sizeof kernal )
	  || !loadFile( "basic.rom", basic, sizeof basic ) )
	{
		printf( "need kernal.rom and basic.rom (see ROMs/README.md)\n" );
		return 1;
	}
	loadFile( "chargen.rom", chargen, sizeof chargen );

	// 2.04 is the preferred image; 1.4 is the documented fallback.
	const char *tried = romPath ? romPath : "scpu.rom";
	bool gotROM = romPath ? loadPath( romPath, rom, sizeof rom )
	                      : loadFile( "scpu.rom", rom, sizeof rom );
	if ( !gotROM )
	{
		printf( "no SuperCPU ROM found -- supply ROMs/scpu-dos-2.04.bin\n" );
		return 1;
	}
	printf( "SuperCPU ROM     : %s\n", tried );
	if ( reuSel != REUSIZE_NONE )
		printf( "REU selector     : %u\n", (unsigned)reuSel );

	static CHostBus bus;
	static CSuperCPU scpu;
	static CRecordingInterceptor rec;

	// Substituting per-read and INSTALLING the image as the machine's KERNAL
	// are different things. If the image boots when installed but not when
	// substituted, the fault is in the substitution, not the ROM.
	static u8 rlKernalImg[ 8192 ];
	if ( kernalFromRL )
	{
		FILE *rf = fopen( rlROMPath, "rb" );
		if ( rf )
		{
			static u8 whole[ RAMLINK_ROM_SIZE ];
			if ( fread( whole, 1, sizeof whole, rf ) == sizeof whole )
			{
				memcpy( rlKernalImg, whole + 0xA000, 8192 );
				scpu.setKernalROM( rlKernalImg );
				printf( "KERNAL           : RAMLink OFF image ($A000 slice)\n" );
			}
			fclose( rf );
		}
	}
	else
		scpu.setKernalROM( kernal );
	scpu.setBasicROM( basic );
	scpu.setCharROM( chargen );
	for ( u32 a = 0xDC00; a <= 0xDDFF; a++ ) bus.m_Memory[ a ] = 0xFF;
	bus.m_Memory[ 0xDD00 ] = 0x3F;
	bus.m_Memory[ 0xDD02 ] = 0x3F;
	// The interrupt control registers are NOT pulled high. $FF there has bit 7
	// set, which is a CIA saying "an interrupt occurred"; a real one reads 0
	// with nothing pending. Seeding them high makes the KERNAL take an NMI
	// during reset, and with RAMLink's KERNAL that lands before BASIC's cold
	// start has installed the $0300-$030B vectors -- so the machine warm-starts
	// into JMP ($0302) = $0000 and loops there forever. The stock KERNAL
	// happens to survive the same seeding, which is exactly why this went
	// unnoticed. All four mirrors, since the CIA registers repeat every 16.
	for ( u32 a = 0xDC00; a <= 0xDDFF; a += 0x10 )
	{
		bus.m_Memory[ a + 0x0D ] = 0x00;	// ICR
	}

	scpu.memoryMap().setROM( rom, sizeof rom );
	scpu.setBootmapEnabled( !noBootmap );
	if ( noBootmap ) printf( "bootmap          : OFF (machine boots its own KERNAL)\n" );

	static u8 rlROM[ RAMLINK_ROM_SIZE ];
	if ( ramLink != RLSIZE_NONE )
	{
		if ( loadPath( rlROMPath, rlROM, sizeof rlROM ) )
		{
			scpu.setRAMLinkROM( rlROM, sizeof rlROM );
			printf( "RAMLink ROM      : %s\n", rlROMPath );
		}
		else
			printf( "RAMLink ROM      : (none -- registers only)\n" );
		printf( "RAMLink selector : %u\n", ramLink );
	}

	if ( !scpu.init( &bus, SCPU_CORE_65816, SCPU_SIMM_16MB,
	                 reuSel, ramLink ) )
	{
		printf( "init failed\n" );
		return 1;
	}

	if ( rlKernalAlways )
	{
		scpu.memory().m_CartKernalMapped = true;
		printf( "RAMLink KERNAL   : mapped whenever fitted (unaccelerated rule)\n" );
	}
	if ( rlDosWindows )
	{
		scpu.memory().setCartridgeDOS( scpu.ramLink().dosMappedStatePtr(),
		                               scpu.ramLink().romBaseStatePtr(), true );
		printf( "RAMLink DOS      : $8000-$BFFF mapped when $DF60 selects it\n" );
	}

	// A configured RAMCard. Without one there is no partition table, no device
	// number and no RAM disk -- and DEVICE NOT PRESENT would then be correct
	// rather than a fault. See Docs/RAMLink.md.
	if ( rlImgPath && scpu.ramLink().cardRAM() )
	{
		FILE *f = fopen( rlImgPath, "rb" );
		if ( f )
		{
			static u8 buf[ 16u << 20 ];
			const size_t got = fread( buf, 1, sizeof buf, f );
			fclose( f );
			const u32 took = scpu.ramLink().loadCardImage( buf, (u32)got );
			// Keep what was loaded, so "did the machine change the card, and
			// by how much" is answerable at the end of the run.
			memcpy( rlImgOrigBuf, scpu.ramLink().cardRAM(),
			        scpu.ramLink().cardSizeBytes() );
			rlImgOrig = rlImgOrigBuf;
			rlImgOrigLen = scpu.ramLink().cardSizeBytes();
			printf( "RAMCard image    : %s, %u of %u bytes loaded\n",
			        rlImgPath, took, (unsigned)got );
			// The system partition lives at the TOP of the card, not at a
			// fixed address -- it is card size minus $1000. Reading it at a
			// hard-coded $7FF5E1 only ever worked because every card tried so
			// far was 8MB, and it reports a healthy card for a 16MB one that
			// RAMLink itself cannot find the configuration on.
			const u8 *c = scpu.ramLink().cardRAM();
			const u32 sz = scpu.ramLink().cardSizeBytes();
			if ( sz >= 0x100000 )
			{
				const u32 sys = sz - 0x1000;
				printf( "  system partition at $%06X: device $%02X, disk name \"%.12s\","
				        " first partition \"%.10s\"\n",
				        sys, c[ sys + 0x5E1 ],
				        (const char *)( c + sys + 0x5F0 ),
				        (const char *)( c + sys + 0x805 ) );
			}
		}
		else printf( "RAMCard image    : cannot open %s\n", rlImgPath );
	}

	// Interpose AFTER init, so the machine is fully built and the object being
	// wrapped is the one it actually installed.
	//
	// It must be the INSTALLED CHAIN, not scpu.registers(). Wrapping the
	// register block alone quietly removes the REU and the RAMLink from the
	// machine, and then a run "with a RAMLink fitted" is byte-identical to one
	// without -- which is exactly the false negative this tool produced the
	// first time it was pointed at the question.
	if ( forceDosExt )
	{
		// Diagnostic: hold $D0BC bit 7 on so RL-DOS is visible at $8000-$9FFF
		// when the KERNAL's reset cart check runs. scpu.rom carries a CBM80
		// autostart header at ROM $8000, so if the check can see it the machine
		// should JMP ($8000) into RL-DOS.
		scpu.registers().ioWrite( 0xD07E, 0 );		// open the register bank
		scpu.registers().ioWrite( 0xD0BE, 0 );		// DOS extension on
		printf( "DOS extension    : forced on from reset\n" );
	}

	{
		// What the KERNAL's reset cart check actually compares. It tests
		// $FD0F+X against $8003+X for X=5..1, so a CBM80 header at $8000 makes
		// the machine JMP ($8000). scpu.rom carries one at ROM $8000, and under
		// bootmap that is what $8000-$9FFF should read.
		printf( "\ncart-check operands as the guest sees them:\n" );
		printf( "  $8003-$8008 :" );
		for ( u16 a = 0x8003; a <= 0x8008; a++ )
			printf( " %02X", scpu.memory().read8( a ) );
		printf( "\n  $FD0F-$FD14 :" );
		for ( u16 a = 0xFD0F; a <= 0xFD14; a++ )
			printf( " %02X", scpu.memory().read8( a ) );
		printf( "\n  bootmap active: %s\n",
		        scpu.memory().m_BootmapActive ? "yes" : "no" );
	}

	rec.inner = scpu.memory().ioInterceptor();
	rec.cpu = scpu.cpu();
	scpu.memory().setIOInterceptor( &rec );
	rec.reset();

	// The splash waits on raster interrupts, so give the bus a real raster
	// clock -- the one added for the band replay harness serves this too.
	struct Src { static u64 now( void *ctx ) { return ( (CC64Memory *)ctx )->emuNow(); } };
	bus.setRasterClock( Src::now, &scpu.memory() );

	for ( u32 f = 0; f < frames; f++ ) scpu.runFrame();

	if ( scpu.ramLink().present() && scpu.ramLink().cardRAM() && rlImgOrig )
	{
		const u8 *card = scpu.ramLink().cardRAM();
		const u32 n = scpu.ramLink().cardSizeBytes();
		u32 diff = 0, firstAt = 0xFFFFFFFFu;
		for ( u32 i = 0; i < n && i < rlImgOrigLen; i++ )
			if ( card[ i ] != rlImgOrig[ i ] )
			{
				if ( diff == 0 ) firstAt = i;
				diff++;
			}
		printf( "\nRAMCard after boot: touched=%s changed=%s, %u of %u bytes differ",
		        scpu.ramLink().cardDirty() ? "yes" : "no",
		        scpu.ramLink().cardChanged() ? "yes" : "no", diff, n );
		if ( diff ) printf( " (first at $%06X)", firstAt );
		printf( "\n" );
	}

	// Write the card out, so what the emulator did to it can be compared
	// byte for byte against what a real machine's RAMLink wrote to the same
	// image. That comparison is the only way to check the RAMCard path
	// without owning the hardware.
	if ( rlSavePath && scpu.ramLink().cardRAM() && scpu.ramLink().cardSizeBytes() )
	{
		FILE *f = fopen( rlSavePath, "wb" );
		if ( f )
		{
			fwrite( scpu.ramLink().cardRAM(), 1,
			        scpu.ramLink().cardSizeBytes(), f );
			fclose( f );
			printf( "RAMCard saved to  : %s\n", rlSavePath );
		}
		else printf( "RAMCard save FAILED: %s\n", rlSavePath );
	}

	// --- does device 16 open? ---------------------------------------------
	// The whole point of emulating a RAMLink is that a program can talk to it.
	// Booting to READY proves nothing about that, so drive the KERNAL's own
	// OPEN through a stub and read back what it says. Error 5 is DEVICE NOT
	// PRESENT -- exactly what the hardware reported.
	if ( rlFlags >= 0 )
	{
		// Diagnostic only. CMD's KERNAL gates its RAMLink dispatch on flags in
		// the accelerator's own $D200 scratch -- BIT $D201 / BIT $D204 at $FA57
		// -- and nothing in our boot ever sets them. Forcing them answers
		// whether that really is the gate, without guessing at what should set
		// them.
		u8 *b1 = scpu.memoryMap().m_Bank1;
		b1[ 0xD201 ] = (u8)rlFlags; b1[ 0xD202 ] = (u8)rlFlags;
		b1[ 0xD203 ] = (u8)rlFlags; b1[ 0xD204 ] = (u8)rlFlags;
		printf( "RAMLink flags    : $D201-$D204 forced to $%02X\n", (u8)rlFlags );
	}

	if ( openDevice16 )
	{
		// Drive it the way a user would: put a BASIC line in memory and type
		// RUN. An earlier version poked the CPU's PC at a machine-code stub,
		// which is unsound on a running machine -- the next IRQ returns through
		// RTI to the PC it saved, abandoning the stub. That produced a sampled
		// PC of $EB18, which is the KEYBOARD SCAN routine and byte-identical to
		// the stock KERNAL, i.e. the ordinary interrupt handler and not a hang
		// at all. Going through BASIC keeps the machine's own control flow.
		//
		// Canned BASIC lines. Tokens: OPEN $9F, LOAD $93, CLOSE $A0.
		// The point of the device-8 lines is the SERIAL path: CMD's KERNAL has
		// STA $DF7E sites sitting inside its CIOUT/LISTEN/SECOND code, so an
		// ordinary disk access is what would arm the RAMLink KERNAL swap.
		static const u8 l1[] = { 0x9F, '1', ',', '1', '6', ',', '1', '5' };
		static const u8 l2[] = { 0x93, '"', '$', '"', ',', '8' };
		static const u8 l3[] = { 0x9F, '1', '5', ',', '8', ',', '1', '5' };
		static const u8 l4[] = { 0x93, '"', '$', '"', ',', '1', '6' };
		// SAVE $94. The point of line 5 is the WRITE path: does anything the
		// guest saves actually land in the RAMCard?
		static const u8 l5[] = { 0x94, '"', 'T', '"', ',', '1', '6' };
		const u8 *body = l1; u32 bodyLen = sizeof l1;
		if ( basicLine == 2 ) { body = l2; bodyLen = sizeof l2; }
		if ( basicLine == 3 ) { body = l3; bodyLen = sizeof l3; }
		if ( basicLine == 4 ) { body = l4; bodyLen = sizeof l4; }
		if ( basicLine == 5 ) { body = l5; bodyLen = sizeof l5; }
		static const char *names[] = { "", "OPEN1,16,15", "LOAD\"$\",8",
		                               "OPEN15,8,15", "LOAD\"$\",16",
		                               "SAVE\"T\",16" };
		printf( "\nBASIC line        : %s\n", names[ basicLine ] );

		u8 prog[ 32 ];
		u32 n = 0;
		const u16 lineStart = 0x0801;
		const u16 next = (u16)( lineStart + 4 + bodyLen + 1 );
		prog[ n++ ] = (u8)( next & 0xFF ); prog[ n++ ] = (u8)( next >> 8 );
		prog[ n++ ] = 0x0A; prog[ n++ ] = 0x00;			// line 10
		for ( u32 i = 0; i < bodyLen; i++ ) prog[ n++ ] = body[ i ];
		prog[ n++ ] = 0x00;								// end of line
		prog[ n++ ] = 0x00; prog[ n++ ] = 0x00;			// end of program

		u8 *ram = scpu.memory().m_RAM;
		memcpy( ram + 0x0801, prog, n );
		const u16 vartab = (u16)( next + 2 );
		ram[ 0x2D ] = (u8)( vartab & 0xFF ); ram[ 0x2E ] = (u8)( vartab >> 8 );
		ram[ 0x2F ] = ram[ 0x2D ]; ram[ 0x30 ] = ram[ 0x2E ];
		ram[ 0x31 ] = ram[ 0x2D ]; ram[ 0x32 ] = ram[ 0x2E ];

		// "RUN" + RETURN into the keyboard buffer.
		ram[ 0x0277 ] = 'R'; ram[ 0x0278 ] = 'U';
		ram[ 0x0279 ] = 'N'; ram[ 0x027A ] = 0x0D;
		ram[ 0xC6 ] = 4;

		for ( u32 f = 0; f < runFrames; f++ ) scpu.runFrame();

		// Did anything the guest did actually reach the RAMCard? This is the
		// question persistence turns on: a save that never touches the card has
		// nothing to persist, and the dirty flag is what boot.cpp consults.
		if ( scpu.ramLink().present() && scpu.ramLink().cardRAM() )
		{
			printf( "\nRAMCard touched   : %s   content changed: %s\n",
			        scpu.ramLink().cardDirty() ? "YES" : "no",
			        scpu.ramLink().cardChanged() ? "YES" : "no" );
			if ( rlImgOrig )
			{
				const u8 *card = scpu.ramLink().cardRAM();
				const u32 n = scpu.ramLink().cardSizeBytes();
				u32 diff = 0, firstAt = 0xFFFFFFFFu;
				for ( u32 i = 0; i < n && i < rlImgOrigLen; i++ )
					if ( card[ i ] != rlImgOrig[ i ] )
					{ if ( !diff ) firstAt = i; diff++; }
				printf( "RAMCard changed   : %u of %u bytes", diff, n );
				if ( diff ) printf( " (first at $%06X)", firstAt );
				printf( "\n" );
			}
		}
	}

	// Where is it actually looping?
	//
	// A per-frame PC sample only says "somewhere in this routine". Stepping the
	// core directly and recording every PC shows the cycle itself, which is the
	// difference between "it is in the screen code" and knowing which branch
	// keeps it there.
	if ( pcTrace )
	{
		// Count markers as we go rather than storing every PC: the interesting
		// runs are millions of instructions long, and only the tail matters for
		// identifying a loop.
		struct Mark { u16 pc; const char *what; u32 hits; };
		static Mark marks[] = {
			{ 0xFCE2, "RESET", 0 }, { 0xFD02, "cart check", 0 },
			{ 0xFD50, "RAMTAS", 0 }, { 0xFD6C, "RAM test loop", 0 },
			{ 0xFD9A, "RAMTAS done", 0 }, { 0xFCFB, "RL reset hook", 0 },
			{ 0xE394, "BASIC cold start", 0 }, { 0xE4B7, "RL vector installer", 0 },
			{ 0xE453, "stock vector installer", 0 },
			{ 0xE4C7, "ICRNCH hook", 0 }, { 0xE4CE, "IERROR hook", 0 },
			{ 0xE4CA, "post-switch fetch", 0 }, { 0xE4D4, "ON-image target", 0 },
			{ 0xE37B, "BASIC warm start", 0 }, { 0xE518, "screen init", 0 },
			{ 0x0000, "PC = $0000", 0 },
			{ 0xD210, "DOS-ext trampoline", 0 },
			{ 0x9F21, "RL-DOS entry $9F21", 0 },
			{ 0x9F3C, "RL-DOS entry $9F3C", 0 },
			{ 0x1B9B, "boot STA $D203 (#$C0)", 0 },
			{ 0x1BE8, "boot STA $D204 (#$C0)", 0 },
			{ 0x1C48, "boot AND #$BF -> $D203", 0 },
			{ 0x1C50, "boot AND #$BF -> $D204", 0 },
			{ 0x9E4D, "RLDOS STX $D203 (#$00)", 0 },
			{ 0x8F7C, "RLDOS ORA #$80 -> $D203", 0 },
		};
		const u32 NM = sizeof marks / sizeof marks[ 0 ];
		static u32 tail[ 4000 ];
		u32 tailN = 0;
		for ( u32 i = 0; i < pcTrace; i++ )
		{
			const u32 pc = (u32)scpu.cpu()->pc();
			const u16 lo = (u16)pc;
			for ( u32 m = 0; m < NM; m++ ) if ( marks[ m ].pc == lo ) marks[ m ].hits++;
			// Which 256-byte pages of the accelerator's own ROM execute. The
			// boot lives in bank $F8, and "which parts of it run" is what says
			// whether a routine is reached at all.
			static u8 f8pages[ 256 ];
			if ( ( pc >> 16 ) == 0xF8 ) f8pages[ ( pc >> 8 ) & 0xFF ] = 1;
			if ( i + 1 == pcTrace )
			{
				printf( "\n  accelerator ROM bank $F8, pages executed:\n   " );
				u32 shown = 0;
				for ( u32 pg = 0; pg < 256; pg++ )
					if ( f8pages[ pg ] )
					{
						printf( " $%02X", pg );
						if ( ( ++shown % 16 ) == 0 ) printf( "\n   " );
					}
				printf( "\n  (flag setters live at $F81B9B/$F81BE8 -> page $1B)\n" );
			}
			static u32 inRLDOS = 0, inDOSExt = 0;
			if ( lo >= 0x8000 && lo <= 0x9FFF ) inRLDOS++;
			if ( lo >= 0x1000 && lo <= 0x5FFF ) inDOSExt++;
			if ( i + 1 == pcTrace )
				printf( "\n  instructions executed in $8000-$9FFF (RL-DOS window): %u\n"
				        "  instructions executed in $1000-$5FFF (ext window):    %u\n",
				        inRLDOS, inDOSExt );
			tail[ tailN % 4000 ] = pc; tailN++;
			// The first crash to $0000 is the one that matters: everything
			// after it is the wreckage. Print what led into it.
			// Window starting at a nominated PC: the reset path is long, and
			// the interesting part is a couple of hundred instructions in the
			// middle of it.
			static bool armed = false; static u32 armedAt = 0;
			if ( traceFrom >= 0 && !armed && pc == (u32)traceFrom )
			{
				armed = true; armedAt = tailN;
				printf( "\n  %u PCs from the first $%06X:\n   ", traceLen,
				        (unsigned)traceFrom );
			}
			if ( armed && tailN - armedAt < traceLen )
			{
				printf( " %06X", pc );
				if ( ( ( tailN - armedAt ) % 12 ) == 11 ) printf( "\n   " );
			}

			static bool firstZero = true;
			if ( pc == 0 && firstZero && tailN > 32 )
			{
				firstZero = false;
				printf( "\n  FIRST jump to $0000, preceding 32 PCs:\n   " );
				for ( u32 j = tailN - 33; j < tailN; j++ )
					printf( " $%04X", tail[ j % 4000 ] & 0xFFFF );
				printf( "\n" );
			}
			scpu.cpu()->step();
		}
		printf( "\n  first 120 PCs from reset:\n   " );
		for ( u32 i = 0; i < 120 && i < pcTrace; i++ )
		{
			printf( " $%04X", tail[ i % 4000 ] & 0xFFFF );
			if ( ( i % 16 ) == 15 ) printf( "\n   " );
		}
		printf( "\n" );
		printf( "\nPC trace: %u instructions\n", pcTrace );
		for ( u32 m = 0; m < NM; m++ )
			if ( marks[ m ].hits )
				printf( "    %-24s $%04X  x%u\n", marks[ m ].what, marks[ m ].pc,
				        marks[ m ].hits );
		printf( "  last PCs:" );
		const u32 start = tailN > 24 ? tailN - 24 : 0;
		for ( u32 i = start; i < tailN; i++ )
			printf( " $%04X", tail[ i % 4000 ] & 0xFFFF );
		printf( "\n" );
	}

	// The KERNAL/BASIC indirect vectors. Executing at $0000 means something
	// jumped through a null vector, and these are where such a vector lives.
	{
		const u8 *r = scpu.memory().m_RAM;
		static const struct { u16 a; const char *n; } vecs[] = {
			{ 0x0300, "IERROR" }, { 0x0302, "IMAIN" }, { 0x0304, "ICRNCH" },
			{ 0x0306, "IQPLOP" }, { 0x0308, "IGONE" }, { 0x030A, "IEVAL" },
			{ 0x0314, "CINV" }, { 0x0316, "CBINV" }, { 0x0318, "NMINV" },
			{ 0x031A, "IOPEN" }, { 0x031C, "ICLOSE" }, { 0x031E, "ICHKIN" },
			{ 0x0320, "ICKOUT" }, { 0x0322, "ICLRCH" }, { 0x0324, "IBASIN" },
			{ 0x0326, "IBSOUT" }, { 0x0328, "ISTOP" }, { 0x032A, "IGETIN" },
			{ 0x032C, "ICLALL" }, { 0x032E, "USRCMD" }, { 0x0330, "ILOAD" },
			{ 0x0332, "ISAVE" },
		};
		printf( "\nKERNAL/BASIC vectors:\n " );
		u32 nulls = 0;
		for ( u32 i = 0; i < sizeof vecs / sizeof vecs[ 0 ]; i++ )
		{
			const u16 v = (u16)( r[ vecs[ i ].a ] | ( r[ vecs[ i ].a + 1 ] << 8 ) );
			if ( !v ) nulls++;
			printf( " %s=$%04X%s", vecs[ i ].n, v, v ? "" : "!!" );
			if ( ( i % 5 ) == 4 ) printf( "\n " );
		}
		printf( "\n  null vectors: %u\n", nulls );
	}

	// Is RLDOS actually installed?
	//
	// CMD's own mirror table names the $8000-$9FFF DOS-extension window
	// "RLDOS" -- the RAMLink DOS. On an accelerated machine that is where
	// RAMLink support lives, mapped out of bank-1 SRAM when CPU DOS extensions
	// are enabled. If the accelerator never copies it there, nothing can drive
	// a RAMLink no matter how well the device is emulated.
	{
		const u8 *b1 = scpu.memoryMap().m_Bank1;
		u32 nonZero = 0, matchROM = 0;
		for ( u32 i = 0x8000; i < 0xA000; i++ )
		{
			if ( b1[ i ] ) nonZero++;
			if ( b1[ i ] == rom[ i ] ) matchROM++;
		}
		printf( "\nRLDOS window (bank 1 $8000-$9FFF): %u/8192 non-zero, "
		        "%u/8192 match scpu.rom $8000-$9FFF\n", nonZero, matchROM );
	}

	// What is actually on the screen. "Did CMD's ROM reach a usable machine"
	// is the question this tool could not answer before, and it is the only
	// one that matters when the complaint is "no cursor".
	{
		static char screen[ 25 ][ 41 ];
		for ( u32 row = 0; row < 25; row++ )
		{
			for ( u32 col = 0; col < 40; col++ )
			{
				const u8 c = scpu.memory().m_RAM[ 0x0400 + row * 40 + col ];
				char a;
				if ( c == 0x20 ) a = ' ';
				else if ( c >= 0x01 && c <= 0x1A ) a = (char)( 'A' + c - 1 );
				else if ( c >= 0x30 && c <= 0x39 ) a = (char)( '0' + c - 0x30 );
				else if ( c == 0x2E ) a = '.';
				else if ( c == 0x2A ) a = '*';
				else if ( c == 0x2C ) a = ',';
				else if ( c == 0x2D ) a = '-';
				else if ( c == 0x3A ) a = ':';
				else if ( c == 0x2F ) a = '/';
				else if ( c == 0x00 ) a = '@';
				else a = '.';
				screen[ row ][ col ] = a;
			}
			screen[ row ][ 40 ] = 0;
		}
		printf( "\n--- screen ---\n" );
		for ( u32 row = 0; row < 25; row++ )
		{
			// Skip runs of blank lines so the useful text is visible.
			bool blank = true;
			for ( u32 col = 0; col < 40; col++ )
				if ( screen[ row ][ col ] != ' ' ) { blank = false; break; }
			if ( !blank ) printf( "  |%s|\n", screen[ row ] );
		}
		printf( "--- end screen ---\n\n" );
	}

	printf( "frames run       : %u\n", frames );
	printf( "PC after boot    : $%06X\n", (unsigned)scpu.cpu()->pc() );
	printf( "intercepted      : %llu reads, %llu writes\n",
	        (unsigned long long)rec.totalReads,
	        (unsigned long long)rec.totalWrites );
	printf( "raster IRQs      : %u\n", bus.rasterIRQsRaised() );
	printf( "\n  register     reads  writes   modelled  values seen\n" );
	printf( "  ---------------------------------------------------\n" );

	u32 shown = 0, unmodelled = 0, passthrough = 0;
	for ( u32 i = 0; i < 0x100; i++ )
	{
		const Coverage &c = rec.cov[ i ];
		if ( !c.reads && !c.writes ) continue;
		const u16 addr = (u16)( 0xD000 + i );
		shown++;
		if ( !CRecordingInterceptor::isAcceleratorWindow( addr ) )
		{
			// The C64's own chips. Declining is correct; count and move on.
			passthrough++;
			if ( showAll )
			{
				char label[ 12 ];
				snprintf( label, sizeof label, "$%04X", addr );
				printRow( label, c );
			}
			continue;
		}
		const bool allClaimed = c.claimedReads == c.reads
		                     && c.claimedWrites == c.writes;
		if ( !allClaimed ) unmodelled++;
		char label[ 12 ];
		snprintf( label, sizeof label, "$%04X", addr );
		printRow( label, c );
	}
	static const char *wideNames[ 4 ] = { "$D2xx", "$D3xx", "$DFxx", "$DExx" };
	// $D2xx/$D3xx are the accelerator's own scratch RAM and its mirror. $DFxx is
	// NOT: it is cartridge I/O that the SuperCPU merely OBSERVES -- $DF7E/$DF7F
	// set and clear the RAMLink flag and then deliberately return false so the
	// write still reaches the cartridge bus. Judging it as unclaimed reports a
	// design decision as a defect.
		//
	// $DExx is the RAMLink window. Like $DFxx it is cartridge I/O rather than
	// the accelerator's own, so it is counted as pass-through -- but it is
	// always PRINTED, because "did CMD's ROM talk to the RAMLink at all" is the
	// question this tool is now being asked.
	static const bool wideIsAccelerator[ 4 ] = { true, true, false, false };
	static const bool wideAlwaysShow[ 4 ] = { false, false, true, true };
	for ( u32 i = 0; i < 4; i++ )
	{
		const Coverage &c = rec.wide[ i ];
		if ( !c.reads && !c.writes ) continue;
		shown++;
		if ( !wideIsAccelerator[ i ] )
		{
			passthrough++;
			if ( showAll || wideAlwaysShow[ i ] ) printRow( wideNames[ i ], c );
			continue;
		}
		const bool allClaimed = c.claimedReads == c.reads
		                     && c.claimedWrites == c.writes;
		if ( !allClaimed ) unmodelled++;
		printRow( wideNames[ i ], c );
	}

	{
		printf( "\n  accelerator scratch detail ($D200-$D2FF)\n" );
		printf( "  ---------------------------------------------------\n" );
		for ( u32 i = 0; i < 0x100; i++ )
		{
			const Coverage &c = rec.sysram[ i ];
			if ( !c.reads && !c.writes ) continue;
			char label[ 16 ];
			snprintf( label, sizeof label, "$%04X", 0xD200 + i );
			printRow( label, c );
		}

		// How many to PRINT. The log itself holds thousands; printing 48 of
		// them once made a run that diverged from VICE at access 300 look like
		// it diverged at 48, which is a whole afternoon of chasing the wrong
		// instruction. --cartlog sets it.
		printf( "\n  cartridge I/O trace (PC -> access), %u of %u recorded\n",
		        cartLogShow < rec.cartLogCount ? cartLogShow : rec.cartLogCount,
		        rec.cartLogCount );
		printf( "  ---------------------------------------------------\n" );
		for ( u32 i = 0; i < rec.cartLogCount && i < cartLogShow; i++ )
		{
			const CRecordingInterceptor::CartAccess &a = rec.cartLog[ i ];
			printf( "    PC $%06X  %s $%04X = $%02X  %s\n", a.pc,
			        a.write ? "W" : "R", a.addr, a.value,
			        a.claimed ? "claimed" : "DECLINED" );
		}

		printf( "\n  cartridge I/O detail ($DE00-$DFFF)\n" );
		printf( "  ---------------------------------------------------\n" );
		u32 any = 0;
		for ( u32 i = 0; i < 0x200; i++ )
		{
			const Coverage &c = rec.cart[ i ];
			if ( !c.reads && !c.writes ) continue;
			char label[ 16 ];
			snprintf( label, sizeof label, "$%04X", 0xDE00 + i );
			printRow( label, c );
			any++;
		}
		if ( !any ) printf( "  (CMD's ROM never touched cartridge I/O)\n" );
	}

	printf( "\n  %u locations touched by CMD's firmware: %u in the "
	        "accelerator's own\n  windows (listed above), %u belonging to the "
	        "C64's chips and correctly passed\n  through.\n",
	        shown, shown - passthrough, passthrough );
	printf( "\n  unmodelled accesses inside the accelerator's windows: %u\n",
	        unmodelled );
	if ( !unmodelled )
		printf( "  every access CMD's own ROM made was answered by something "
		        "we model\n" );
	else
		printf( "  the rows above are what CMD's firmware asks for and we do "
		        "not answer\n" );
	if ( !showAll && !unmodelled )
		printf( "  (--all lists the fully-modelled ones too)\n" );
	return unmodelled ? 1 : 0;
}
