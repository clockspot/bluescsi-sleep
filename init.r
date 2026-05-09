#include "Retro68.r"

/*
 * Package the flat INIT binary as an 'INIT' resource.
 *
 * The file BlueSCSISleepINIT.flt is the output of the add_executable target
 * built with --mac-flat; Rez embeds it verbatim here.
 */
type 'INIT' {
	RETRO68_CODE_TYPE
};

resource 'INIT' (128, locked) {
	dontBreakAtEntry, $$read("BlueSCSISleepINIT.flt");
};
