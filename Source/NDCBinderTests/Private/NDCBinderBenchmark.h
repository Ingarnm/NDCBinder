// NDCBinderBenchmark.h
#pragma once

#include "CoreMinimal.h"

/**
 * What the writer costs over writing the same thing by hand, as a list of report lines.
 *
 * Deliberately not an automation test and deliberately not behind WITH_DEV_AUTOMATION_TESTS, so that
 * a build with no automation framework — and, on an installed engine, no logging to report through
 * either — can still produce the numbers. Both of those are global defines a project cannot change
 * without a unique build environment, which an installed engine forbids.
 *
 * So it is a plain function with two callers: the automation test, which reports the lines through
 * AddInfo, and a command-line hook that writes them to a file. The file is what lets the report come
 * out of a packaged game rather than only out of the editor.
 *
 * HOW FAR THAT REACHES. The module this lives in is a DeveloperTool one, which UBT leaves out of
 * Shipping entirely, so the switch reaches Development and DebugGame and no further. Measuring a
 * Shipping build means temporarily retyping the module to Runtime in the .uplugin and rebuilding —
 * worth knowing before reading "what does this cost shipped" into a number from here.
 */
class UWorld;

namespace NDCBinderBenchmark
{
	/**
	 * Runs every case and returns the report, one line each. Game thread; takes a fraction of a second.
	 *
	 * Given a World, the report also covers a WHOLE WriteToChannel — Niagara's own per-write path
	 * included — which is the only case that says what a write costs rather than what this way of
	 * asking for one adds. Without a world those cases are skipped and say so.
	 */
	TArray<FString> Run(UWorld* World = nullptr);

	/**
	 * Runs it when the command line asks, and writes the report to the file it names. This is the
	 * whole invocation, and the only place it is written down:
	 *
	 *   YourGame.exe YourProject.uproject -ndcbench="C:/path/to/report.txt" -unattended -nullrhi
	 *
	 * -unattended keeps it from stopping on a dialog and -nullrhi from opening a window, neither of
	 * which the report needs. Does nothing without the switch. Called from the module's startup, and
	 * waits from there for the first ticking game world so the real-write cases have one.
	 */
	void RunIfRequestedOnCommandLine();
}
