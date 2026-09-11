// NDCBinderPerfTests.cpp
//
// The editor-side way to run the benchmark. The measurement itself lives in NDCBinderBenchmark, which
// is deliberately free of the automation framework so the same cases can run in a Shipping build,
// where there is no automation framework to run them with. See that file for what is measured and why.

#include "NDCBinderBenchmark.h"

#include "Engine/World.h"
#include "Misc/AutomationTest.h"

#if WITH_DEV_AUTOMATION_TESTS

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FNDCBinderBindingOverheadTest,
	"NDCBinder.Performance.BindingOverhead",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::PerfFilter)

bool FNDCBinderBindingOverheadTest::RunTest(const FString& Parameters)
{
	// GWorld rather than the editor world through GEditor: this module must keep building in Shipping,
	// where there is no UnrealEd to ask. In an editor run this is the editor's own world, which has a
	// Niagara world manager like any other — enough for the real-write cases to run.
	const TArray<FString> Report = NDCBinderBenchmark::Run(GWorld);
	for (const FString& Line : Report)
	{
		AddInfo(Line);
	}

	// Reported, not asserted: a machine-specific number is not a pass/fail condition, and a test that
	// fails on a busy build agent teaches nobody anything. The one thing worth failing on is a result
	// that cannot be true, which would mean the harness is broken rather than the code being fast.
	TestTrue(TEXT("the benchmark produced a report"), Report.Num() > 0);
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
