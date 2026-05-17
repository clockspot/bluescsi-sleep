#include "Retro68.r"

/*
 * Diagnostic build variant of the BlueSCSI Sleep INIT.
 *
 * Same code as init.r but reads the diagnostic flat binary (compiled
 * with -DBLUESCSI_SLEEP_DIAGNOSTIC=1, which enables verbose logging
 * to "BlueSCSI Sleep Log" in the System Folder).
 */
type 'INIT' {
	RETRO68_CODE_TYPE
};

resource 'INIT' (128, locked) {
	dontBreakAtEntry, $$read("BlueSCSISleepINIT-diag.flt");
};
