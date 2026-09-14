/*
   SCPU-EMU - CMD SuperCPU emulation for the C64/C128 using a RAD Expansion Unit
   Copyright (c) 2026 SCPU-EMU contributors

   The two small pieces that connect CREU to a machine.

   CIOInterceptorChain exists because CC64Memory holds exactly ONE
   IIOInterceptor and CSuperCPURegisters already occupies it. Rather than widen
   that interface -- which is on a hot path and owned by the memory layer -- the
   chain presents itself as a single interceptor and forwards to two.

   CREUMemoryHost is the other half: it hands the REU a way to move bytes to and
   from C64 memory without the REU knowing what a CC64Memory is.

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
#ifndef _scpu_reu_wiring_h
#define _scpu_reu_wiring_h

#include "reu.h"
#include "../C64/c64_memory.h"

// Presents several interceptors as one.
//
// ORDER MATTERS AND IS NOT ARBITRARY. The primary is asked first and each
// subsequent device only sees what the ones before it declined.
// CSuperCPURegisters must be primary: it observes $DF7E/$DF7F for RAMLink and
// deliberately returns false so the write still continues to the cartridge bus
// -- where, now that there IS a RAMLink, the RAMLink itself claims it. That
// pair is the whole reason the ordering rule is written down: the accelerator
// has to see the strobe to update $D0BC bit 6 AND the device has to see it to
// switch on, and only a chain that forwards a declined write gets both.
//
// The REU is deliberately AFTER RAMLink. A real RAMLink hosts the REU inside
// its own pass-through and claims I/O2 ahead of it, which is why RAMLink needs
// a $DF21 trap to reach the REU's command register at all.
#define SCPU_IO_CHAIN_MAX 4

class CIOInterceptorChain : public IIOInterceptor
{
public:
	CIOInterceptorChain() : m_Count( 0 )
	{
		for ( u32 i = 0; i < SCPU_IO_CHAIN_MAX; i++ ) m_Chain[ i ] = 0;
	}

	// Two-argument form kept because it reads well at the call site and is what
	// every existing test uses.
	void attach( IIOInterceptor *primary, IIOInterceptor *secondary )
	{
		m_Count = 0;
		add( primary );
		add( secondary );
	}

	void attach( IIOInterceptor *primary, IIOInterceptor *second,
	             IIOInterceptor *third )
	{
		m_Count = 0;
		add( primary );
		add( second );
		add( third );
	}

	void add( IIOInterceptor *next )
	{
		if ( next && m_Count < SCPU_IO_CHAIN_MAX )
			m_Chain[ m_Count++ ] = next;
	}

	bool ioRead( u16 addr, u8 &value ) override
	{
		for ( u32 i = 0; i < m_Count; i++ )
			if ( m_Chain[ i ]->ioRead( addr, value ) ) return true;
		return false;
	}

	bool ioWrite( u16 addr, u8 value ) override
	{
		for ( u32 i = 0; i < m_Count; i++ )
			if ( m_Chain[ i ]->ioWrite( addr, value ) ) return true;
		return false;
	}

	// The timing and policy queries below are asked ABOUT an address rather
	// than performed on it, so there is no "handled" answer to chain on. The
	// primary owns them: it is the accelerator's own register file, and its
	// answers describe the machine. The REU adds no stretch policy of its own.
	bool ioAccessNeedsStretch( u16 a, bool w ) const override
		{ return m_Count ? m_Chain[ 0 ]->ioAccessNeedsStretch( a, w ) : w; }
	bool ioAccessUsesWriteBuffer( u16 a, bool w ) const override
		{ return m_Count ? m_Chain[ 0 ]->ioAccessUsesWriteBuffer( a, w ) : false; }
	bool dosExtensionEnabled() const override
		{ return m_Count ? m_Chain[ 0 ]->dosExtensionEnabled() : false; }
	const bool *dosExtensionStatePtr() const override
		{ return m_Count ? m_Chain[ 0 ]->dosExtensionStatePtr() : 0; }
	const bool *hardwareRegsStatePtr() const override
		{ return m_Count ? m_Chain[ 0 ]->hardwareRegsStatePtr() : 0; }
	bool simmWindowWritesEnabled() const override
		{ return m_Count ? m_Chain[ 0 ]->simmWindowWritesEnabled() : true; }
	bool interruptRerouteRequested() const override
		{ return m_Count ? m_Chain[ 0 ]->interruptRerouteRequested() : false; }

private:
	IIOInterceptor *m_Chain[ SCPU_IO_CHAIN_MAX ];
	u32             m_Count;
};

// Presents a CREU as an IIOInterceptor.
//
// CREU deliberately does NOT derive from IIOInterceptor: that interface lives
// in the memory layer, and inheriting it would drag c64_memory.h into a class
// whose whole value is being pure logic testable without a C64. The adapter
// costs one virtual call on an access that was already going to a device, and
// keeps that separation intact.
class CREUInterceptor : public IIOInterceptor
{
public:
	CREUInterceptor() : m_REU( 0 ) {}
	void attach( CREU *reu ) { m_REU = reu; }

	bool ioRead( u16 addr, u8 &value ) override
	{
		return m_REU ? m_REU->read( addr, value ) : false;
	}
	bool ioWrite( u16 addr, u8 value ) override
	{
		return m_REU ? m_REU->write( addr, value ) : false;
	}

private:
	CREU *m_REU;
};

// Moves REU transfer bytes through the ordinary memory path.
//
// write8() is used rather than a direct array store ON PURPOSE. A real REU
// writes into the DRAM the VIC-II fetches from, so a transfer into screen or
// bitmap memory has to reach the physical machine exactly as a CPU store would.
// Writing only the Pi's shadow would produce a machine that believes the
// transfer happened while the picture never changes -- the same failure the
// $D000-$DFFF suppression produced, and just as hard to recognise.
//
// It also means an REU transfer costs mirror bus traffic. That is correct: on
// real hardware the transfer really does occupy the bus, which is why the unit
// holds the CPU off it for the duration.
class CREUMemoryHost : public IREUHost
{
public:
	CREUMemoryHost() : m_Memory( 0 ) {}
	void attach( CC64Memory *memory ) { m_Memory = memory; }

	u8 reuHostRead( u16 addr ) override
	{
		return m_Memory ? m_Memory->read8( addr ) : 0xFF;
	}

	void reuHostWrite( u16 addr, u8 value ) override
	{
		if ( m_Memory ) m_Memory->write8( addr, value );
	}

private:
	CC64Memory *m_Memory;
};

#endif
