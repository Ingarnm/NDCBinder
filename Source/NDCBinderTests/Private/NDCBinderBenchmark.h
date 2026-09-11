// NDCBinderBenchmark.h
#pragma once

#include "CoreMinimal.h"

/**
 * What the writer costs over writing the same thing by hand, as a list of report lines.
 *
 * Deliberately not an automation test and deliberately not behind WITH_DEV_AUTOMATION_TESTS. The
 * question it answers is "what does this cost in Shipping", and a Shipping build has no automation
 * framework, no console to type a command into and, on an installed engine, no logging either —
 * every one of those is a global define that a project cannot change without a unique build
 * environment, which an installed engine forbids.
 *
 * So it is a plain function with two callers: the automation test, which reports the lines through
 * AddInfo, and a command-line hook that writes them to a file. The second works in any configuration,
 * which is the only way the numbers from different configurations can be compared at all.
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
	 * Runs it when the command line asks, and writes the report to the file it names:
	 *   LyraGame.exe ... -ndcbench="C:/path/to/report.txt"
	 * Does nothing otherwise. Called from the module's startup, and waits from there for the first
	 * ticking game world so the real-write cases have one.
	 */
	void RunIfRequestedOnCommandLine();
}
