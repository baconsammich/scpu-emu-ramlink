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

#ifndef _helpers_h
#define _helpers_h

#include <SDCard/emmc.h>
#include <fatfs/ff.h>

// Reads a whole file into 'data'. 'maxSize' is the capacity of that buffer and
// is enforced: a file larger than the destination is refused outright rather
// than being read past the end of the buffer. Returns 0 on any failure -- file
// missing, too large, or a short/failed read.
// Both readFile() and writeFile() move data in chunks of this size rather than
// asking FatFs for the whole thing at once. 64KB is 128 sectors, comfortably
// inside what Circle's EMMC driver does all day for ROM loads; one request for
// an 8MB image is 16384 sectors and the driver does not survive it. That is
// not a theoretical limit -- it is the bug that left a real machine with no
// accelerator and no cursor, diagnosed by renaming the image away.
#define SCPU_IO_CHUNK ( 64 * 1024 )

extern int readFile( CLogger *logger, const char *DRIVE, const char *FILENAME, u8 *data, u32 *size, u32 maxSize );
extern int getFileSize( CLogger *logger, const char *DRIVE, const char *FILENAME, u32 *size );
// Returns success only when the complete buffer was written and the file was
// closed successfully. The input is read-only.
extern int writeFile( CLogger *logger, const char *DRIVE, const char *FILENAME, const u8 *data, u32 size );
// Write via a temporary and rename over the target, so a failure part-way
// through leaves the previous file intact. FA_CREATE_ALWAYS truncates on open,
// which for an 8MB RAMCard image means a failed save would otherwise destroy
// the card you were trying to preserve.
extern int writeFileAtomic( CLogger *logger, const char *DRIVE, const char *FILENAME,
                            const char *TMPNAME, const u8 *data, u32 size );

#define ROMH_ACCESS			(!(g2 & bROMH))
#define CPU_RESET			(!(g2&bRESET_OUT)) 

#define STANDARD_SETUP_TIMER_INTERRUPT_CYCLECOUNTER_GPIO										\
	boolean bOK = TRUE;																			\
	m_CPUThrottle.SetSpeed( CPUSpeedMaximum );													\
	if ( bOK ) bOK = m_Screen.Initialize();														\
	if ( bOK ) { 																				\
		CDevice *pTarget = m_DeviceNameService.GetDevice( m_Options.GetLogDevice(), FALSE );	\
		if ( pTarget == 0 )	pTarget = &m_Screen;												\
		bOK = m_Logger.Initialize( pTarget ); 													\
	}																							\
	if ( bOK ) bOK = m_Interrupt.Initialize(); 													\
	if ( bOK ) bOK = m_Timer.Initialize();														\
	/* initialize ARM cycle counters (for accurate timing) */ 									\
	initCycleCounter(); 																		\
	/* initialize GPIOs */ 																		\
	gpioInit(); 																				

#define min(a,b) (((a)<(b))?(a):(b))
#define max(a,b) (((a)>(b))?(a):(b))

#endif
