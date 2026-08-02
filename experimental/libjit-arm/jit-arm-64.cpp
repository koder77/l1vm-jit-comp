/*
 * This file jit-arm-64.cpp is part of L1vm.
 *
 * (c) Copyright Stefan Pietzonke (info@midnight-coding.de), 2023
 *
 * L1vm is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * L1vm is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with L1vm.  If not, see <http://www.gnu.org/licenses/>.
 */

// JIT-compiler uses asmjit library.
// Generates code for ARM 64 bit.

// work in progress!!!
// This should work:
// addi, subi, muli, divi
// addd, subd, muld, divd
// bandi, bori, bxori
// jmp, jmpi
// eqi, neqi, gri, lsi, greqi, lseqi
// eqd, neqd, grd, lsd, greqd, lseqd
//

#include <asmjit/a64.h>
#include <asmjit/core.h>

using namespace asmjit;
using namespace asmjit::a64;

JitRuntime rt;                          // Create a runtime specialized for JIT.
CodeHolder jcode;                   	// Holds code and relocation information.

#include "../vm/jit.h"

#include "../include/global.h"
#include "../include/stack.h"

class SimpleErrorHandler : public ErrorHandler {
public:
  Error err;

  inline SimpleErrorHandler() : err(Error::kOk) {}

  void handle_error(Error err, const char* message, BaseEmitter* origin) override {
    this->err = err;
    fprintf(stderr, "ERROR: %s\n", message);
  }
};

SimpleErrorHandler eh;

// This is type of function we will generate
typedef void (*Func)(void);

// #define DEBUG 1

#define MAXREGJIT_INT 4
#define MAXREGJIT_DOUBLE 16


#define OFFSET(x) ((x) * 8)

		#define RSI 	x0
		#define RDI 	x1
		#define R8 		x2
		#define R9 		x3
		#define R10		x4
		#define R11 	x5
		#define RBX 	x6
		#define RCX 	x7
		#define EAX 	x8
		#define EDX		x9
		// int register cache: x10 (0) to x13 (3), only used for cached int VM registers
		#define C0 		x10
		#define C1 		x11
		#define C2 		x12
		#define C3 		x13
		// scratch registers: must not be used by the int register cache
		#define SCRATCH 	x14
		#define SCRATCH2	x15
		// #define ST0     x86::fp7
		// #define ST1     x86::fp6

struct JIT_label
{
	asmjit::Label lab;
	S8 pos ALIGN;	// was S4
	S8 if_ ALIGN;
	S8 endif ALIGN;
};

struct JIT_label JIT_label[MAXJUMPLEN];

S8 JIT_label_ind ALIGN = -1;


// for storing VM registers
// S8 jit_regs[MAXREGJIT_INT];			// R8 (0) to R11 (3)
S8 jit_regsd[MAXREGJIT_DOUBLE]; 	// xmm0 (0) to xmm5 (5)

// double registers code ======================================================
S8 get_double_reg (S8 reg)
{
	S8 i ALIGN;

	for (i = 0; i < MAXREGJIT_DOUBLE; i++)
	{
		if (jit_regsd[i] == reg)
		{
			return (i);
		}
	}
	return (-1);	// jit register not found
}

S8 get_free_double_reg (S8 reg)
{
	S8 i ALIGN;

	for (i = 2; i < MAXREGJIT_DOUBLE; i++)
	{
		if (jit_regsd[i] == 0)
		{
			return (i);
		}
	}
	return (-1);	// no free jit register found
}

void free_double_reg (S8 cpu_reg)
{
	// free CPU register
	jit_regsd[cpu_reg] = 0;
}

S8 set_double_reg (S8 cpu_reg, S8 reg)
{
	jit_regsd[cpu_reg] = reg;
	return (1);
}

// int registers code ==========================================================
S8 jit_regs[MAXREGJIT_INT];			// C0 (0) to C3 (3): VM register number + 1, 0 = free
U1 jit_regs_dirty[MAXREGJIT_INT];	// 1 = value in CPU register changed, must be stored to memory before flush

a64::Gp int_cpu_reg (S8 idx)
{
	switch (idx)
	{
		case 0: return (C0);
		case 1: return (C1);
		case 2: return (C2);
		case 3: return (C3);
	}
	return (C0);
}

S8 get_int_reg (S8 reg)
{
	S8 i;

	for (i = 0; i < MAXREGJIT_INT; i++)
	{
		if (jit_regs[i] == reg + 1)
		{
			return (i);
		}
	}
	return (-1);	// jit register not found
}

S8 get_free_int_reg (void)
{
	S8 i;

	for (i = 0; i < MAXREGJIT_INT; i++)
	{
		if (jit_regs[i] == 0)
		{
			return (i);
		}
	}
	return (-1);	// no free jit register found
}

void set_int_reg (S8 cpu_reg, S8 reg)
{
	jit_regs[cpu_reg] = reg + 1;
}

void free_int_reg (S8 cpu_reg)
{
	jit_regs[cpu_reg] = 0;
	jit_regs_dirty[cpu_reg] = 0;
}

void flush_int_reg (Assembler &a, S8 cpu_reg)
{
	// store dirty CPU register value to VM register in memory
	if (jit_regs_dirty[cpu_reg] == 1)
	{
		a.str (int_cpu_reg (cpu_reg), ptr (RSI, OFFSET (jit_regs[cpu_reg] - 1)));
		jit_regs_dirty[cpu_reg] = 0;
	}
}

void flush_int_regs (Assembler &a)
{
	S8 i;

	for (i = 0; i < MAXREGJIT_INT; i++)
	{
		flush_int_reg (a, i);
	}
}

void clear_int_regs (void)
{
	S8 i;

	for (i = 0; i < MAXREGJIT_INT; i++)
	{
		jit_regs[i] = 0;
		jit_regs_dirty[i] = 0;
	}
}

S8 alloc_int_reg (Assembler &a, S8 reg, S8 avoid1, S8 avoid2, S8 avoid3)
{
	S8 cur;
	S8 i;

	cur = get_int_reg (reg);
	if (cur != -1)
	{
		return (cur);
	}

	cur = get_free_int_reg ();
	if (cur == -1)
	{
		// evict a CPU register, but do not evict registers holding avoid1, avoid2, avoid3
		for (i = 0; i < MAXREGJIT_INT; i++)
		{
			if (jit_regs[i] != avoid1 + 1 && jit_regs[i] != avoid2 + 1 && jit_regs[i] != avoid3 + 1 && jit_regs_dirty[i] == 0)
			{
				cur = i;
				break;
			}
		}

		if (cur == -1)
		{
			for (i = 0; i < MAXREGJIT_INT; i++)
			{
				if (jit_regs[i] != avoid1 + 1 && jit_regs[i] != avoid2 + 1 && jit_regs[i] != avoid3 + 1)
				{
					cur = i;
					break;
				}
			}
		}

		if (cur == -1)
		{
			cur = 0;
		}

		flush_int_reg (a, cur);
		free_int_reg (cur);
	}

	set_int_reg (cur, reg);
	jit_regs_dirty[cur] = 0;
	return (cur);
}

S8 load_int_reg (Assembler &a, S8 reg, S8 avoid1, S8 avoid2, S8 avoid3)
{
	S8 cur;

	cur = get_int_reg (reg);
	if (cur != -1)
	{
		return (cur);
	}

	cur = alloc_int_reg (a, reg, avoid1, avoid2, avoid3);
	a.ldr (int_cpu_reg (cur), ptr (RSI, OFFSET (reg)));
	jit_regs_dirty[cur] = 0;
	return (cur);
}

S8 result_int_reg (Assembler &a, S8 reg, S8 avoid1, S8 avoid2, S8 avoid3)
{
	S8 cur;

	cur = get_int_reg (reg);
	if (cur != -1)
	{
		jit_regs_dirty[cur] = 1;
		return (cur);
	}

	cur = alloc_int_reg (a, reg, avoid1, avoid2, avoid3);
	jit_regs_dirty[cur] = 1;
	return (cur);
}

extern "C" int jit_compiler (U1 *code, U1 *data, S8 *jumpoffs, S8 *regi, F8 *regd, U1 *sp, U1 *sp_top, U1 *sp_bottom, S8 start, S8 end, struct JIT_code *JIT_code, S8 JIT_code_ind, S8 code_size)
{
    S8 i ALIGN;
    S8 j ALIGN;
    S8 l ALIGN;
    S8 r1 ALIGN;
    S8 r2 ALIGN;
    S8 r3 ALIGN;
    S2 offset;
    U1 label_created;
    S8 label ALIGN;
    U1 run_jit = 0;
    U1 after_uncond_jmp = 0;

	S8 r1_d ALIGN;
	S8 r2_d ALIGN;
	S8 r3_d ALIGN;

	// CPU registers used for int register tracking
	S8 A ALIGN;
	S8 B ALIGN;
	S8 C ALIGN;

	// JMP opcode:
	U1 jump_ok = 0;

	S8 jump_l ALIGN = 0;
	S8 jump_label ALIGN = 0;
	S8 jump_target ALIGN = 0;

	jcode.init(rt.environment(), rt.cpu_features());
	jcode.set_error_handler(&eh);

	// FileLogger logger(stdout);
  	Assembler a(&jcode);           // Create and attach arm::Assembler to code.

	// a.setLogger(&logger);					// DEBUG: switch logger on!

	// set data, integer and double register bases
	a.mov (RBX, imm ((intptr_t)(void *) data)); /* data segment base: rbx */
	a.mov (RSI, imm ((intptr_t)(void *) regi));
    a.mov (RDI, imm ((intptr_t)(void *) regd));

	if (JIT_code_ind < 0 || JIT_code_ind >= MAXJITCODE)
	{
		printf ("JIT compiler: error code index out of range!\n");
		return (1);
	}

    /* initialize label pos */
	for (i = 0; i < MAXJUMPLEN; i++)
	{
		JIT_label[i].pos = -1;
	}

	// init jit_regs
	// CPU register
	for (i = 0; i <= 5; i++)
	{
		jit_regsd[i] = 0;
	}

	// init int register cache
	for (i = 0; i < MAXREGJIT_INT; i++)
	{
		jit_regs[i] = 0;
		jit_regs_dirty[i] = 0;
	}

    i = start;
    while (i <= end)
    {
		offset = 0;
		#if DEBUG
			printf ("opcode: %i\n", code[i]);
		#endif

        /* check if current opcode is on label */
		label_created = 0;

		// printf ("DEBUG: jit_compiler: code_size: %lli\n", code_size);

		for (j = start; j <= end; j++)
		{
			// printf ("DEBUG: jit_compiler: j: %lli\n", j);

			if (i == jumpoffs[j])
			{
				/* create label */

                for (l = 0; l < MAXJUMPLEN; l++)
				{
					if (JIT_label[l].pos == i)
					{
						label_created = 1;
						label = l;
						break;
					}
				}

				if (label_created == 0 && JIT_label_ind < MAXJUMPLEN)
				{
					JIT_label_ind++;
				}
				else
				{
					if (JIT_label_ind == MAXJUMPLEN)
					{
						printf ("JIT compiler: error label list full!\n");
						return (1);
					}
				}

				if (label_created == 0)
				{
					JIT_label[JIT_label_ind].lab = a.new_label ();
					JIT_label[JIT_label_ind].pos = jumpoffs[j];
					JIT_label[JIT_label_ind].if_ = -1;
					JIT_label[JIT_label_ind].endif = -1;
					a.bind (JIT_label[JIT_label_ind].lab);
				}
				else
				{
					a.bind (JIT_label[label].lab);

                    #if DEBUG
                        printf ("LABEL binded!\n");
                    #endif
				}

				// a label can be reached from different code paths: store all
				// dirty int registers to memory and reset int register tracking
				//
				// exception: if the label follows an unconditional JMP, the JMP
				// already flushed live state, and cache allocations made in the
				// (dead) skipped block are stale -- flushing here would clobber
				// memory with garbage
				if (!after_uncond_jmp)
				{
					flush_int_regs (a);
				}
				after_uncond_jmp = 0;
				clear_int_regs ();
            }
        }

		// set opcode offset for current opcode

        if (code[i] <= LSEQD)
		{
			offset = 4;
		}

		if (code[i] == JMP)
		{
            offset = 9;
        }
        if (code[i] == JMPI)
        {
            offset = 10;
        }

		if (code[i] == INCLSIJMPI || code[i] == DECGRIJMPI)
		{
			offset = 11;
		}

		if (code[i] == JSR)
		{
			offset = 9;
		}

		if (offset == 0)
		{
			// not set yet, do it!

			switch (code[i])
			{
				case STPUSHB:
				case STPOPB:
				case STPUSHI:
				case STPOPI:
				case STPUSHD:
				case STPOPD:
					offset = 2;
					break;

				case LOADA:
				case LOADD:
					offset = 18;
					break;

				case INTR0:
				case INTR1:
					offset = 5;
					break;

				case MOVI:
				case MOVD:
					offset = 3;
					break;

				case LOADL:
					offset = 10;
					break;

				case JMPA:
					offset = 2;
					break;

				case JSRA:
					offset = 2;
					break;

				case RTS:
					offset = 1;
					break;

				case LOAD:
					offset = 18;
					break;

				case NOTI:
					offset = 3;
					break;
			}
		}

		if (offset == 0)
		{
			// INTERNAL ERROR: no offset found!!!
			printf ("FATAL error: JIT compiler: setting jump offset failed! opcode: %i\n", code[i]);
            return (1);
		}

		// check if current opcode is in JIT-compiler:

		switch (code[i])
        {
			// ADDI, SUBI, MULI, DIVI ==================================================
            case ADDI:
			case SUBI:
			case MULI:
			case DIVI:
                r1 = code[i + 1];
                r2 = code[i + 2];
                r3 = code[i + 3];

				#if DEBUG
					printf ("JIT-compiler: int opcode: %i: R1 = %lli, R2 = %lli, R3 = %lli\n", code[i], r1, r2, r3);

					switch (code[i])
					{
						case ADDI:
							printf ("ADDI\n\n");
							break;

						case SUBI:
							printf ("SUBI\n\n");
							break;

						case MULI:
							printf ("MULI\n\n");
							break;

						case DIVI:
							printf ("DIVI\n\n");
							break;
					}
				#endif

	            A = load_int_reg (a, r1, r2, r3, -1);
	            B = load_int_reg (a, r2, r1, r3, -1);
	            C = result_int_reg (a, r3, r1, r2, -1);

				switch (code[i])
				{
					case ADDI:
						a.add (int_cpu_reg (C), int_cpu_reg (A), int_cpu_reg (B));
						break;

					case SUBI:
						a.sub (int_cpu_reg (C), int_cpu_reg (A), int_cpu_reg (B));
						break;

					case MULI:
						a.mul (int_cpu_reg (C), int_cpu_reg (A), int_cpu_reg (B));
						break;

					case DIVI:
						a.sdiv (int_cpu_reg (C), int_cpu_reg (A), int_cpu_reg (B));
						break;
				}

				run_jit = 1;
				break;

			// ADDD, SUBD, MULD, DIVD ==================================================
			case ADDD:
			case SUBD:
			case MULD:
			case DIVD:
				// double precision floating point using SSE
				r1 = code[i + 1];
				r2 = code[i + 2];
				r3 = code[i + 3];

				#if DEBUG
					printf ("JIT-compiler: double opcode: %i: R1 = %lli, R2 = %lli, R3 = %lli\n", code[i], r1, r2, r3);

					switch (code[i])
					{
						case ADDD:
							printf ("ADDD\n\n");
							break;

						case SUBD:
							printf ("SUBD\n\n");
							break;

						case MULD:
							printf ("MULD\n\n");
							break;

						case DIVD:
							printf ("DIVD\n\n");
							break;
					}
				#endif

				/*
				r1_d = get_double_reg (r1);
				if (r1_d == -1)
				{
					a.ldr (d0, ptr (RDI, OFFSET(r1)));
					set_double_reg (0, r1);
				}

				r2_d = get_double_reg (r2);
				if (r2_d == -1)
				{
					a.ldr (d1, ptr (RDI, OFFSET(r2)));
                }
                */

                a.ldr (d0, ptr (RDI, OFFSET(r1)));
                a.ldr (d1, ptr (RDI, OFFSET(r2)));

				switch (code[i])
				{
					case ADDD:
						a.fadd (d2, d0, d1);
						break;

					case SUBD:
						a.fsub (d2, d0, d1);
						break;

					case MULD:
						a.fmul (d2, d0, d1);
						break;

					case DIVD:
						a.fdiv (d2, d0, d1);
						break;
				}

				a.str (d2, ptr (RDI, OFFSET(r3)));

				/* free_double_reg (r1);
				*  free_double_reg (r2);
				*/

				run_jit = 1;
				break;

			// LOGICAL OPCODES =================================================
			 case BANDI:
				#if DEBUG
				printf ("JIT-compiler: opcode: %i: R1 = %lli, R2 = %lli, R3 = %lli\n", code[i], r1, r2, r3);
				printf ("BANDI\n\n");
				#endif
				r1 = code[i + 1];
				r2 = code[i + 2];
				r3 = code[i + 3];

				A = load_int_reg (a, r1, r2, r3, -1);
				B = load_int_reg (a, r2, r1, r3, -1);
				C = result_int_reg (a, r3, r1, r2, -1);

				a.ands (int_cpu_reg (C), int_cpu_reg (A), int_cpu_reg (B));

				run_jit = 1;
				break;

			case BORI:
				#if DEBUG
				printf ("JIT-compiler: opcode: %i: R1 = %lli, R2 = %lli, R3 = %lli\n", code[i], r1, r2, r3);
				printf ("BORI\n\n");
				#endif
				r1 = code[i + 1];
				r2 = code[i + 2];
				r3 = code[i + 3];

				A = load_int_reg (a, r1, r2, r3, -1);
				B = load_int_reg (a, r2, r1, r3, -1);
				C = result_int_reg (a, r3, r1, r2, -1);

				a.orr (int_cpu_reg (C), int_cpu_reg (A), int_cpu_reg (B));

				run_jit = 1;
				break;

			case BXORI:
				#if DEBUG
				printf ("JIT-compiler: opcode: %i: R1 = %lli, R2 = %lli, R3 = %lli\n", code[i], r1, r2, r3);
				printf ("BXORI\n\n");
				#endif
				r1 = code[i + 1];
				r2 = code[i + 2];
				r3 = code[i + 3];

				A = load_int_reg (a, r1, r2, r3, -1);
				B = load_int_reg (a, r2, r1, r3, -1);
				C = result_int_reg (a, r3, r1, r2, -1);

				a.eor (int_cpu_reg (C), int_cpu_reg (A), int_cpu_reg (B));

				run_jit = 1;
				break;

			case MODI:
				#if DEBUG
				printf ("JIT-compiler: opcode: %i: R1 = %lli, R2 = %lli, R3 = %lli\n", code[i], r1, r2, r3);
				printf ("MODI\n\n");
				#endif
				r1 = code[i + 1];
				r2 = code[i + 2];
				r3 = code[i + 3];

				A = load_int_reg (a, r1, r2, r3, -1);
				B = load_int_reg (a, r2, r1, r3, -1);
				C = result_int_reg (a, r3, r1, r2, -1);

				a.sdiv (SCRATCH, int_cpu_reg (A), int_cpu_reg (B));		/* SCRATCH = A / B */
				a.mul (SCRATCH, SCRATCH, int_cpu_reg (B));				/* SCRATCH = (A / B) * B */
				a.sub (int_cpu_reg (C), int_cpu_reg (A), SCRATCH);		/* C = A - (A / B) * B */

				run_jit = 1;
				break;

            // COMPARE OPCODES INT =====================================================
			case EQI:
				#if DEBUG
				printf ("JIT-compiler: opcode: %i: R1 = %lli, R2 = %lli, R3 = %lli\n", code[i], r1, r2, r3);
				printf ("EQI\n\n");
				#endif

				r1 = code[i + 1];
				r2 = code[i + 2];
				r3 = code[i + 3];

				A = load_int_reg (a, r1, r2, r3, -1);
				B = load_int_reg (a, r2, r1, r3, -1);
				C = result_int_reg (a, r3, r1, r2, -1);

				a.cmp (int_cpu_reg (A), int_cpu_reg (B));		// compare A, B
				a.cset (int_cpu_reg (C), CondCode::kEQ);		// C = 1 if A == B, else 0

				run_jit = 1;
				break;

			case NEQI:
				#if DEBUG
				printf ("JIT-compiler: opcode: %i: R1 = %lli, R2 = %lli, R3 = %lli\n", code[i], r1, r2, r3);
				printf ("NEQI\n\n");
				#endif

				r1 = code[i + 1];
				r2 = code[i + 2];
				r3 = code[i + 3];

				A = load_int_reg (a, r1, r2, r3, -1);
				B = load_int_reg (a, r2, r1, r3, -1);
				C = result_int_reg (a, r3, r1, r2, -1);

				a.cmp (int_cpu_reg (A), int_cpu_reg (B));		// compare A, B
				a.cset (int_cpu_reg (C), CondCode::kNE);		// C = 1 if A != B, else 0

				run_jit = 1;
				break;

			case GRI:
				#if DEBUG
				printf ("JIT-compiler: opcode: %i: R1 = %lli, R2 = %lli, R3 = %lli\n", code[i], r1, r2, r3);
				printf ("GRI\n\n");
				#endif

				r1 = code[i + 1];
				r2 = code[i + 2];
				r3 = code[i + 3];

				A = load_int_reg (a, r1, r2, r3, -1);
				B = load_int_reg (a, r2, r1, r3, -1);
				C = result_int_reg (a, r3, r1, r2, -1);

				a.cmp (int_cpu_reg (A), int_cpu_reg (B));		// compare A, B
				a.cset (int_cpu_reg (C), CondCode::kGT);		// C = 1 if A > B, else 0

				run_jit = 1;
				break;

			case LSI:
				#if DEBUG
				printf ("JIT-compiler: opcode: %i: R1 = %lli, R2 = %lli, R3 = %lli\n", code[i], r1, r2, r3);
				printf ("LSI\n\n");
				#endif

				r1 = code[i + 1];
				r2 = code[i + 2];
				r3 = code[i + 3];

				A = load_int_reg (a, r1, r2, r3, -1);
				B = load_int_reg (a, r2, r1, r3, -1);
				C = result_int_reg (a, r3, r1, r2, -1);

				a.cmp (int_cpu_reg (A), int_cpu_reg (B));		// compare A, B
				a.cset (int_cpu_reg (C), CondCode::kLT);		// C = 1 if A < B, else 0

				run_jit = 1;
				break;

			case GREQI:
				#if DEBUG
				printf ("JIT-compiler: opcode: %i: R1 = %lli, R2 = %lli, R3 = %lli\n", code[i], r1, r2, r3);
				printf ("GREQI\n\n");
				#endif

				r1 = code[i + 1];
				r2 = code[i + 2];
				r3 = code[i + 3];

				A = load_int_reg (a, r1, r2, r3, -1);
				B = load_int_reg (a, r2, r1, r3, -1);
				C = result_int_reg (a, r3, r1, r2, -1);

				a.cmp (int_cpu_reg (A), int_cpu_reg (B));		// compare A, B
				a.cset (int_cpu_reg (C), CondCode::kGE);		// C = 1 if A >= B, else 0

				run_jit = 1;
				break;

			case LSEQI:
				#if DEBUG
				printf ("JIT-compiler: opcode: %i: R1 = %lli, R2 = %lli, R3 = %lli\n", code[i], r1, r2, r3);
				printf ("LSEQI\n\n");
				#endif

				r1 = code[i + 1];
				r2 = code[i + 2];
				r3 = code[i + 3];

				A = load_int_reg (a, r1, r2, r3, -1);
				B = load_int_reg (a, r2, r1, r3, -1);
				C = result_int_reg (a, r3, r1, r2, -1);

				a.cmp (int_cpu_reg (A), int_cpu_reg (B));		// compare A, B
				a.cset (int_cpu_reg (C), CondCode::kLE);		// C = 1 if A <= B, else 0

				run_jit = 1;
				break;

            // COMPARE OPCODES DOUBLE ==================================================
			case EQD:
				#if DEBUG
				printf ("JIT-compiler: opcode: %i: R1 = %lli, R2 = %lli, R3 = %lli\n", code[i], r1, r2, r3);
				printf ("EQD\n\n");
				#endif

				r1 = code[i + 1];
				r2 = code[i + 2];
				r3 = code[i + 3];

				// double compare writes result to int register r3: store all dirty
				// int registers to memory and reset int register tracking
				flush_int_regs (a);
				clear_int_regs ();

				a.ldr (d0, ptr (RDI, OFFSET(r1)));
                a.ldr (d1, ptr (RDI, OFFSET(r2)));

				a.fcmp (d0, d1);

				// set label for JUMP equal
				if (JIT_label_ind < MAXJUMPLEN)
				{
					JIT_label_ind++;
				}
				else
				{
					if (JIT_label_ind == MAXJUMPLEN)
					{
						printf ("JIT compiler: error label list full!\n");
						return (1);
					}
				}

				JIT_label[JIT_label_ind].lab = a.new_label ();
				JIT_label[JIT_label_ind].pos = jumpoffs[j];
				JIT_label[JIT_label_ind].if_ = -1;
				JIT_label[JIT_label_ind].endif = -1;

				a.b_eq (JIT_label[JIT_label_ind].lab);		// jump equal

				// code for not equal than
				a.mov (R8, Imm (0));
                a.str (R8, ptr (RSI, OFFSET(r3)));

				// set label for jump equal

				// set label for JUMP END
				if (label_created == 0 && JIT_label_ind < MAXJUMPLEN)
				{
					JIT_label_ind++;
				}
				else
				{
					if (JIT_label_ind == MAXJUMPLEN)
					{
						printf ("JIT compiler: error label list full!\n");
						return (1);
					}
				}

				JIT_label[JIT_label_ind].lab = a.new_label ();
				JIT_label[JIT_label_ind].pos = jumpoffs[j];
				JIT_label[JIT_label_ind].if_ = -1;
				JIT_label[JIT_label_ind].endif = -1;

				a.b (JIT_label[JIT_label_ind].lab);

				a.bind (JIT_label[JIT_label_ind - 1].lab);	// set label for jmp equal

				// code for equal than
				a.mov (R8, Imm (1));
                a.str (R8, ptr (RSI, OFFSET(r3)));

				a.bind (JIT_label[JIT_label_ind].lab);		// set label for equal jump

				run_jit = 1;
				break;

			case NEQD:
				#if DEBUG
				printf ("JIT-compiler: opcode: %i: R1 = %lli, R2 = %lli, R3 = %lli\n", code[i], r1, r2, r3);
				printf ("EQD\n\n");
				#endif

				r1 = code[i + 1];
				r2 = code[i + 2];
				r3 = code[i + 3];

				// double compare writes result to int register r3: store all dirty
				// int registers to memory and reset int register tracking
				flush_int_regs (a);
				clear_int_regs ();

				a.ldr (d0, ptr (RDI, OFFSET(r1)));
                a.ldr (d1, ptr (RDI, OFFSET(r2)));

				a.fcmp (d0, d1);

				// set label for JUMP equal
				if (JIT_label_ind < MAXJUMPLEN)
				{
					JIT_label_ind++;
				}
				else
				{
					if (JIT_label_ind == MAXJUMPLEN)
					{
						printf ("JIT compiler: error label list full!\n");
						return (1);
					}
				}

				JIT_label[JIT_label_ind].lab = a.new_label ();
				JIT_label[JIT_label_ind].pos = jumpoffs[j];
				JIT_label[JIT_label_ind].if_ = -1;
				JIT_label[JIT_label_ind].endif = -1;

				a.b_eq (JIT_label[JIT_label_ind].lab);		// jump equal

				// code for not equal than
				a.mov (R8, Imm (1));
                a.str (R8, ptr (RSI, OFFSET(r3)));

				// set label for jump equal

				// set label for JUMP END
				if (label_created == 0 && JIT_label_ind < MAXJUMPLEN)
				{
					JIT_label_ind++;
				}
				else
				{
					if (JIT_label_ind == MAXJUMPLEN)
					{
						printf ("JIT compiler: error label list full!\n");
						return (1);
					}
				}

				JIT_label[JIT_label_ind].lab = a.new_label ();
				JIT_label[JIT_label_ind].pos = jumpoffs[j];
				JIT_label[JIT_label_ind].if_ = -1;
				JIT_label[JIT_label_ind].endif = -1;

				a.b (JIT_label[JIT_label_ind].lab);

				a.bind (JIT_label[JIT_label_ind - 1].lab);	// set label for jmp equal

				// code for equal than
				a.mov (R8, Imm (0));
                a.str (R8, ptr (RSI, OFFSET(r3)));

				a.bind (JIT_label[JIT_label_ind].lab);		// set label for equal jump

				run_jit = 1;
				break;

			case GRD:
				#if DEBUG
				printf ("JIT-compiler: opcode: %i: R1 = %lli, R2 = %lli, R3 = %lli\n", code[i], r1, r2, r3);
				printf ("EQD\n\n");
				#endif

				r1 = code[i + 1];
				r2 = code[i + 2];
				r3 = code[i + 3];

				// double compare writes result to int register r3: store all dirty
				// int registers to memory and reset int register tracking
				flush_int_regs (a);
				clear_int_regs ();

				a.ldr (d0, ptr (RDI, OFFSET(r1)));
				a.ldr (d1, ptr (RDI, OFFSET(r2)));

				a.fcmp (d0, d1);

				// set label for JUMP greater
				if (JIT_label_ind < MAXJUMPLEN)
				{
					JIT_label_ind++;
				}
				else
				{
					if (JIT_label_ind == MAXJUMPLEN)
					{
						printf ("JIT compiler: error label list full!\n");
						return (1);
					}
				}

				JIT_label[JIT_label_ind].lab = a.new_label ();
				JIT_label[JIT_label_ind].pos = jumpoffs[j];
				JIT_label[JIT_label_ind].if_ = -1;
				JIT_label[JIT_label_ind].endif = -1;

				a.b_gt (JIT_label[JIT_label_ind].lab);		// jump greater

				// code for not equal than
				a.mov (R8, Imm (0));
                a.str (R8, ptr (RSI, OFFSET(r3)));

				// set label for jump equal

				// set label for JUMP END
				if (label_created == 0 && JIT_label_ind < MAXJUMPLEN)
				{
					JIT_label_ind++;
				}
				else
				{
					if (JIT_label_ind == MAXJUMPLEN)
					{
						printf ("JIT compiler: error label list full!\n");
						return (1);
					}
				}

				JIT_label[JIT_label_ind].lab = a.new_label ();
				JIT_label[JIT_label_ind].pos = jumpoffs[j];
				JIT_label[JIT_label_ind].if_ = -1;
				JIT_label[JIT_label_ind].endif = -1;

				a.b (JIT_label[JIT_label_ind].lab);

				a.bind (JIT_label[JIT_label_ind - 1].lab);	// set label for jmp equal

				// code for equal than
				a.mov (R8, Imm (1));
                a.str (R8, ptr (RSI, OFFSET(r3)));

				a.bind (JIT_label[JIT_label_ind].lab);		// set label for equal jump

				run_jit = 1;
				break;

			case LSD:
				#if DEBUG
				printf ("JIT-compiler: opcode: %i: R1 = %lli, R2 = %lli, R3 = %lli\n", code[i], r1, r2, r3);
				printf ("EQD\n\n");
				#endif

				r1 = code[i + 1];
				r2 = code[i + 2];
				r3 = code[i + 3];

				// double compare writes result to int register r3: store all dirty
				// int registers to memory and reset int register tracking
				flush_int_regs (a);
				clear_int_regs ();

				a.ldr (d0, ptr (RDI, OFFSET(r1)));
				a.ldr (d1, ptr (RDI, OFFSET(r2)));

				a.fcmp (d0, d1);

				// set label for JUMP lower
				if (JIT_label_ind < MAXJUMPLEN)
				{
					JIT_label_ind++;
				}
				else
				{
					if (JIT_label_ind == MAXJUMPLEN)
					{
						printf ("JIT compiler: error label list full!\n");
						return (1);
					}
				}

				JIT_label[JIT_label_ind].lab = a.new_label ();
				JIT_label[JIT_label_ind].pos = jumpoffs[j];
				JIT_label[JIT_label_ind].if_ = -1;
				JIT_label[JIT_label_ind].endif = -1;

				a.b_lt (JIT_label[JIT_label_ind].lab);		// jump lower

				// code for not equal than
				a.mov (R8, Imm (0));
                a.str (R8, ptr (RSI, OFFSET(r3)));

				// set label for jump equal

				// set label for JUMP END
				if (label_created == 0 && JIT_label_ind < MAXJUMPLEN)
				{
					JIT_label_ind++;
				}
				else
				{
					if (JIT_label_ind == MAXJUMPLEN)
					{
						printf ("JIT compiler: error label list full!\n");
						return (1);
					}
				}

				JIT_label[JIT_label_ind].lab = a.new_label ();
				JIT_label[JIT_label_ind].pos = jumpoffs[j];
				JIT_label[JIT_label_ind].if_ = -1;
				JIT_label[JIT_label_ind].endif = -1;

				a.b (JIT_label[JIT_label_ind].lab);

				a.bind (JIT_label[JIT_label_ind - 1].lab);	// set label for jmp equal

				// code for equal than
				a.mov (R8, Imm (1));
				a.str (R8, ptr (RSI, OFFSET(r3)));

				a.bind (JIT_label[JIT_label_ind].lab);		// set label for equal jump

				run_jit = 1;
				break;

			case GREQD:
				#if DEBUG
				printf ("JIT-compiler: opcode: %i: R1 = %lli, R2 = %lli, R3 = %lli\n", code[i], r1, r2, r3);
				printf ("EQD\n\n");
				#endif

				r1 = code[i + 1];
				r2 = code[i + 2];
				r3 = code[i + 3];

				// double compare writes result to int register r3: store all dirty
				// int registers to memory and reset int register tracking
				flush_int_regs (a);
				clear_int_regs ();

				a.ldr (d0, ptr (RDI, OFFSET(r1)));
                a.ldr (d1, ptr (RDI, OFFSET(r2)));

				a.fcmp (d0, d1);

				// set label for JUMP equal
				if (JIT_label_ind < MAXJUMPLEN)
				{
					JIT_label_ind++;
				}
				else
				{
					if (JIT_label_ind == MAXJUMPLEN)
					{
						printf ("JIT compiler: error label list full!\n");
						return (1);
					}
				}

				JIT_label[JIT_label_ind].lab = a.new_label ();
				JIT_label[JIT_label_ind].pos = jumpoffs[j];
				JIT_label[JIT_label_ind].if_ = -1;
				JIT_label[JIT_label_ind].endif = -1;

				a.b_ge (JIT_label[JIT_label_ind].lab);		// jump equal

				// code for not equal than
				a.mov (R8, Imm (0));
				a.str (R8, ptr (RSI, OFFSET(r3)));

				// set label for jump equal

				// set label for JUMP END
				if (label_created == 0 && JIT_label_ind < MAXJUMPLEN)
				{
					JIT_label_ind++;
				}
				else
				{
					if (JIT_label_ind == MAXJUMPLEN)
					{
						printf ("JIT compiler: error label list full!\n");
						return (1);
					}
				}

				JIT_label[JIT_label_ind].lab = a.new_label ();
				JIT_label[JIT_label_ind].pos = jumpoffs[j];
				JIT_label[JIT_label_ind].if_ = -1;
				JIT_label[JIT_label_ind].endif = -1;

				a.b (JIT_label[JIT_label_ind].lab);

				a.bind (JIT_label[JIT_label_ind - 1].lab);	// set label for jmp equal

				// code for equal than
				a.mov (R8, Imm (1));
				a.str (R8, ptr (RSI, OFFSET(r3)));

				a.bind (JIT_label[JIT_label_ind].lab);		// set label for equal jump

				run_jit = 1;
				break;

			case LSEQD:
				#if DEBUG
				printf ("JIT-compiler: opcode: %i: R1 = %lli, R2 = %lli, R3 = %lli\n", code[i], r1, r2, r3);
				printf ("EQD\n\n");
				#endif

				r1 = code[i + 1];
				r2 = code[i + 2];
				r3 = code[i + 3];

				// double compare writes result to int register r3: store all dirty
				// int registers to memory and reset int register tracking
				flush_int_regs (a);
				clear_int_regs ();

				a.ldr (d0, ptr (RDI, OFFSET(r1)));
                a.ldr (d1, ptr (RDI, OFFSET(r2)));

				a.fcmp (d0, d1);

				// set label for JUMP equal
				if (JIT_label_ind < MAXJUMPLEN)
				{
					JIT_label_ind++;
				}
				else
				{
					if (JIT_label_ind == MAXJUMPLEN)
					{
						printf ("JIT compiler: error label list full!\n");
						return (1);
					}
				}

				JIT_label[JIT_label_ind].lab = a.new_label ();
				JIT_label[JIT_label_ind].pos = jumpoffs[j];
				JIT_label[JIT_label_ind].if_ = -1;
				JIT_label[JIT_label_ind].endif = -1;

				a.b_le (JIT_label[JIT_label_ind].lab);		// jump equal

				// code for not equal than
				a.mov (R8, Imm (0));
				a.str (R8, ptr (RSI, OFFSET(r3)));

				// set label for jump equal

				// set label for JUMP END
				if (label_created == 0 && JIT_label_ind < MAXJUMPLEN)
				{
					JIT_label_ind++;
				}
				else
				{
					if (JIT_label_ind == MAXJUMPLEN)
					{
						printf ("JIT compiler: error label list full!\n");
						return (1);
					}
				}

				JIT_label[JIT_label_ind].lab = a.new_label ();
				JIT_label[JIT_label_ind].pos = jumpoffs[j];
				JIT_label[JIT_label_ind].if_ = -1;
				JIT_label[JIT_label_ind].endif = -1;

				a.b (JIT_label[JIT_label_ind].lab);

				a.bind (JIT_label[JIT_label_ind - 1].lab);	// set label for jmp equal

				// code for equal than
				//
				a.mov (R8, Imm (1));
				a.str (R8, ptr (RSI, OFFSET(r3)));

				a.bind (JIT_label[JIT_label_ind].lab);		// set label for equal jump

				run_jit = 1;
				break;


            // JUMP OPCODES ============================================================
			case JMP:
				#if DEBUG
					printf ("JMP\n\n");
				#endif

				r1 = code[i + 1];
				r2 = code[i + 2];
				r3 = code[i + 3];

				// store all dirty int registers to memory and reset int register
				// tracking before the jump: the jump target may be reached from
				// other code paths, so memory must be current at the jump target
				flush_int_regs (a);
				clear_int_regs ();

				label_created = 0;

				// printf ("DEBUG: JIT: JMP...\n");

				jump_target = jumpoffs[i];

				for (jump_l = 0; jump_l < MAXJUMPLEN; jump_l++)
				{
					if (JIT_label[jump_l].pos == jump_target)
					{
						// printf ("DEBUG: found jump_target\n");
						label_created = 1;
						jump_label = jump_l;
						break;
					}
				}

				if (label_created == 1)
				{
					// printf ("DEBUG: jit: jmpi: jump_label: %lli\n, epos: %lli\n", jump_label, i);
					a.b (JIT_label[jump_label].lab);
					goto jump_end;
				}
				else
				{
					if (JIT_label_ind < MAXJUMPLEN)
					{
						JIT_label_ind++;
					}
					else
					{
						if (JIT_label_ind == MAXJUMPLEN)
						{
							printf ("JIT compiler: error label list full!\n");
							return (1);
						}
					}

					// set jump  pos to JIT_label
					JIT_label[JIT_label_ind].lab = a.new_label ();
					JIT_label[JIT_label_ind].pos = jump_target;
					JIT_label[JIT_label_ind].if_ = -1;
					JIT_label[JIT_label_ind].endif = -1;

					a.b (JIT_label[JIT_label_ind].lab);
					goto jump_end;
				}
				if (jump_ok == 1)
				{
					goto jump_end;
				}

				jump_end:
				run_jit = 1;
				break;

			case JMPI:
				// DEBUG: OK: prog/jit-test-loop.l1com
				#if DEBUG
					printf ("JMPI\n\n");
				#endif

				r1 = code[i + 1];
				r2 = code[i + 2];
				r3 = code[i + 3];

				A = load_int_reg (a, r1, -1, -1, -1);

				a.cmp (int_cpu_reg (A), Imm (1));		// compare A, one

				// store all dirty int registers to memory and reset int register
				// tracking before the conditional jump: the jump target may be
				// reached from other code paths (e.g. a loop back edge), so
				// memory must be current at the jump target
				flush_int_regs (a);
				clear_int_regs ();

				label_created = 0;

				// printf ("DEBUG: JIT: JMPI...\n");

				jump_target = jumpoffs[i];

				for (jump_l = 0; jump_l < MAXJUMPLEN; jump_l++)
				{
					if (JIT_label[jump_l].pos == jump_target)
					{
						// printf ("DEBUG: found jump_target\n");
						label_created = 1;
						jump_label = jump_l;
						break;
					}
				}

				if (label_created == 1)
				{
					// printf ("DEBUG: jit: jmpi: jump_label: %lli\n, epos: %lli\n", jump_label, i);
					a.b_eq (JIT_label[jump_label].lab);
					goto jumpi_end;
				}
				else
				{
					if (JIT_label_ind < MAXJUMPLEN)
					{
						JIT_label_ind++;
					}
					else
					{
						if (JIT_label_ind == MAXJUMPLEN)
						{
							printf ("JIT compiler: error label list full!\n");
							return (1);
						}
					}

					// set jump  pos to JIT_label
					JIT_label[JIT_label_ind].lab = a.new_label ();
					JIT_label[JIT_label_ind].pos = jump_target;
					JIT_label[JIT_label_ind].if_ = -1;
					JIT_label[JIT_label_ind].endif = -1;

					a.b_eq (JIT_label[JIT_label_ind].lab);
					goto jumpi_end;
				}
				if (jump_ok == 1)
				{
					goto jumpi_end;
				}

				jumpi_end:
				run_jit = 1;
				break;

            // MOVI, MOVD ==============================================================
			case MOVI:
				#if DEBUG
				printf ("JIT-compiler: opcode: %i: R1 = %lli, R2 = %lli, R3 = %lli\n", code[i], r1, r2, r3);
				printf ("MOVI\n\n");
				#endif

				r1 = code[i + 1];
				r2 = code[i + 2];

				if (r1 != r2)
				{
					// load source register, then copy it into the destination register
					A = load_int_reg (a, r1, r2, -1, -1);
					C = result_int_reg (a, r2, r1, -1, -1);

					// if C == A, the CPU register physically already holds the
					// source value, so tagging the slot as r2 is the copy itself
					if (C != A)
					{
						a.mov (int_cpu_reg (C), int_cpu_reg (A));
					}
				}

				run_jit = 1;
				break;

			case MOVD:
				#if DEBUG
				printf ("JIT-compiler: opcode: %i: R1 = %lli, R2 = %lli, R3 = %lli\n", code[i], r1, r2, r3);
				printf ("MOVD\n\n");
				#endif

				r1 = code[i + 1];
				r2 = code[i + 2];

				a.ldr (d0, ptr (RDI, OFFSET(r1)));
				a.fmov (d1, d0);
				a.str (d1, ptr (RDI, OFFSET(r2)));

				run_jit = 1;
				break;

			// LOADL =====================================================================
			// load 64 bit literal into integer register: regi[r2] = arg1
			case LOADL:
				#if DEBUG
				printf ("LOADL: arg1 = %lli, R2 = %lli\n", r1, r2);
				#endif

				memcpy (&r1, &code[i + 1], sizeof (uint64_t));
				r2 = code[i + 9];

				C = result_int_reg (a, r2, -1, -1, -1);

				a.mov (int_cpu_reg (C), imm (r1));

				run_jit = 1;
				break;

			// LOAD ======================================================================
			// compute address: regi[r3] = arg1 + arg2
			case LOAD:
				#if DEBUG
				printf ("LOAD: arg1 = %lli, arg2 = %lli, R3 = %lli\n", r1, r2, r3);
				#endif

				memcpy (&r1, &code[i + 1], sizeof (uint64_t));
				memcpy (&r2, &code[i + 9], sizeof (uint64_t));
				r3 = code[i + 17];

				C = result_int_reg (a, r3, -1, -1, -1);

				a.mov (int_cpu_reg (C), imm (r1));		/* C = arg1 */
				a.mov (SCRATCH, imm (r2));
				a.add (int_cpu_reg (C), int_cpu_reg (C), SCRATCH);		/* C = arg1 + arg2 */

				run_jit = 1;
				break;

			// LOADA =====================================================================
			// load 64 bit int from data segment: regi[r3] = data[arg1 + arg2]
			case LOADA:
				#if DEBUG
				printf ("LOADA: arg1 = %lli, arg2 = %lli, R3 = %lli\n", r1, r2, r3);
				#endif

				memcpy (&r1, &code[i + 1], sizeof (uint64_t));
				memcpy (&r2, &code[i + 9], sizeof (uint64_t));
				r3 = code[i + 17];

				C = result_int_reg (a, r3, -1, -1, -1);

				a.mov (int_cpu_reg (C), imm (r1));		/* C = arg1 */
				a.mov (SCRATCH, imm (r2));
				a.add (int_cpu_reg (C), int_cpu_reg (C), SCRATCH);		/* C = arg1 + arg2 */
				a.ldr (int_cpu_reg (C), ptr (RBX, int_cpu_reg (C)));	/* load 64 bit from data segment */

				run_jit = 1;
				break;

			// LOADD =====================================================================
			// load 64 bit double from data segment: regd[r3] = data[arg1 + arg2]
			case LOADD:
				#if DEBUG
				printf ("LOADD: arg1 = %lli, arg2 = %lli, R3 = %lli\n", r1, r2, r3);
				#endif

				memcpy (&r1, &code[i + 1], sizeof (uint64_t));
				memcpy (&r2, &code[i + 9], sizeof (uint64_t));
				r3 = code[i + 17];

				a.mov (R8, imm (r1));
				a.mov (R9, imm (r2));
				a.add (R10, R8, R9);			/* R10 = arg1 + arg2 */
				a.ldr (R8, ptr (RBX, R10));		/* load 64 bit from data segment */
				a.str (R8, ptr (RDI, OFFSET(r3)));

				run_jit = 1;
				break;

			// PUSHB, PUSHW, PUSHDW, PUSHQW =============================================
			// load from data segment array: regi[r3] = data[regi[r1] + regi[r2]]
			case PUSHB:
			case PUSHW:
			case PUSHDW:
			case PUSHQW:
				r1 = code[i + 1];
				r2 = code[i + 2];
				r3 = code[i + 3];

				A = load_int_reg (a, r1, r2, r3, -1);	/* array base address */
				B = load_int_reg (a, r2, r1, r3, -1);	/* index */

				a.add (SCRATCH, int_cpu_reg (A), int_cpu_reg (B));	/* SCRATCH = base + index */

				C = result_int_reg (a, r3, r1, r2, -1);

				switch (code[i])
				{
					case PUSHB:
						a.ldrb (int_cpu_reg (C).w(), ptr (RBX, SCRATCH));
						break;

					case PUSHW:
						a.ldrh (int_cpu_reg (C).w(), ptr (RBX, SCRATCH));
						break;

					case PUSHDW:
						a.ldr (int_cpu_reg (C).w(), ptr (RBX, SCRATCH));
						break;

					case PUSHQW:
						a.ldr (int_cpu_reg (C), ptr (RBX, SCRATCH));
						break;
				}

				run_jit = 1;
				break;

			// PUSHD =========================================================================
			// load double from data segment array: regd[r3] = data[regi[r1] + regi[r2]]
			case PUSHD:
				r1 = code[i + 1];
				r2 = code[i + 2];
				r3 = code[i + 3];

				// reads int registers r1, r2 from memory: store all dirty int
				// registers to memory and reset int register tracking
				flush_int_regs (a);
				clear_int_regs ();

				a.ldr (R8, ptr (RSI, OFFSET(r1))); /* array base address */
				a.ldr (R9, ptr (RSI, OFFSET(r2))); /* index */
				a.add (R10, R8, R9);				/* R10 = base + index */

				a.ldr (R8, ptr (RBX, R10));
				a.str (R8, ptr (RDI, OFFSET(r3)));
				run_jit = 1;
				break;

			// PULLB, PULLW, PULLDW, PULLQW =================================================
			// store to data segment array: data[regi[r2] + regi[r3]] = regi[r1]
			case PULLB:
			case PULLW:
			case PULLDW:
			case PULLQW:
				r1 = code[i + 1];
				r2 = code[i + 2];
				r3 = code[i + 3];

				A = load_int_reg (a, r2, r1, r3, -1);	/* array base address */
				B = load_int_reg (a, r3, r1, r2, -1);	/* index */

				a.add (SCRATCH, int_cpu_reg (A), int_cpu_reg (B));	/* SCRATCH = base + index */

				C = load_int_reg (a, r1, r2, r3, -1);	/* value to store */

				switch (code[i])
				{
					case PULLB:
						a.strb (int_cpu_reg (C).w(), ptr (RBX, SCRATCH));
						break;

					case PULLW:
						a.strh (int_cpu_reg (C).w(), ptr (RBX, SCRATCH));
						break;

					case PULLDW:
						a.str (int_cpu_reg (C).w(), ptr (RBX, SCRATCH));
						break;

					case PULLQW:
						a.str (int_cpu_reg (C), ptr (RBX, SCRATCH));
						break;
				}

				run_jit = 1;
				break;

			// PULLD =========================================================================
			// store double to data segment array: data[regi[r2] + regi[r3]] = regd[r1]
			case PULLD:
				r1 = code[i + 1];
				r2 = code[i + 2];
				r3 = code[i + 3];

				// reads int registers r2, r3 from memory: store all dirty int
				// registers to memory and reset int register tracking
				flush_int_regs (a);
				clear_int_regs ();

				a.ldr (R9, ptr (RSI, OFFSET(r2))); /* array base address */
				a.ldr (R10, ptr (RSI, OFFSET(r3))); /* index */
				a.add (R9, R9, R10);				/* R9 = base + index */

				a.ldr (R8, ptr (RDI, OFFSET(r1))); /* value to store */
				a.str (R8, ptr (RBX, R9));
				run_jit = 1;
				break;

			// DEFAULT: output ERROR message if oopcode not found! =====================
			default:
                printf ("JIT compiler: UNKNOWN opcode: %i - exiting!\n", code[i]);
                return (1);
        }
        if (code[i] == JMP)
        {
            after_uncond_jmp = 1;
        }
        i = i + offset;
    }

    if (run_jit)
    {
		// store all remaining dirty int registers to memory: the interpreter
		// resumes reading regi[] from memory after run_jit returns
		flush_int_regs (a);

		// a.mov (R8, imm (0));   // R8 zero
        a.ret (x30);		// return to main program code

        // create JIT code function

        Func funcptr;

		// store JIT code:
        rt.add (&funcptr, &jcode);
        if ((bool) eh.err)
        {
            printf ("JIT compiler: code generation failed!\n");
            return (1);
        }

        JIT_code[JIT_code_ind].fn = (Func) funcptr;
        JIT_code[JIT_code_ind].used = 1;
        #if DEBUG
            printf ("JIT compiler: function saved.\n");
        #endif
	}
	return (0);
}

extern "C" int run_jit (S8 code, struct JIT_code *JIT_code)
{
	#if	 DEBUG
		printf ("run_jit: code: %lli\n", code);
	#endif

	if (code < 0 || code >= MAXJITCODE)
	{
		printf ("JIT compiler: FATAL ERROR! code index %lli out of range!!!\n", code);
		return (1);
	}

	if (JIT_code[code].used == 0)
	{
		printf ("JIT compiler: FATAL ERROR! code index %lli not compiled!\n", code);
		return (1);
	}

	Func func = JIT_code[code].fn;

    #if DEBUG
        printf ("run_jit: code address: %lli\n", (S8) func);
    #endif

    if (func == NULL)
	{
		printf ("JIT compiler: FATAL ERROR! NULL pointer code!!!\n");
		return (1);
	}

	// call JIT code function, stored in JIT_code[]
	JIT_code[code].fn();
	return (0);
}

extern "C" int free_jit_code (struct JIT_code *JIT_code, S8 JIT_code_ind)
{
	/* free all JIT code functions from memory */

	S4 i;

	for (i = 0; i < MAXJITCODE; i++)
	{
		if (JIT_code[i].used == 1)
		{
			rt.release((Func *) JIT_code[i].fn);
		}
	}

	return (0);
}
