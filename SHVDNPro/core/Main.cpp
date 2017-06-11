#include <vector>

#include <Script.h>
#include <Function.h>
#include <Input.h>

#include <MemoryAccess.h>
#include <GlobalVariable.h>
#include <NativeObjects.h>
#include <NativeHashes.h>
#include <INativeValue.h>
#include <OutputArgument.h>

#include <Matrix.hpp>
#include <Quaternion.hpp>
#include <Vector2.hpp>
#include <Vector3.hpp>

#include <ManagedGlobals.h>
#include <ScriptDomain.h>
#include <Log.h>

#include <UnmanagedLog.h>

ref class ManagedEventSink
{
public:
	System::Reflection::Assembly^ OnAssemblyResolve(System::Object^ sender, System::ResolveEventArgs^ args)
	{
		auto exeAssembly = System::Reflection::Assembly::GetExecutingAssembly();
		if (args->Name == exeAssembly->FullName) {
			return exeAssembly;
		}

		return nullptr;
	}

	void OnUnhandledException(System::Object^ sender, System::UnhandledExceptionEventArgs^ args)
	{
		GTA::WriteLog("*** Unhandled exception: {0}", args->ExceptionObject->ToString());
	}

	static ManagedEventSink^ Instance;
};

void LoadScriptDomain()
{
	auto curDir = System::IO::Path::GetDirectoryName(System::Reflection::Assembly::GetExecutingAssembly()->Location);

	auto setup = gcnew System::AppDomainSetup();
	setup->ApplicationBase = System::IO::Path::GetFullPath(curDir + "\\Scripts");
	setup->ShadowCopyFiles = "true"; // !?
	setup->ShadowCopyDirectories = curDir;

	GTA::WriteLog("Creating AppDomain with base \"{0}\"", setup->ApplicationBase);

	auto appDomainName = "ScriptDomain_" + (curDir->GetHashCode() * System::Environment::TickCount).ToString("X");
	auto appDomainPermissions = gcnew System::Security::PermissionSet(System::Security::Permissions::PermissionState::Unrestricted);

	GTA::ManagedGlobals::g_appDomain = System::AppDomain::CreateDomain(appDomainName, nullptr, setup, appDomainPermissions);
	GTA::ManagedGlobals::g_appDomain->InitializeLifetimeService();

	//TODO: This crashes.. probably requires an instance created by the AppDomain
	//GTA::ManagedGlobals::g_appDomain->AssemblyResolve += gcnew System::ResolveEventHandler(eventSink, &ManagedEventSink::OnAssemblyResolve);
	//GTA::ManagedGlobals::g_appDomain->UnhandledException += gcnew System::UnhandledExceptionEventHandler(eventSink, &ManagedEventSink::OnUnhandledException);

	GTA::WriteLog("Created AppDomain \"{0}\"", GTA::ManagedGlobals::g_appDomain->FriendlyName);

	auto typeScriptDomain = GTA::ScriptDomain::typeid;
	try {
		GTA::ManagedGlobals::g_scriptDomain = static_cast<GTA::ScriptDomain^>(GTA::ManagedGlobals::g_appDomain->CreateInstanceFromAndUnwrap(typeScriptDomain->Assembly->Location, typeScriptDomain->FullName));
	} catch (System::Exception^ ex) {
		GTA::WriteLog("*** Failed to create ScriptDomain: {0}", ex->ToString());
		if (ex->InnerException != nullptr) {
			GTA::WriteLog("*** InnerException: {0}", ex->InnerException->ToString());
		}
		return;
	} catch (...) {
		GTA::WriteLog("*** Failed to create ScriptDomain beacuse of unmanaged exception");
		return;
	}
	GTA::ManagedGlobals::g_scriptDomain->FindAllTypes();

	GTA::WriteLog("Created ScriptDomain!");
}

static bool ManagedScriptInit(int scriptIndex, void* fiberMain, void* fiberScript)
{
	return GTA::ManagedGlobals::g_scriptDomain->ScriptInit(scriptIndex, fiberMain, fiberScript);
}

static int ManagedScriptGetWaitTime(int scriptIndex)
{
	auto script = GTA::ManagedGlobals::g_scriptDomain->m_scripts[scriptIndex];
	if (script == nullptr) {
		return 0;
	}
	return script->m_fiberWait;
}

static void ManagedScriptResetWaitTime(int scriptIndex)
{
	auto script = GTA::ManagedGlobals::g_scriptDomain->m_scripts[scriptIndex];
	if (script == nullptr) {
		return;
	}
	script->m_fiberWait = 0;
}

static void ManagedScriptTick(int scriptIndex)
{
	auto script = GTA::ManagedGlobals::g_scriptDomain->m_scripts[scriptIndex];
	if (script != nullptr) {
		script->ProcessOneTick();
	}
	GTA::Native::Function::ClearStringPool();
}

static void ManagedD3DPresent(void* swapchain)
{
	System::IntPtr ptrSwapchain(swapchain);

	for each (auto script in GTA::ManagedGlobals::g_scriptDomain->m_scripts) {
		try {
			script->OnPresent(ptrSwapchain);
		} catch (System::Exception^ ex) {
			GTA::WriteLog("*** Exception during OnPresent: {0}", ex->ToString());
		}
	}
}

static void ManagedInitialize();

#pragma unmanaged
#include <main.h>
#include <Windows.h>
#include <cstdarg>

struct ScriptFiberInfo
{
	int m_index;
	void* m_fiberMain;
	void* m_fiberScript;

	bool m_initialized;
	bool m_defect;
};

static HMODULE _instance;

static std::vector<ScriptFiberInfo> _scriptFibers; //TODO: Add stuff to this

static void ScriptMainFiber(LPVOID pv)
{
	ScriptFiberInfo* fi = (ScriptFiberInfo*)pv;

	while (true) {
		if (fi->m_defect) {
			SwitchToFiber(fi->m_fiberMain);
			continue;
		}

		if (!fi->m_initialized) {
			if (ManagedScriptInit(fi->m_index, fi->m_fiberMain, fi->m_fiberScript)) {
				fi->m_initialized = true;
			} else {
				fi->m_defect = true;
			}
			SwitchToFiber(fi->m_fiberMain);
			continue;
		}

		ManagedScriptTick(fi->m_index);
		SwitchToFiber(fi->m_fiberMain);
	}
}

static void ScriptMain(int index)
{
	ScriptFiberInfo fi;
	fi.m_index = index;
	fi.m_fiberMain = GetCurrentFiber();
	fi.m_fiberScript = CreateFiber(0, ScriptMainFiber, (LPVOID)&fi);
	fi.m_initialized = false;
	fi.m_defect = false;

	UnmanagedLogWrite("ScriptMain(%d) -> Initialized: %d, defect: %d (main: %p, script: %p)\n", index, (int)fi.m_initialized, (int)fi.m_defect, fi.m_fiberMain, fi.m_fiberScript);

	while (true) {
		int ms = ManagedScriptGetWaitTime(fi.m_index);
		scriptWait(ms);
		ManagedScriptResetWaitTime(fi.m_index);
		SwitchToFiber(fi.m_fiberScript);
	}
}

// uh
// yeah
// it would be nice if shv gave us some more leeway here
#pragma optimize("", off)
static void ScriptMain_Wrapper0() { ScriptMain(0); }
static void ScriptMain_Wrapper1() { ScriptMain(1); }
static void ScriptMain_Wrapper2() { ScriptMain(2); }
static void ScriptMain_Wrapper3() { ScriptMain(3); }
static void ScriptMain_Wrapper4() { ScriptMain(4); }
static void ScriptMain_Wrapper5() { ScriptMain(5); }
static void ScriptMain_Wrapper6() { ScriptMain(6); }
static void ScriptMain_Wrapper7() { ScriptMain(7); }
static void ScriptMain_Wrapper8() { ScriptMain(8); }
static void ScriptMain_Wrapper9() { ScriptMain(9); }
static void ScriptMain_Wrapper10() { ScriptMain(10); }
static void ScriptMain_Wrapper11() { ScriptMain(11); }
static void ScriptMain_Wrapper12() { ScriptMain(12); }
static void ScriptMain_Wrapper13() { ScriptMain(13); }
static void ScriptMain_Wrapper14() { ScriptMain(14); }
static void ScriptMain_Wrapper15() { ScriptMain(15); }
static void ScriptMain_Wrapper16() { ScriptMain(16); }
static void ScriptMain_Wrapper17() { ScriptMain(17); }
static void ScriptMain_Wrapper18() { ScriptMain(18); }
static void ScriptMain_Wrapper19() { ScriptMain(19); }
static void ScriptMain_Wrapper20() { ScriptMain(20); }
#pragma optimize("", on)

static void(*_scriptWrappers[])() = {
	&ScriptMain_Wrapper0,
	&ScriptMain_Wrapper1,
	&ScriptMain_Wrapper2,
	&ScriptMain_Wrapper3,
	&ScriptMain_Wrapper4,
	&ScriptMain_Wrapper5,
	&ScriptMain_Wrapper6,
	&ScriptMain_Wrapper7,
	&ScriptMain_Wrapper8,
	&ScriptMain_Wrapper9,
	&ScriptMain_Wrapper10,
	&ScriptMain_Wrapper11,
	&ScriptMain_Wrapper12,
	&ScriptMain_Wrapper13,
	&ScriptMain_Wrapper14,
	&ScriptMain_Wrapper15,
	&ScriptMain_Wrapper16,
	&ScriptMain_Wrapper17,
	&ScriptMain_Wrapper18,
	&ScriptMain_Wrapper19,
	&ScriptMain_Wrapper20,
};

static void RegisterScriptMain(int index)
{
	if (index > 20) {
		//TODO: Log some error?
		return;
	}
	UnmanagedLogWrite("RegisterScriptMain(%d) : %p\n", index, _scriptWrappers[index]);
	scriptRegister(_instance, _scriptWrappers[index]);
}

static void ScriptKeyboardMessage(DWORD key, WORD repeats, BYTE scanCode, BOOL isExtended, BOOL isWithAlt, BOOL wasDownBefore, BOOL isUpNow)
{
	ManagedScriptKeyboardMessage(key, repeats, scanCode, isExtended, isWithAlt, wasDownBefore, isUpNow);
}

static void DXGIPresent(void* swapChain)
{
	//TODO
}
#pragma managed

static void ManagedInitialize()
{
	GTA::WriteLog("SHVDN Pro Initializing");

	ManagedEventSink::Instance = gcnew ManagedEventSink();
	System::AppDomain::CurrentDomain->AssemblyResolve += gcnew System::ResolveEventHandler(ManagedEventSink::Instance, &ManagedEventSink::OnAssemblyResolve);
	System::AppDomain::CurrentDomain->UnhandledException += gcnew System::UnhandledExceptionEventHandler(ManagedEventSink::Instance, &ManagedEventSink::OnUnhandledException);
}

static void* _fiberControl;

static void ManagedSHVDNProControl()
{
	void* fiber = CreateFiber(0, [](void*) {
		GTA::WriteLog("Control thread initializing");

		LoadScriptDomain();

		GTA::WriteLog("{0} script types found:", GTA::ManagedGlobals::g_scriptDomain->m_types->Length);
		for (int i = 0; i < GTA::ManagedGlobals::g_scriptDomain->m_types->Length; i++) {
			GTA::WriteLog("  {0}: {1}", i, GTA::ManagedGlobals::g_scriptDomain->m_types[i]->FullName);
		}

		for (int i = 0; i < GTA::ManagedGlobals::g_scriptDomain->m_types->Length; i++) {
			RegisterScriptMain(i);
		}

		SwitchToFiber(_fiberControl);
	}, nullptr);

	SwitchToFiber(fiber);
}

#pragma unmanaged
static void SHVDNProControl()
{
	_fiberControl = GetCurrentFiber();

	scriptWait(0);
	ManagedSHVDNProControl();
}

BOOL APIENTRY DllMain(HMODULE hInstance, DWORD reason, LPVOID lpReserved)
{
	if (reason == DLL_PROCESS_ATTACH) {
		UnmanagedLogWrite("DllMain DLL_PROCESS_ATTACH\n");

		DisableThreadLibraryCalls(hInstance);
		_instance = hInstance;

		ManagedInitialize();
		scriptRegister(hInstance, SHVDNProControl);
		keyboardHandlerRegister(&ScriptKeyboardMessage);
		presentCallbackRegister(&DXGIPresent);

	} else if (reason == DLL_PROCESS_DETACH) {
		UnmanagedLogWrite("DllMain DLL_PROCESS_DETACH\n");

		presentCallbackUnregister(&DXGIPresent);
		keyboardHandlerUnregister(&ScriptKeyboardMessage);
		scriptUnregister(hInstance);

		for (auto fi : _scriptFibers) {
			DeleteFiber(fi.m_fiberScript);
		}
	}

	return TRUE;
}
