/*

  {_______            {_          {______
        {__          {_ __               {__
        {__         {_  {__               {__
     {__           {__   {__               {__
 {______          {__     {__              {__
       {__       {__       {__            {__   
         {_________         {______________		Expansion Unit
                
 RADExp - A framework for DMA interfacing with Commodore C64/C128 computers using a Raspberry Pi Zero 2 or 3A+/3B+
        - this file contains some code already used in Sidekick64
 Copyright (c) 2019-2022 Carsten Dachsbacher <frenetic@dachsbacher.de>

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

#include "helpers.h"

// file reading
int readFile( CLogger *logger, const char *DRIVE, const char *FILENAME, u8 *data, u32 *size, u32 maxSize )
{
	FATFS m_FileSystem;

	*size = 0;

	// mount file system
	if ( f_mount( &m_FileSystem, DRIVE, 1 ) != FR_OK )
	{
		logger->Write( "RAD", LogPanic, "Cannot mount drive: %s", DRIVE );
		return 0;
	}

	// get filesize. The result of f_stat has to be checked before info.fsize is
	// used: on failure it is uninitialised, and a garbage length here becomes a
	// wild read straight into the caller's buffer.
	FILINFO info;
	u32 result = f_stat( FILENAME, &info );
	if ( result != FR_OK )
	{
		logger->Write( "RAD", LogNotice, "Cannot stat file: %s", FILENAME );
		f_mount( 0, DRIVE, 0 );
		return 0;
	}

	u32 filesize = (u32)info.fsize;

	// Refuse anything that will not fit rather than overrunning the caller.
	// Without this a mis-named 128K SuperCPU ROM dropped in as kernal.rom would
	// scribble 120K past an 8K buffer and corrupt the firmware.
	if ( filesize > maxSize )
	{
		logger->Write( "RAD", LogError, "%s is %u bytes, buffer holds %u -- refused",
		               FILENAME, filesize, maxSize );
		f_mount( 0, DRIVE, 0 );
		return 0;
	}

	// open file
	FIL file;
	result = f_open( &file, FILENAME, FA_READ | FA_OPEN_EXISTING );
	if ( result != FR_OK )
	{
		logger->Write( "RAD", LogNotice, "Cannot open file: %s", FILENAME );

		if ( f_mount( 0, DRIVE, 0 ) != FR_OK )
			logger->Write( "RAD", LogPanic, "Cannot unmount drive: %s", DRIVE );

		return 0;
	}

	// Read in bounded chunks rather than one call.
	//
	// This used to be a single f_read of the whole file, which is fine for a
	// 128K ROM and is NOT fine for an 8MB RAMCard image: FatFs turns one
	// request into one disk_read of 16384 sectors, and Circle's EMMC driver
	// does not survive a transfer that size. The symptom on real hardware was a
	// Raspberry Pi that never reached its second ACT-LED milestone, so the C64
	// was left with no accelerator and no cursor -- a hang with no apparent
	// connection to the file being read. Bisected on hardware: the same card
	// with the image renamed away booted normally.
	//
	// 64KB is 128 sectors, comfortably inside what the driver does all day for
	// ROM loads, and costs one extra loop iteration per 64KB.
	//
	// CONFIRMED ON HARDWARE (2026-09-14): the same card that used to hang now
	// boots with the 8MB SCPU/ramlink.img present, and boots identically with
	// it renamed away. The file is exactly card-sized and the guard above is a
	// strict `>`, so it cannot have been refused for size -- the read ran.

	u32 nBytesRead = 0;
	result = FR_OK;
	while ( nBytesRead < filesize )
	{
		u32 want = filesize - nBytesRead;
		if ( want > SCPU_IO_CHUNK ) want = SCPU_IO_CHUNK;

		UINT got = 0;
		result = f_read( &file, data + nBytesRead, want, &got );
		if ( result != FR_OK )
			break;

		nBytesRead += got;
		if ( got != want )
			break;					// short read: reported below
	}

	int ok = 1;
	if ( result != FR_OK )
	{
		logger->Write( "RAD", LogError, "Read error on %s after %u of %u bytes",
		               FILENAME, nBytesRead, filesize );
		ok = 0;
	} else
	if ( nBytesRead != filesize )
	{
		// A short read leaves the tail of the buffer holding whatever was there
		// before. Reporting success would hand the caller a half-filled ROM.
		logger->Write( "RAD", LogError, "Short read on %s: got %u of %u bytes",
		               FILENAME, nBytesRead, filesize );
		ok = 0;
	}

	if ( f_close( &file ) != FR_OK )
		logger->Write( "RAD", LogPanic, "Cannot close file" );

	// unmount file system
	if ( f_mount( 0, DRIVE, 0 ) != FR_OK )
		logger->Write( "RAD", LogPanic, "Cannot unmount drive: %s", DRIVE );

	if ( ok )
		*size = nBytesRead;

	return ok;
}

int getFileSize( CLogger *logger, const char *DRIVE, const char *FILENAME, u32 *size )
{
	FATFS m_FileSystem;
	*size = 0;

	// mount file system
	if ( f_mount( &m_FileSystem, DRIVE, 1 ) != FR_OK )
	{
		logger->Write( "RAD", LogPanic, "Cannot mount drive: %s", DRIVE );
		return 0;
	}

	// get filesize
	FILINFO info;
	u32 result = f_stat( FILENAME, &info );

	if ( result != FR_OK )
	{
		logger->Write( "RAD", LogNotice, "Cannot stat file: %s", FILENAME );
		f_mount( 0, DRIVE, 0 );
		return 0;
	}

	*size = (u32)info.fsize;

	// unmount file system
	if ( f_mount( 0, DRIVE, 0 ) != FR_OK )
		logger->Write( "RAD", LogPanic, "Cannot unmount drive: %s", DRIVE );
	
	return 1;
}

// file writing
int writeFile( CLogger *logger, const char *DRIVE, const char *FILENAME, const u8 *data, u32 size )
{
	FATFS m_FileSystem;

	// mount file system
	if ( f_mount( &m_FileSystem, DRIVE, 1 ) != FR_OK )
	{
		logger->Write( "RAD", LogPanic, "Cannot mount drive: %s", DRIVE );
		return 0;
	}

	// open file
	FIL file;
	u32 result = f_open( &file, FILENAME, FA_WRITE | FA_CREATE_ALWAYS );
	if ( result != FR_OK )
	{
		logger->Write( "RAD", LogNotice, "Cannot open file: %s", FILENAME );
		f_mount( 0, DRIVE, 0 );
		return 0;
	}

	// Write in bounded chunks, for exactly the reason readFile() does.
	//
	// This used to be one f_write of the whole buffer. It was harmless while
	// the only caller was a 64KB diagnostic text file, and it is NOT harmless
	// now that an 8MB RAMCard image goes through here: FatFs turns one request
	// into one disk_write of 16384 sectors, and Circle's EMMC driver does not
	// survive a transfer that size. That failure has already been paid for
	// once on the read side -- a Pi that never reached its second ACT-LED
	// milestone, so the C64 was left with no accelerator and no cursor.
	u32 nBytesWritten = 0;
	result = FR_OK;
	while ( nBytesWritten < size )
	{
		u32 want = size - nBytesWritten;
		if ( want > SCPU_IO_CHUNK ) want = SCPU_IO_CHUNK;

		UINT put = 0;
		result = f_write( &file, data + nBytesWritten, want, &put );
		if ( result != FR_OK ) break;
		nBytesWritten += put;
		if ( put != want ) break;			// disk full, or a short write
	}

	int ok = 1;
	if ( result != FR_OK )
	{
		logger->Write( "RAD", LogError, "Write error on %s", FILENAME );
		ok = 0;
	} else if ( nBytesWritten != size )
	{
		logger->Write( "RAD", LogError, "Short write on %s: wrote %u of %u bytes",
		               FILENAME, nBytesWritten, size );
		ok = 0;
	}

	if ( f_close( &file ) != FR_OK )
	{
		logger->Write( "RAD", LogPanic, "Cannot close file" );
		ok = 0;
	}

	// unmount file system
	if ( f_mount( 0, DRIVE, 0 ) != FR_OK )
		logger->Write( "RAD", LogPanic, "Cannot unmount drive: %s", DRIVE );
	
	return ok;
}
int writeFileAtomic( CLogger *logger, const char *DRIVE, const char *FILENAME,
                     const char *TMPNAME, const u8 *data, u32 size )
{
	// Write the new contents beside the target, then swap. f_open with
	// FA_CREATE_ALWAYS truncates the moment it succeeds, so writing in place
	// means any later failure -- a short write, a full card, a power cut --
	// leaves nothing behind. For an 8MB RAMCard that is the difference between
	// "this save did not work" and "your card is gone".
	if ( !writeFile( logger, DRIVE, TMPNAME, data, size ) )
		return 0;

	FATFS fs;
	if ( f_mount( &fs, DRIVE, 1 ) != FR_OK )
	{
		logger->Write( "RAD", LogError, "Cannot mount to swap %s", FILENAME );
		return 0;
	}

	// f_rename will not replace an existing file, so the old one goes first.
	// This is the one instant where neither name holds the finished data; it
	// spans two directory updates and nothing else.
	f_unlink( FILENAME );
	int ok = ( f_rename( TMPNAME, FILENAME ) == FR_OK );
	if ( !ok )
	{
		logger->Write( "RAD", LogError, "Wrote %s but could not rename it to %s",
		               TMPNAME, FILENAME );
		logger->Write( "RAD", LogNotice, "the data is intact in %s", TMPNAME );
	}

	f_mount( 0, DRIVE, 0 );
	return ok;
}

