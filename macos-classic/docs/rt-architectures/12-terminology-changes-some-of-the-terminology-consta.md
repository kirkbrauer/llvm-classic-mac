# Terminology Changes  Some of the terminology, constant names, and data type names in this book have been changed from previous documentation. The lists here describe the terminology used as of the E.T.O. 21 software development release. In some cases, the terminology has changed due to conceptual shifts, with the result that the old term and the new term are not directly interchangeable. Such changes are explained in the notes.  Table A-1 lists old terms used in previous documentation and the current terms used in this book.  Table A-1 Changes to terminology Old termNew termNotes A5 worldDirect data areaThis change applies only to the CFM-68K runtime environment. Global data world Direct data areaDirect data area entries can contain either the data itself or a pointer to indirect data.  Table of ContentsDirect data areaThe old Table of Contents is the set of pointers in the direct data area that point to indirect data.  Table of Contents Register (RTOC)Base registerAlso referred to as simply GPR2.  XDataPointerSee next columnXDataPointers are now simply referred to as pointers to indirect data.  XPointerSee next columnXPointers are now simply referred to as the pointer to the transition vector. XVectorTransition vectorChanged to emphasize the commonality between the PowerPC and CFM-68K implementations.  Table A-2 lists names used in previous versions of the codeFragments.h header file and the new names.  Table A-2 Changes to names in the CodeFragments.h header file Old nameNew nameNotes LoadFlagskCFragLoadOptionsPossible values for this flag are kReferenceCFrag, kFindCFrag, and kPrivateCFragCopy. kFindLibkFindCFrag  kFullLibkIsCompleteCFrag  kInMemkMemoryCFragLocator  kIsAppkApplicationCFrag  kIsDropInkDropInAdditionCFragDrop-in additions are now called plug-ins. kIsLibkImportLibraryCFrag  kLoadCFragkReferenceCFrag  kLoadLibkReferenceCFrag  kLoadNewCopykPrivateCFragCopy  kMotorola68KkMotorola68KCFragArch  kMotorola68KArchkMotorola68KCFragArch  kNewCFragCopykPrivateCFragCopy  kOnDiskFlatkDataForkCFragLocator  kOnDiskSegmentedkResourceCFragLocator  kPowerPCkPowerPCCFragArch  kPowerPCArchkPowerPCCFragArch  kPrivateConnectionkPrivateCFragCopy  kWholeForkkCFragGoesToEOF   Table A-3 lists older "source code" data types and the binary counterparts used in this book. Note that these mappings assume you are using MPW compilers. Other development environments may assume different sizes (2 bytes for type int rather than 4, for example).  Table A-3 Changes to names of data types Old typeNew type charUInt8 signed charSInt8 shortSInt16 unsigned shortUInt16 intSInt32 unsigned intUInt32 longSInt32 unsigned longUInt32          
# Appendix A -Terminology Changes
  Some of the terminology, constant names, and data type names in this book have been changed from previous documentation. The lists here describe the terminology used as of the E.T.O. 21 software development release. In some cases, the terminology has changed due to conceptual shifts, with the result that the old term and the new term are not directly interchangeable. Such changes are explained in the notes.
Table A-1  lists old terms used in previous documentation and the current terms used in this book.
| Old term | New term | Notes |
| --- | --- | --- |
| A5 world | Direct data area | This change applies only to the CFM-68K runtime environment. |
| Global data world | Direct data area | Direct data area entries can contain either the data itself or a pointer to i... |
| Table of Contents | Direct data area | The old Table of Contents is the set of pointers in the direct data area that... |
| Table of Contents Register (RTOC) | Base register | Also referred to as simply GPR2. |
| XDataPointer | See next column | XDataPointers are now simply referred to as pointers to indirect data. |
| XPointer | See next column | XPointers are now simply referred to as the pointer to the transition vector. |
| XVector | Transition vector | Changed to emphasize the commonality between the PowerPC and CFM-68K implemen... |

Table A-2  lists names used in previous versions of the  `codeFragments.h`  header file and the new names.
| Old name | New name | Notes |
| --- | --- | --- |
| LoadFlags | kCFragLoadOptions | Possible values for this flag arekReferenceCFrag,kFindCFrag, andkPrivateCFrag... |
| kFindLib | kFindCFrag |  |
| kFullLib | kIsCompleteCFrag |  |
| kInMem | kMemoryCFragLocator |  |
| kIsApp | kApplicationCFrag |  |
| kIsDropIn | kDropInAdditionCFrag | Drop-in additions are now called plug-ins. |
| kIsLib | kImportLibraryCFrag |  |
| kLoadCFrag | kReferenceCFrag |  |
| kLoadLib | kReferenceCFrag |  |
| kLoadNewCopy | kPrivateCFragCopy |  |
| kMotorola68K | kMotorola68KCFragArch |  |
| kMotorola68KArch | kMotorola68KCFragArch |  |
| kNewCFragCopy | kPrivateCFragCopy |  |
| kOnDiskFlat | kDataForkCFragLocator |  |
| kOnDiskSegmented | kResourceCFragLocator |  |
| kPowerPC | kPowerPCCFragArch |  |
| kPowerPCArch | kPowerPCCFragArch |  |
| kPrivateConnection | kPrivateCFragCopy |  |
| kWholeFork | kCFragGoesToEOF |  |

Table A-3  lists older "source code" data types and the binary counterparts used in this book. Note that these mappings assume you are using MPW compilers. Other development environments may assume different sizes (2 bytes for type  `int`  rather than 4, for example).
| Old type | New type |
| --- | --- |
| char | UInt8 |
| signed char | SInt8 |
| short | SInt16 |
| unsigned short | UInt16 |
| int | SInt32 |
| unsigned int | UInt32 |
| long | SInt32 |
| unsigned long | UInt32 |

---