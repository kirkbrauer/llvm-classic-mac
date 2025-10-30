/**
 * MacHeadersCompat.h - Compatibility layer for Classic Mac OS Universal Interfaces with Clang
 *
 * This header provides compatibility shims to allow the original Universal Interfaces 3.4
 * headers to compile with Clang for PowerPC Classic Mac OS targets, without modifying
 * the original headers.
 *
 * Include this header BEFORE any Mac OS headers:
 *   #include <MacHeadersCompat.h>
 *   #include <MacTypes.h>
 *   #include <MacMemory.h>
 *   // etc.
 *
 * Part of the LLVM PEF Linker project for Classic Mac OS PowerPC.
 */

#ifndef __MACHEADERSCOMPAT_H__
#define __MACHEADERSCOMPAT_H__

#if defined(__clang__) && !defined(__MACHEADERSCOMPAT_CONFIGURED__)
#define __MACHEADERSCOMPAT_CONFIGURED__

/*
 * =============================================================================
 * CONDITIONALMACROS.H BYPASS
 * =============================================================================
 *
 * We completely bypass the original ConditionalMacros.h by defining its include
 * guard, then providing all the defines it would have provided. This avoids:
 * 1. GCC MPW extensions (#cpu, #system) that Clang doesn't support
 * 2. Wrong PRAGMA_* values from the Apple GCC branch
 * 3. Unsupported #pragma options align=mac68k
 */

#ifndef __CONDITIONALMACROS__
#define __CONDITIONALMACROS__
#endif

/* Universal Interfaces version */
#define UNIVERSAL_INTERFACES_VERSION 0x0340

/*
 * =============================================================================
 * TARGET PLATFORM MACROS
 * =============================================================================
 */

/* CPU Architecture */
#define TARGET_CPU_PPC          1
#define TARGET_CPU_68K          0
#define TARGET_CPU_X86          0
#define TARGET_CPU_MIPS         0
#define TARGET_CPU_SPARC        0
#define TARGET_CPU_ALPHA        0

/* Operating System */
#define TARGET_OS_MAC           1
#define TARGET_OS_WIN32         0
#define TARGET_OS_UNIX          0

/* Runtime Environment */
#define TARGET_RT_MAC_CFM       1    /* Code Fragment Manager (PEF) */
#define TARGET_RT_MAC_MACHO     0    /* Mach-O (Mac OS X) */
#define TARGET_RT_MAC_68881     0    /* 68K math coprocessor */
#define TARGET_RT_BIG_ENDIAN    1    /* PowerPC is big-endian */
#define TARGET_RT_LITTLE_ENDIAN 0

/* Carbon Compatibility - We are targeting Classic Mac OS, not Carbon */
#define CALL_NOT_IN_CARBON      1    /* Include non-Carbon APIs */
#define TARGET_CARBON           0    /* Not targeting Carbon */

/*
 * =============================================================================
 * PRAGMA SUPPORT MACROS
 * =============================================================================
 */

/* Clang supports pack(push/pop) but NOT #pragma options align=mac68k */
#define PRAGMA_IMPORT           0
#define PRAGMA_STRUCT_ALIGN     0    /* Do NOT use #pragma options align */
#define PRAGMA_ONCE             1
#define PRAGMA_STRUCT_PACK      0
#define PRAGMA_STRUCT_PACKPUSH  1    /* We use pack(push, 2) instead */
#define PRAGMA_ENUM_PACK        0
#define PRAGMA_ENUM_ALWAYSINT   0
#define PRAGMA_ENUM_OPTIONS     0
#define PRAGMA_ALIGN_SUPPORTED  PRAGMA_STRUCT_PACKPUSH  /* For compatibility */

/* Set default structure packing for Mac OS (2-byte alignment) */
#pragma pack(push, 2)

/*
 * =============================================================================
 * FOUR CHARACTER CODE SUPPORT
 * =============================================================================
 */

#ifndef FOUR_CHAR_CODE
#define FOUR_CHAR_CODE(x) (x)
#endif

/*
 * =============================================================================
 * TYPE SUPPORT MACROS
 * =============================================================================
 */

#define TYPE_LONGLONG           1    /* Clang supports long long */
#define TYPE_EXTENDED           0    /* No 80-bit extended on PowerPC */
#define TYPE_LONGDOUBLE_IS_DOUBLE 1  /* long double == double on PPC */

#ifdef __cplusplus
#define TYPE_BOOL               1    /* C++ has native bool */
#else
#define TYPE_BOOL               0    /* C89 doesn't have bool */
#endif

/*
 * =============================================================================
 * CALLING CONVENTION SUPPORT
 * =============================================================================
 *
 * Classic Mac OS used "pascal" calling convention on 68K, but PowerPC CFM
 * actually uses C calling convention. The "pascal" keyword in function
 * declarations was for source compatibility; runtime RoutineDescriptors
 * handled the actual calling convention metadata.
 *
 * For PowerPC targets, we map pascal to cdecl (C calling convention).
 */

#define FUNCTION_PASCAL         0    /* Not using special pascal convention */
#define FUNCTION_DECLSPEC       0    /* Not using Windows __declspec */
#define FUNCTION_WIN32CC        0    /* Not using Windows calling conventions */

/* Replace pascal keyword with C calling convention attribute */
#ifndef pascal
#define pascal __attribute__((cdecl))
#endif

#ifndef __pascal
#define __pascal __attribute__((cdecl))
#endif

/* Other calling convention aliases sometimes used */
#ifndef _pascal
#define _pascal __attribute__((cdecl))
#endif

/*
 * =============================================================================
 * API TARGET VERSIONS
 * =============================================================================
 */

#define TARGET_API_MAC_OS8      1    /* Targeting Classic Mac OS (7-9) */
#define TARGET_API_MAC_CARBON   0    /* Not targeting Carbon */
#define TARGET_API_MAC_OSX      0    /* Not targeting Mac OS X */

/*
 * =============================================================================
 * DISABLE CONFLICTING HEADERS
 * =============================================================================
 */

/* Debugging.h has conflicts, disable it (per Retro68 approach) */
#define __DEBUGGING__

/*
 * =============================================================================
 * CALLBACK AND INLINE ASSEMBLY MACROS
 * =============================================================================
 */

/* Callback macros for function pointers - just use standard C on PowerPC CFM
 * These macros define function pointer TYPES, not function types.
 * Usage: typedef CALLBACK_API(long, ProcPtr)();
 * Expands to: typedef long (*ProcPtr)();
 */
#define CALLBACK_API(_type, _name)              _type (*_name)
#define CALLBACK_API_C(_type, _name)            _type (*_name)
#define CALLBACK_API_STDCALL(_type, _name)      _type (*_name)
#define CALLBACK_API_PASCAL(_type, _name)       _type pascal (*_name)

/* 68K register-based callback - takes parameters in register format */
#define CALLBACK_API_REGISTER68K(_type, _name, _params) _type _name _params

/* Universal Procedure Pointer (UPP) type macros */
#define STACK_UPP_TYPE(_procPtr)                _procPtr
#define REGISTER_UPP_TYPE(_procPtr)             _procPtr

/* Mixed mode and UPP support macros */
#define TVECTOR_UPP_TYPE(_procPtr)              _procPtr
#define OPAQUE_UPP_TYPES                        0

/* Inline assembly macros for 68K - not used on PowerPC, define as empty */
#define ONEWORDINLINE(w1)
#define TWOWORDINLINE(w1,w2)
#define THREEWORDINLINE(w1,w2,w3)
#define FOURWORDINLINE(w1,w2,w3,w4)
#define FIVEWORDINLINE(w1,w2,w3,w4,w5)
#define SIXWORDINLINE(w1,w2,w3,w4,w5,w6)
#define SEVENWORDINLINE(w1,w2,w3,w4,w5,w6,w7)
#define EIGHTWORDINLINE(w1,w2,w3,w4,w5,w6,w7,w8)
#define NINEWORDINLINE(w1,w2,w3,w4,w5,w6,w7,w8,w9)
#define TENWORDINLINE(w1,w2,w3,w4,w5,w6,w7,w8,w9,w10)
#define ELEVENWORDINLINE(w1,w2,w3,w4,w5,w6,w7,w8,w9,w10,w11)
#define TWELVEWORDINLINE(w1,w2,w3,w4,w5,w6,w7,w8,w9,w10,w11,w12)

/* External declaration macros */
#define EXTERN_API(_type)                       extern _type
#define EXTERN_API_C(_type)                     extern _type
#define EXTERN_API_STDCALL(_type)               extern _type
#define EXTERN_API_C_INLINE(_type)              extern _type

/*
 * =============================================================================
 * COMPILER DETECTION WORKAROUND
 * =============================================================================
 *
 * ConditionalMacros.h checks for various compilers in a specific order.
 * Clang defines __GNUC__ for compatibility, which would cause it to match
 * the GCC/Linux branch (#elif defined(__GNUC__) && defined(__linux__))
 * which uses unsupported MPW extensions like #cpu(powerpc).
 *
 * We need to make Clang match the Apple GCC branch instead:
 * #elif defined(__GNUC__) && (defined(__APPLE_CPP__) || defined(__APPLE_CC__) || defined(__NEXT_CPP__))
 *
 * Define __APPLE_CC__ to trigger this branch, which properly handles PowerPC without MPW extensions.
 */

#ifndef __APPLE_CC__
#define __APPLE_CC__ 1
#endif

/* Restore previous packing */
#pragma pack(pop)

#endif /* __clang__ && !__MACHEADERSCOMPAT_CONFIGURED__ */

#endif /* __MACHEADERSCOMPAT_H__ */
