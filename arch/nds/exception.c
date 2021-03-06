/*---------------------------------------------------------------------------------
  $Id: gurumeditation.c,v 1.6 2006/08/03 09:35:36 wntrmute Exp $

  Copyright (C) 2005
  	Dave Murphy (WinterMute)

  This software is provided 'as-is', without any express or implied
  warranty.  In no event will the authors be held liable for any
  damages arising from the use of this software.

  Permission is granted to anyone to use this software for any
  purpose, including commercial applications, and to alter it and
  redistribute it freely, subject to the following restrictions:

  1. The origin of this software must not be misrepresented; you
     must not claim that you wrote the original software. If you use
     this software in a product, an acknowledgment in the product
     documentation would be appreciated but is not required.
  2. Altered source versions must be plainly marked as such, and
     must not be misrepresented as being the original software.
  3. This notice may not be removed or altered from any source
     distribution.

  $Log: gurumeditation.c,v $
  Revision 1.6  2006/08/03 09:35:36  wntrmute
  fix storing pc

  Revision 1.5  2006/07/06 02:14:33  wntrmute
  read r15 in enterException
  add return to bios

  Revision 1.4  2006/06/21 20:12:39  wntrmute
  thumb ldrsh has register offset

  Revision 1.3  2006/06/19 19:12:01  wntrmute
  correct prototypes again
  add defaultHandler function to install default handler

  Revision 1.2  2006/06/19 18:22:25  wntrmute
  completed thumb address calculations

  Revision 1.1  2006/06/19 14:09:37  wntrmute
  split default exception handler into separate file
  correct prototypes

---------------------------------------------------------------------------------*/

// Modified by kvance 2007-09-13
// Disable DMA and reset scrolling as soon as the exception handler is called.

#include "exception.h"
#include "../../src/config.h"
#include "../../src/compat.h"

#ifdef CONFIG_NDS

#include <stdio.h>

//---------------------------------------------------------------------------------
static unsigned long ARMShift(unsigned long value,unsigned char shift) {
//---------------------------------------------------------------------------------
	int index, i;
	bool isN;

	// no shift at all
	if (shift == 0x0B) return value ;
	if (shift & 0x01) {
		// shift index is a register
		index = exceptionRegisters[(shift >> 4) & 0x0F];
	} else {
		// constant shift index
		index = ((shift >> 3) & 0x1F) ;
	} ;
	switch (shift & 0x06) {
		case 0x00:
			// logical left
			return (value << index) ;
		case 0x02:
			// logical right
			return (value >> index) ;
		case 0x04:
			// arithmetical right
			isN = (value & 0x80000000) ;
			value = value >> index ;
			if (isN) {
				for (i=31;i>31-index;i--) {
					value = value | (1 << i) ;
				} ;
			} ;
			return value ;
		case 0x06:
			// rotate right
			index = index & 0x1F;
			value = (value >> index) | (value << (32-index));
			return value;
	};
	return value;
}


//---------------------------------------------------------------------------------
static u32 getExceptionAddress( u32 opcodeAddress, u32 thumbState) {
//---------------------------------------------------------------------------------

	int Rf, Rb, Rd, Rn, Rm;

	if (thumbState) {
		// Thumb

		unsigned short opcode = *(unsigned short *)opcodeAddress ;
		// ldr r,[pc,###]			01001ddd ffffffff
		// ldr r,[r,r]				0101xx0f ffbbbddd
		// ldrsh					0101xx1f ffbbbddd
		// ldr r,[r,imm]			011xxfff ffbbbddd
		// ldrh						1000xfff ffbbbddd
		// ldr r,[sp,###]			1001xddd ffffffff
		// push						1011x10l llllllll
		// ldm						1100xbbb llllllll


		if ((opcode & 0xF800) == 0x4800) {
			// ldr r,[pc,###]
			s8 offset = opcode & 0xff;
			return exceptionRegisters[15] + offset;
		} else if ((opcode & 0xF200) == 0x5000) {
			// ldr r,[r,r]
			Rb = (opcode >> 3) & 0x07 ;
			Rf = (opcode >> 6) & 0x07 ;
			return exceptionRegisters[Rb] + exceptionRegisters[Rf];

		} else if ((opcode & 0xF200) == 0x5200) {
			// ldrsh
			Rb = (opcode >> 3) & 0x07;
			Rf = (opcode >> 6) & 0x03;
			return exceptionRegisters[Rb] + exceptionRegisters[Rf];

		} else if ((opcode & 0xE000) == 0x6000) {
			// ldr r,[r,imm]
			Rb = (opcode >> 3) & 0x07;
			Rf = (opcode >> 6) & 0x1F ;
			return exceptionRegisters[Rb] + (Rf << 2);
		} else if ((opcode & 0xF000) == 0x8000) {
			// ldrh
			Rb = (opcode >> 3) & 0x07 ;
			Rf = (opcode >> 6) & 0x1F ;
			return exceptionRegisters[Rb] + (Rf << 2);
		} else if ((opcode & 0xF000) == 0x9000) {
			// ldr r,[sp,#imm]
			s8 offset = opcode & 0xff;
			return exceptionRegisters[13] + offset;
		} else if ((opcode & 0xF700) == 0xB500) {
			// push/pop
			return exceptionRegisters[13];
		} else if ((opcode & 0xF000) == 0xC000) {
			// ldm/stm
			Rd = (opcode >> 8) & 0x07;
			return exceptionRegisters[Rd];
		}
	} else {
		// arm32
		unsigned long opcode = *(unsigned long *)opcodeAddress ;

		// SWP			xxxx0001 0x00nnnn dddd0000 1001mmmm
		// STR/LDR		xxxx01xx xxxxnnnn ddddffff ffffffff
		// STRH/LDRH	xxxx000x x0xxnnnn dddd0000 1xx1mmmm
		// STRH/LDRH	xxxx000x x1xxnnnn ddddffff 1xx1ffff
		// STM/LDM		xxxx100x xxxxnnnn llllllll llllllll

		if ((opcode & 0x0FB00FF0) == 0x01000090) {
			// SWP
			Rn = (opcode >> 16) & 0x0F;
			return exceptionRegisters[Rn];
		} else if ((opcode & 0x0C000000) == 0x04000000) {
			// STR/LDR
			Rn = (opcode >> 16) & 0x0F;
			if (opcode & 0x02000000) {
				// Register offset
				Rm = opcode & 0x0F;
				if (opcode & 0x01000000) {
					unsigned short shift = (unsigned short)((opcode >> 4) & 0xFF) ;
					// pre indexing
					long Offset = ARMShift(exceptionRegisters[Rm],shift);
					// add or sub the offset depending on the U-Bit
					return exceptionRegisters[Rn] + ((opcode & 0x00800000)?Offset:-Offset);
				} else {
					// post indexing
					return exceptionRegisters[Rn];
				}
			} else {
				// Immediate offset
				unsigned long Offset = (opcode & 0xFFF) ;
				if (opcode & 0x01000000) {
					// pre indexing
					// add or sub the offset depending on the U-Bit
					return exceptionRegisters[Rn] + ((opcode & 0x00800000)?Offset:-Offset);
				} else {
					// post indexing
					return exceptionRegisters[Rn];
				}
			}
		} else if ((opcode & 0x0E400F90) == 0x00000090) {
			unsigned short shift;
			long Offset;
			// LDRH/STRH with register Rm
			Rn = (opcode >> 16) & 0x0F;
			Rd = (opcode >> 12) & 0x0F;
			Rm = opcode & 0x0F;
			shift = (unsigned short)((opcode >> 4) & 0xFF);
			Offset = ARMShift(exceptionRegisters[Rm],shift);
			// add or sub the offset depending on the U-Bit
			return exceptionRegisters[Rn] + ((opcode & 0x00800000)?Offset:-Offset);
		} else if ((opcode & 0x0E400F90) == 0x00400090) {
			unsigned long Offset;
			// LDRH/STRH with immediate offset
			Rn = (opcode >> 16) & 0x0F;
			Rd = (opcode >> 12) & 0x0F;
			Offset = (opcode & 0xF) | ((opcode & 0xF00)>>8) ;
			// add or sub the offset depending on the U-Bit
			return exceptionRegisters[Rn] + ((opcode & 0x00800000)?Offset:-Offset) ;
		} else if ((opcode & 0x0E000000) == 0x08000000) {
			// LDM/STM
			Rn = (opcode >> 16) & 0x0F;
			return exceptionRegisters[Rn];
		}
	}
	return 0;
}

static const char *registerNames[] =
	{	"r0","r1","r2","r3","r4","r5","r6","r7",
		"r8 ","r9 ","r10","r11","r12","sp ","lr ","pc " };

extern const char __itcm_start[];
//---------------------------------------------------------------------------------
static void mzxExceptionHandler() {
//---------------------------------------------------------------------------------
	u32 *stack, currentMode, thumbState, codeAddress, exceptionAddress = 0;
	int i, offset = 8;

	DMA0_CR = 0;
	DMA1_CR = 0;
	DMA2_CR = 0;
	DMA3_CR = 0;
	REG_BG0HOFS_SUB = 0;
	REG_BG0VOFS_SUB = 0;

	videoSetMode(0);
	videoSetModeSub(MODE_0_2D | DISPLAY_BG0_ACTIVE);
	vramSetBankC(VRAM_C_SUB_BG);

	REG_BG0CNT_SUB = BG_MAP_BASE(31);

	BG_PALETTE_SUB[0] = RGB15(31,0,0);
	BG_PALETTE_SUB[255] = RGB15(31,31,31);

	consoleDemoInit();

	iprintf("\x1b[5CGuru Meditation Error!\n");
	currentMode = getCPSR() & 0x1f;
	thumbState = ((*(u32*)0x027FFD90) & 0x20);

	if ( currentMode == 0x17 ) {
		iprintf ("\x1b[10Cdata abort!\n\n");
		codeAddress = exceptionRegisters[15] - offset;
		if (	(codeAddress > 0x02000000 && codeAddress < 0x02400000) ||
				(codeAddress > (u32)__itcm_start && codeAddress < (u32)(__itcm_start + 32768)) )
			exceptionAddress = getExceptionAddress( codeAddress, thumbState);
		else
			exceptionAddress = codeAddress;
			
	} else {
		if (thumbState)
			offset = 2;
		else
			offset = 4;
		iprintf("\x1b[5Cundefined instruction!\n\n");
		codeAddress = exceptionRegisters[15] - offset;
		exceptionAddress = codeAddress;
	}

	iprintf("  pc: %08X addr: %08X\n\n",codeAddress,exceptionAddress);

	for ( i=0; i < 8; i++ ) {
		iprintf(	"  %s: %08X   %s: %08X\n",
					registerNames[i], (unsigned int)exceptionRegisters[i],
					registerNames[i+8], (unsigned int)exceptionRegisters[i+8]);
	}
	iprintf("\n");
	stack = (u32 *)exceptionRegisters[13];
	for ( i=0; i<10; i++ ) {
		iprintf( "\x1b[%d;2H%08X: %08X %08X", i + 14, (u32)&stack[i*2],stack[i*2], stack[(i*2)+1] );
	}
	while(1);

}

//---------------------------------------------------------------------------------
void setMzxExceptionHandler() {
//---------------------------------------------------------------------------------
	setExceptionHandler(mzxExceptionHandler) ;
}

#endif // CONFIG_NDS
