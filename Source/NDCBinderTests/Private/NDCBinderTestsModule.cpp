// NDCBinderTestsModule.cpp

#include "NDCBinderBenchmark.h"

#include "Modules/ModuleManager.h"
#include "NDCBinder.h"

#if WITH_DEV_AUTOMATION_TESTS
#include "Misc/AutomationTest.h"
#endif

/**
 * Two things on startup, and neither of them is a test.
 *
 * The benchmark, when the command line asks for it, because that is the one thing that cannot go
 * through the automation framework: a Shipping build has no automation framework, no console and — on
 * an installed engine — no logging, so a command-line switch writing to a file is the only channel
 * that works in every configuration, and therefore the only one whose numbers can be compared between
 * them.
 *
 * And a hook that empties the writer's warned-once record before every test. A misconfigured row warns
 * once per owner class and binding for the life of the process, which is right for a game and wrong
 * for a test process: three of these tests assert that a warning WAS logged, so they passed on the
 * first run of a session and failed on the second, reading as a regression rather than as the
 * suppression it was. Done here rather than in each test, because the next test to assert on a
 * warning should not have to know any of this.
 */
class FNDCBinderTestsModule : public FDefaultModuleImpl
{
public:
	virtual void StartupModule() override
	{
		NDCBinderBenchmark::RunIfRequestedOnCommandLine();

#if WITH_DEV_AUTOMATION_TESTS
		TestStartHandle = FAutomationTestFramework::Get().OnTestStartEvent.AddStatic(&FNDCBinderTestsModule::OnTestStart);
#endif
	}

	virtual void ShutdownModule() override
	{
#if WITH_DEV_AUTOMATION_TESTS
		if (TestStartHandle.IsValid())
		{
			FAutomationTestFramework::Get().OnTestStartEvent.Remove(TestStartHandle);
			TestStartHandle.Reset();
		}
#endif
	}

private:
#if WITH_DEV_AUTOMATION_TESTS
	static void OnTestStart(FAutomationTestBase* /*Test*/)
	{
		// Every test, not only the ones that assert on a warning: a test that runs after one of those
		// would otherwise inherit whichever lines it happened to leave suppressed.
		FNDCBinder::ResetBindingWarnings();
	}

	FDelegateHandle TestStartHandle;
#endif
};

IMPLEMENT_MODULE(FNDCBinderTestsModule, NDCBinderTests);
