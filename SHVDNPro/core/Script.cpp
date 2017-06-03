#include <cstdio>

#include <Script.h>

#include <ManagedGlobals.h>

#include <Config.h>

#pragma unmanaged
#include <Windows.h>
#undef Yield

static void ScriptSwitchToMainFiber(void* fiber)
{
	SwitchToFiber(fiber);
}
#pragma managed

GTA::Script::Script()
{
	m_fiberMain = nullptr;
	m_fiberWait = 0;
}

void GTA::Script::Wait(int ms)
{
	m_fiberWait = ms;
	ScriptSwitchToMainFiber(m_fiberMain);
}

void GTA::Script::Yield()
{
	Wait(0);
}

void GTA::Script::OnTick()
{
}

void GTA::Script::OnPresent(System::IntPtr swapchain)
{
}

void GTA::Script::OnKeyDown(System::Windows::Forms::KeyEventArgs^ args)
{
}

void GTA::Script::OnKeyUp(System::Windows::Forms::KeyEventArgs^ args)
{
}

GTA::Script^ GTA::Script::GetExecuting()
{
	void* currentFiber = GetCurrentFiber();

	// I don't know if GetCurrentFiber ever returns null, but whatever
	if (currentFiber == nullptr) {
		return nullptr;
	}

	for each (auto script in GTA::ManagedGlobals::g_scripts) {
		if (script->m_fiberCurrent == currentFiber) {
			return script;
		}
	}

	return nullptr;
}

void GTA::Script::WaitExecuting(int ms)
{
	auto script = GetExecuting();
	if (script == nullptr) {
		throw gcnew System::Exception("Illegal call to WaitExecuting() from a non-script fiber!");
	}
	script->Wait(ms);
}

void GTA::Script::YieldExecuting()
{
	WaitExecuting(0);
}

void GTA::Script::ProcessOneTick()
{
	System::Tuple<bool, System::Windows::Forms::KeyEventArgs^>^ ev = nullptr;

	while (m_keyboardEvents->TryDequeue(ev)) {
		try {
			if (ev->Item1) {
				OnKeyDown(ev->Item2);
			} else {
				OnKeyUp(ev->Item2);
			}
		} catch (System::Exception^ ex) {
			if (ev->Item1) {
				GTA::ManagedGlobals::g_logWriter->WriteLine("*** Exception during OnKeyDown: {0}", ex->ToString());
			} else {
				GTA::ManagedGlobals::g_logWriter->WriteLine("*** Exception during OnKeyUp: {0}", ex->ToString());
			}
		}
	}

	try {
		OnTick();
	} catch (System::Exception^ ex) {
		GTA::ManagedGlobals::g_logWriter->WriteLine("*** Exception during OnTick: {0}", ex->ToString());
	}
}
