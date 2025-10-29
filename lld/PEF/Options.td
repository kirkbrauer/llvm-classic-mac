include "llvm/Option/OptParser.td"

//===----------------------------------------------------------------------===//
/// PEF-specific options
//===----------------------------------------------------------------------===//

def grp_pef : OptionGroup<"PEF">, HelpText<"PEF LINKER OPTIONS">;

// Help and version
def help : Flag<["-", "--"], "help">,
    HelpText<"Print this help message">,
    Group<grp_pef>;

def version : Flag<["-", "--"], "version">,
    HelpText<"Display the version number and exit">,
    Group<grp_pef>;

// Output file
def o : JoinedOrSeparate<["-"], "o">,
    HelpText<"Output file path">,
    MetaVarName<"<path>">,
    Group<grp_pef>;

def output : Separate<["--"], "output">,
    Alias<o>,
    HelpText<"Alias for -o">,
    Group<grp_pef>;

// Entry point
def e : JoinedOrSeparate<["-"], "e">,
    HelpText<"Entry point symbol">,
    MetaVarName<"<symbol>">,
    Group<grp_pef>;

def entry : Separate<["--"], "entry">,
    Alias<e>,
    HelpText<"Alias for -e">,
    Group<grp_pef>;

// Base addresses
def base_code : Separate<["--"], "base-code">,
    HelpText<"Code section base address (default: 0x0)">,
    MetaVarName<"<addr>">,
    Group<grp_pef>;

def base_data : Separate<["--"], "base-data">,
    HelpText<"Data section base address (default: 0x0)">,
    MetaVarName<"<addr>">,
    Group<grp_pef>;

// Library search paths (Phase 2)
def L : JoinedOrSeparate<["-"], "L">,
    HelpText<"Add directory to library search path">,
    MetaVarName<"<dir>">,
    Group<grp_pef>;

// Libraries (Phase 2)
def l : JoinedOrSeparate<["-"], "l">,
    HelpText<"Link against library">,
    MetaVarName<"<name>">,
    Group<grp_pef>;

def weak_l : JoinedOrSeparate<["--"], "weak-l">,
    HelpText<"Weak link against library">,
    MetaVarName<"<name>">,
    Group<grp_pef>;

// Diagnostics
def verbose : Flag<["-", "--"], "verbose">,
    HelpText<"Verbose output">,
    Group<grp_pef>;

def v : Flag<["-"], "v">,
    Alias<verbose>,
    HelpText<"Alias for --verbose">,
    Group<grp_pef>;

// Error handling
def allow_undefined : Flag<["--"], "allow-undefined">,
    HelpText<"Allow undefined symbols">,
    Group<grp_pef>;
