/*
   SCPU-EMU - CMD SuperCPU emulation for the C64/C128 using a RAD Expansion Unit
   Copyright (c) 2026 SCPU-EMU contributors

   Host-side ROM file resolution.

   The firmware wants four fixed names under SCPU/ on the SD card -- kernal.rom,
   basic.rom, chargen.rom, scpu.rom -- because the card layout is a contract and
   a bare-metal loader has no business guessing. Host tools and the test suite
   are the opposite case: they run against whatever a developer happened to
   download, and the canonical dumps arrive named after their Commodore part
   numbers. Requiring a manual `cp kernal.901227-03.bin kernal.rom` before the
   strongest tests in the suite will run is a step that gets skipped, and when
   it is skipped those tests report "skipped: no ROMs" and go green -- which is
   exactly how eleven integration tests, including every genuine-KERNAL boot,
   sat dark.

   So: try the SD-card name first, then the part-number names, and search
   upwards from the working directory so the tests run from a build tree too.
   SCPU_ROMS overrides the directory outright.

   Host builds only -- the firmware reads the SD card through Circle.

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
#ifndef _scpu_rom_paths_h
#define _scpu_rom_paths_h

#include <cstdio>
#include <cstdlib>
#include <cstring>

// Alternative spellings for each logical ROM, most-preferred first. The first
// entry is always the SD-card name, so an existing ROMs/ directory that was set
// up by hand keeps resolving exactly as it did before.
struct SCPURomAliases
{
	const char *logical;
	const char *alias[ 4 ];
};

static const SCPURomAliases g_SCPURomAliases[] =
{
	{ "kernal.rom",  { "kernal.901227-03.bin", "kernal-901227-03.bin",
	                   "901227-03.bin", 0 } },
	{ "basic.rom",   { "basic.901226-01.bin", "basic-901226-01.bin",
	                   "901226-01.bin", 0 } },
	// 906143-02 is the character generator fitted to later C64 boards. It is a
	// different part number from 901225-01 but the same 4KB of glyphs, and
	// either is a correct chargen for this purpose.
	{ "chargen.rom", { "characters.901225-01.bin", "chargen-901225-01.bin",
	                   "chargen-906143-02.bin", 0 } },
	// The accelerator's own image. 2.04 is what `make sdcard` stages and what
	// the runtime expects; 1.4 is accepted as a fallback so a card set up with
	// the earlier revision still resolves.
	{ "scpu.rom",    { "scpu-dos-2.04.bin", "scpu-dos-1.4.bin", 0, 0 } },
};

// Directory prefixes to try, in order. The repeated parent steps let a tool run
// from build/host/ or from a sibling directory and still find the tree's ROMs/.
static const char *const g_SCPURomDirs[] =
{
	"ROMs/", "../ROMs/", "../../ROMs/", "../../../ROMs/", "", 0
};

// Open the named logical ROM, or return null. `resolved`, if given, receives
// the path that actually opened -- worth reporting, because "which chargen did
// it pick up?" is otherwise invisible.
inline std::FILE *scpuRomOpen( const char *logical, const char **resolved = 0 )
{
	static char path[ 512 ];

	// SCPU_ROMS wins outright when it is set: an explicit answer to "where are
	// the ROMs" should not be second-guessed by a search.
	const char *envDir = std::getenv( "SCPU_ROMS" );

	for ( int pass = 0; pass < 2; pass++ )
	{
		// pass 0: the logical name. pass 1: the part-number aliases.
		const char *const *names = 0;
		const char *single[ 2 ] = { logical, 0 };
		if ( pass == 0 )
			names = single;
		else
		{
			for ( unsigned i = 0;
			      i < sizeof g_SCPURomAliases / sizeof g_SCPURomAliases[ 0 ]; i++ )
				if ( !std::strcmp( g_SCPURomAliases[ i ].logical, logical ) )
					names = g_SCPURomAliases[ i ].alias;
			if ( !names ) continue;
		}

		for ( unsigned n = 0; names[ n ]; n++ )
		{
			for ( unsigned d = 0; ; d++ )
			{
				const char *dir;
				if ( envDir && d == 0 )
					dir = envDir;
				else
				{
					const unsigned k = envDir ? d - 1 : d;
					if ( !g_SCPURomDirs[ k ] ) break;
					dir = g_SCPURomDirs[ k ];
				}

				const size_t dl = std::strlen( dir );
				const bool needSlash = dl && dir[ dl - 1 ] != '/' && dir[ dl - 1 ] != '\\';
				std::snprintf( path, sizeof path, "%s%s%s",
				               dir, needSlash ? "/" : "", names[ n ] );

				std::FILE *f = std::fopen( path, "rb" );
				if ( f )
				{
					if ( resolved ) *resolved = path;
					return f;
				}
			}
		}
	}

	if ( resolved ) *resolved = 0;
	return 0;
}

// Load exactly `expected` bytes of a logical ROM. Short files fail rather than
// leaving the tail of the buffer undefined -- a truncated KERNAL boots into
// nonsense a long way from its cause.
inline bool scpuRomLoad( const char *logical, unsigned char *dst, unsigned expected,
                         const char **resolved = 0 )
{
	std::FILE *f = scpuRomOpen( logical, resolved );
	if ( !f ) return false;
	const size_t got = std::fread( dst, 1, expected, f );
	std::fclose( f );
	return got == expected;
}

#endif
