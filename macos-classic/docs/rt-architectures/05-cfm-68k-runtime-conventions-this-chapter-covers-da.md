# CFM-68K Runtime Conventions This chapter covers data storage and parameter-passing conventions for the CFM-68K runtime environment. All CFM-68K runtime conventions are language independent. These conventions may be useful for low-level programming (if you are writing in assembly language, for example) or for optimizing higher-level code.   Chapter Contents   Data Types  Routine Calling Conventions   Parameter Deallocation  Stack Alignment  Fixed-Argument Passing Conventions  Variable-Argument Passing Conventions  Function Value Return  Stack Frames, A6, and Reserved Frame Slots  Register Preservation               
# Data Types
   Table 5-1  lists the binary data types and their sizes in the CFM-68K runtime environment. These types and sizes are identical to those in the PowerPC runtime environment.
All numeric and pointer data types are stored in big-endian format (that is, high bytes first, then low bytes). Signed integers use two's-complement representation.
IMPORTANT   The layout of the  `extended`  data type is either that of the SANE 80-bit data type or that of the 96-bit MC68881 data type, depending on the software development environment used. Because of this variability, you should not use the  `extended`  data type for imported or exported routines or data.      The size of data structures and unions must be a multiple of two, and an extra byte may be added at the end to meet this requirement. Items inside a data structure (except for types  `UInt8`  and  `SInt8` ) are placed on a 2-byte boundary with an extra padding byte inserted if necessary. Type  `UInt8`  and type  `SInt8`  items (single variables or arrays) are merely placed in the next available byte.
---


Main Body
 Chapter 5 - CFM-68K Runtime Conventions
---

# Routine Calling Conventions
   Th is section details the process of passing parameters to a routine in the CFM-68K runtime environment.
Note   These parameter passing conventions are part of Apple's standard for procedural interfaces. Object-oriented languages may use different rules for their own method calls. For example, the conventions for C++ virtual function calls may be different from those for C functions.      A routine can have a fixed or variable number of arguments. In an ANSI-style C syntax definition, a routine with a variable number of arguments typically appears with ellipsis points (...) at the end of its input parameter list.
A variable-argument routine may have several required (that is, fixed) parameters preceding the variable parameter portion. For example, the function definition
```
mooColor(number,[color1. . .])
```
  gives no restriction on the number of color arguments, but you must always precede them with a number argument. Therefore, number is a fixed parameter.
The calling routine passes parameters by pushing their values onto the stack, and the stack grows downward (towards lower addresses) with each push. The rightmost parameter is the first pushed onto the stack, with the others following from right to left. For example, given the code
```
cow = mooFunc(moo1, moo2, moo3);
```
  the calling routine first pushes the value of  `moo3`  onto the stack, followed by  `moo2`  and then  `moo1` .
The return address of the routine is the last item pushed onto the stack.
Note   The order of passing parameters onto the stack in CFM-68K is identical to that for the classic 68K C calling convention. For information about the 68K stack structure, see  "Classic 68K Stack Structure and Calling Conventions," beginning on page 11-4 .
---
   SubtopicsTOC      Parameter Deallocation     Stack Alignment     Fixed-Argument Passing Conventions     Variable-Argument Passing Conventions     Function Value Return     Stack Frames, A6, and Reserved Frame Slots     Register Preservation

---


Main Body
 Chapter 5 - CFM-68K Runtime Conventions  /  Routine Calling Conventions
---

## Parameter Deallocation
  In the CFM-68K runtime environment, responsibility for removing items from the stack depends on the function type.
- In a fixed-argument type routine, the called routine deallocates (that is, pops from the stack) the return address and all the passed parameters before, or as part of, its return. The calling routine does not need to do any cleanup.  If the called routine is a variable-argument type, it only pops the return address before returning. The calling routine must then deallocate all the parameters it pushed onto the stac k.

---


Main Body
 Chapter 5 - CFM-68K Runtime Conventions  /  Routine Calling Conventions
---

## Stack Alignment
   To improve performance, the CFM-68K runtime architecture requires a 4-byte (minimum) alignment for all parameters pushed onto the stack. This applies to stack space used in function prologs (that is, stack space reserved for automatic memory variables and temporaries) as well as space allocated using the  `alloca`  dynamic stack allocation operation. Types  `UInt8` , ` SInt8` ,  `Boolean` ,  `UInt16` , and  `SInt16`  parameters are passed in the least significant byte or bytes with padding added. Data types  `struct` ,  `union` , and  `extended`  are passed in the most significant bytes, with padding added afterwards if necessary.

---


Main Body
 Chapter 5 - CFM-68K Runtime Conventions  /  Routine Calling Conventions
---

## Fixed-Argument Passing Conventions
   Fixed parameters may either be items used to call a fixed-argument type routine, or fixed items that precede the variable items in a variable-argument function call. In either case, fixed parameters must occupy a multiple of 4 bytes when pushed onto the stack, with padding added if necessary. Note that the data can actually be pushed in any order as long as the final alignment matches the required convention.
- Parameters of type  `UInt8` ,  `SInt8` , and  `Boolean`  are pushed onto the stack as 1 byte of data (the least significant byte) along with 3 bytes of undefined padding.  Parameters of type  `UInt16`  and  `SInt16`  are pushed onto the stack as 2 bytes (least significant) of data plus 2 bytes of padding.  Pointers to procedures and arrays are pushed normally (since they are 4 bytes long), as are  `UInt32` ,  `SInt32` , and  `float`  data items.  Type  `double`  parameters are passed by pushing the memory image of the 8-byte item onto the stack.  Type  `extended`  parameters can be either 10 or 12 bytes long, depending on the development environment. For 10-byte items, 2 padding bytes are pushed onto the stack before pushing the parameter. A 12-byte  `extended`  data item is pushed onto the stack normally (since it is a multiple of 4 bytes).  If the size of a data structure or  `union`  is not a multiple of 4 bytes, 2 padding bytes are added to the stack before pushing the parameter. Otherwise, the parameter is pushed onto the stack normally. In both cases, the memory image of the item is passed.  Bit field layout is not defined. You should not use bit fields in procedures or data structures that have shared library interfaces.

---


Main Body
 Chapter 5 - CFM-68K Runtime Conventions  /  Routine Calling Conventions
---

## Variable-Argument Passing Conventions
   When passing variable arguments, padding is added to some data types when pushing them onto the stack:
- Parameters of types  `UInt8` ,  `SInt8` ,  `UInt16` ,  `SInt16` , and  `Boolean`  are converted to type  `SInt32`  (as if by assignment) and pushed onto the stack as a 4-byte integer data item.  Both  `float`  and  `double`  parameters are pushed onto the stack as 8-byte  `double`  data items ( `float`  data types are converted to type  `double`  (as if by assignment) before being pushed).  All other data types are passed normally.

---


Main Body
 Chapter 5 - CFM-68K Runtime Conventions  /  Routine Calling Conventions
---

## Function Value Return
   In the CFM-68K runtime environment, the placement of the return value depends on its size:
- Functions returning  `UInt8` ,  `SInt8` , or  `Boolean`  data types place the return value in the least significant byte of D0. The three most significant bytes in D0 are undefined.  Functions returning  `UInt16`  or  `SInt16`  data types place the return value in the two least significant bytes of D0. The two most significant bytes in D0 are undefined.  Functions returning pointers (including array pointers),  `UInt32` ,  `SInt32` , or  `float`  data types place the return value in D0.  Functions returning small data structures or  `union`  data types place them in the least significant bytes of D0. For example, a 4-byte structure takes up D0, while a 2-byte structure occupies the two least significant bytes of D0, with the extra bytes being undefined.  If the function return value is larger than 4 bytes (this applies to  `double`  and  `extended`  data types, as well as to large  `struct`  or  `union`  data types), a pointer must be pushed onto the stack at call time after all the user-visible arguments have been pushed. The address of the pointer must be a memory location large enough to hold the function return value. When the function exits, it returns this address in the D0 register.

---


Main Body
 Chapter 5 - CFM-68K Runtime Conventions  /  Routine Calling Conventions
---

## Stack Frames, A6, and Reserved Frame Slots
   The CFM-68K runtime architecture requires two long words in the stack frame to be reserved (that is, unused) for future use. The word locations for these reserved slots are  `-4(A6)`  and  `-8(A6)` .
Routines making calls through procedure pointers (that is, indirect or cross-fragment calls) must have an A6 frame and must reserve the two long words. Leaf routines or routines that make only direct (in-fragment) calls do not need to use A6 as a link, and they do not require reserved stack frame slots. However, some debugging options may require you to set up a stack frame.
In general, you should not use the A6 register except as a frame pointer, and if you do set up an A6 stack frame, you must also reserve the two long frame slots.

---


Main Body
 Chapter 5 - CFM-68K Runtime Conventions  /  Routine Calling Conventions
---

## Register Preservation
   Table 5-2  lists registers used in the CFM-68K runtime environment and their volatility in function calls. Registers that retain their value after a routine call are called nonvolatile. All registers are 4 bytes long.
| Type | Register | Preserved bya routiine call? | Notes |
| --- | --- | --- | --- |
| Data register | D0 through D2 | No |  |
|  | D3 through D7 | Yes |  |
| Address register | A0 | No |  |
|  | A1 | No | Used to pass transition vector addresses (+4) when making indirect or cross-f... |
|  | A2 through A4 | Yes |  |
|  | Address register | A5 | See next column |
| Address register | A5 | See next column | Used to access global data objects and the jump table. A5 is preserved by dir... |
|  | A6 | Yes | Used as the back link and frame pointer when making cross-fragment calls. |
|  | A7 | See next column | A7 is the stack pointer used to push and pop parameters and other temporary d... |
| Floating- point register | F0 through F3 | No | When present. |
|  | F4 through F7 | Yes | When present. |
| Condition Register | CR | No | Bits are set by compare instructions and used for conditional branching. |

---