/*
   SCPU-EMU - CMD SuperCPU emulation for the C64/C128 using a RAD Expansion Unit
   Copyright (c) 2026 SCPU-EMU contributors

   The small pieces that connect CRAMLink to a machine.

   Same shape and same reasoning as REU/reu_wiring.h: CRAMLink is pure logic
   that knows nothing about a C64, and these adapters are what give it an I/O
   window, a clock and a way to reach the REU.

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
#ifndef _scpu_ramlink_wiring_h
#define _scpu_ramlink_wiring_h

#include "ramlink.h"
#include "../C64/c64_memory.h"
#include "../REU/reu.h"

// Presents a CRAMLink as an IIOInterceptor, for the same separation-of-layers
// reason CREUInterceptor exists.
class CRAMLinkInterceptor : public IIOInterceptor
{
public:
	CRAMLinkInterceptor() : m_Link( 0 ) {}
	void attach( CRAMLink *link ) { m_Link = link; }

	bool ioRead( u16 addr, u8 &value ) override
	{
		return m_Link ? m_Link->read( addr, value ) : false;
	}
	bool ioWrite( u16 addr, u8 value ) override
	{
		return m_Link ? m_Link->write( addr, value ) : false;
	}

private:
	CRAMLink *m_Link;
};

// Lets RAMLink's $DF21 trap reach the REU's command register.
//
// The REU's own register write is used rather than poking its state, so the
// forwarded command goes through exactly the same path -- including the
// autoload and $FF00-arming rules -- as a direct write to $DF01 would.
class CRAMLinkREUBridge : public IRAMLinkREU
{
public:
	CRAMLinkREUBridge() : m_REU( 0 ) {}
	void attach( CREU *reu ) { m_REU = reu; }

	void ramlinkREUCommand( u8 value ) override
	{
		if ( m_REU ) m_REU->write( 0xDF01, value );
	}

private:
	CREU *m_REU;
};

// A clock that always reports the same instant.
//
// This is the default, and on the RAD it is the honest one: a Raspberry Pi in a
// C64 has no battery-backed RTC and no network, so it does not know the time.
// Reporting a fixed, obviously-placeholder timestamp is better than reporting
// an uptime counter dressed up as a date, because a file written at "1 Jan 1980
// 00:00" is recognisably undated while one written at "1 Jan 1970 00:04" looks
// like real metadata and is not.
class CRAMLinkFixedClock : public IRAMLinkClock
{
public:
	CRAMLinkFixedClock()
		: m_Year( 1980 ), m_Month( 1 ), m_Day( 1 ), m_Weekday( 2 ),
		  m_Hour( 0 ), m_Minute( 0 ), m_Second( 0 ) {}

	void set( u32 year, u32 month, u32 day, u32 weekday,
	          u32 hour, u32 minute, u32 second )
	{
		m_Year = year; m_Month = month; m_Day = day; m_Weekday = weekday;
		m_Hour = hour; m_Minute = minute; m_Second = second;
	}

	void ramlinkNow( u32 &year, u32 &month, u32 &day, u32 &weekday,
	                 u32 &hour, u32 &minute, u32 &second ) override
	{
		year = m_Year; month = m_Month; day = m_Day; weekday = m_Weekday;
		hour = m_Hour; minute = m_Minute; second = m_Second;
	}

private:
	u32 m_Year, m_Month, m_Day, m_Weekday, m_Hour, m_Minute, m_Second;
};

#endif
