include "llvm/Option/OptParser.td"

// For backward compatibility, classic68k-ld accepts most ELF-style options.

def help : Flag<["--", "-"], "help">, HelpText<"Print option help">;
def version : Flag<["--"], "version">, HelpText<"Print version">;

def output : JoinedOrSeparate<["-"], "o">, MetaVarName<"<path>">,
  HelpText<"Output file path">;

def entry : Separate<["-"], "e">, MetaVarName<"<symbol>">,
  HelpText<"Specify entry point symbol (default: main)">;
def entry_eq : Joined<["--"], "entry=">, Alias<entry>;

def L : JoinedOrSeparate<["-"], "L">, MetaVarName<"<path>">,
  HelpText<"Add library search path">;

def l : JoinedOrSeparate<["-"], "l">, MetaVarName<"<name>">,
  HelpText<"Link with library">;

def stack_size : Separate<["--"], "stack-size">, MetaVarName<"<size>">,
  HelpText<"Set stack size in bytes (default: 32768)">;
def stack_size_eq : Joined<["--"], "stack-size=">, Alias<stack_size>;

def heap_size : Separate<["--"], "heap-size">, MetaVarName<"<size>">,
  HelpText<"Set preferred heap size in bytes (default: 393216)">;
def heap_size_eq : Joined<["--"], "heap-size=">, Alias<heap_size>;

def min_heap_size : Separate<["--"], "min-heap-size">, MetaVarName<"<size>">,
  HelpText<"Set minimum heap size in bytes (default: same as heap-size)">;
def min_heap_size_eq : Joined<["--"], "min-heap-size=">, Alias<min_heap_size>;

def creator : Separate<["--"], "creator">, MetaVarName<"<4-char>">,
  HelpText<"Set creator code (default: ????)">;
def creator_eq : Joined<["--"], "creator=">, Alias<creator>;

def segment_name : Separate<["--"], "segment-name">, MetaVarName<"<name>">,
  HelpText<"Set CODE segment name (default: Sources)">;
def segment_name_eq : Joined<["--"], "segment-name=">, Alias<segment_name>;

def verbose : Flag<["--"], "verbose">, HelpText<"Print verbose information">;

// Standard linker flags we accept but mostly ignore
def static : Flag<["-"], "static">;
def nostdlib : Flag<["-"], "nostdlib">;
def r : Flag<["-"], "r">, HelpText<"Relocatable output (not supported)">;
