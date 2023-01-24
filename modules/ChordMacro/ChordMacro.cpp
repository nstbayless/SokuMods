#include <windows.h>
#include <initguid.h>
#include <detours.h>
#include <dinput.h>
#include <shlwapi.h>
#include <SokuLib.hpp>
#include <tchar.h>
#include <stdio.h>
#include <iostream>
#include <fstream>
#include <intrin.h>

#pragma intrinsic(_ReturnAddress)

static char s_profilePath[1024 + MAX_PATH];

using namespace std;

fstream logfile;

#define printf(s, ...) _tprintf(_T(s), ##__VA_ARGS__)

HRESULT(WINAPI *oldGetDeviceState)(LPVOID IDirectInputDevice8W, DWORD cbData, LPVOID lpvData) = NULL;

typedef void (__fastcall  *ChordInputDetect)(unsigned int param_1, unsigned int param_2, int param_1_00, char param_2_00, char param_3);
ChordInputDetect cidfn = reinterpret_cast<ChordInputDetect>((uintptr_t)(0x0046CC90));

SokuLib::BattleManager *battleMgr;

static bool enable = 1;
static int gTriggersThreshold = 500;
static int gTriggersEnabled = 0;
static size_t inputCounter = 0;
static int gDirectionalOnly = 0;

#define STALEMAX 20

struct ExtraInput {
	bool lbutton;
	bool rbutton;
	bool lbuttonRelease;
	bool rbuttonRelease;
	bool used;
	bool ignore;
	int lastDir;
	int ignoreLastDir;
	int buttonspressed;
	int stale;
	bool holdA;
	bool holdB;
	bool holdC;
	bool holdD;
	bool ignoreHoldA;
	bool ignoreHoldB;
	bool ignoreHoldC;
	bool ignoreHoldD;
	bool handled;
};

int association = -1;

static ExtraInput extraInput[2];

static int sign(int x) {
	if (x == 0)
		return 0;
	if (x > 0) return 1;
	return -1;
}

__declspec(noinline) static void altInputPlayer(int slot) {
	if (slot == 1) {
		// second player only in local vs mode
		switch (SokuLib::mainMode) {
		case SokuLib::BATTLE_MODE_VSPLAYER:
			break;
		default:
			return;
		}
	}

	if (association == -1)
		return;

	// character manager and extra input for this slot.
	SokuLib::CharacterManager &characterManager = (slot == 0) ? battleMgr->leftCharacterManager : battleMgr->rightCharacterManager;
	ExtraInput &exinput = extraInput[(association == 0) ? slot : (1 - slot)];

	if (exinput.handled) return;
	exinput.handled = true;

	int facing = sign(characterManager.objectBase.direction);
	if (facing == 0) facing = 1;

	int hdir = sign(characterManager.keyMap.horizontalAxis);
	int vdir = sign(characterManager.keyMap.verticalAxis);

	// limit combo direction to non-diagonals
	if (vdir > 0 && hdir != 0)
		vdir = 0;
	if (vdir < 0 && hdir != 0)
		hdir = 0;

	exinput.lastDir = 0;
	if (exinput.lbutton || exinput.rbutton) {
		if (vdir == -1)
			exinput.lastDir = 1;
		if (vdir == 1)
			exinput.lastDir = 2;
		if (hdir == 1)
			exinput.lastDir = 3;
		if (hdir == -1)
			exinput.lastDir = 4;
	}
	if (exinput.stale >= STALEMAX)
		return;

	if (gDirectionalOnly) {
		// this style does not require pressing a face button,
		// however, it does not work with a/d buttons; it's limited to b/c only.
		if (exinput.lbuttonRelease) {
			characterManager.keyCombination._22b = 1;
		}
		if (exinput.rbuttonRelease) {
			characterManager.keyCombination._22c = 1;
		}

		if (exinput.lbutton || exinput.rbutton) {
			if (exinput.lastDir == exinput.ignoreLastDir)
				return;
		}

		if (exinput.lbutton) {
			if (hdir == 1 * facing) {
				characterManager.keyCombination._236b = 1;
				exinput.used = true;
			}
			if (hdir == -1 * facing) {
				characterManager.keyCombination._214b = 1;
				exinput.used = true;
			}
			if (vdir == -1) {
				characterManager.keyCombination._623b = 1;
				// prevent jumping
				characterManager.keyMap.verticalAxis = 0;
				exinput.used = true;
			}
			if (vdir == 1) {
				characterManager.keyCombination._421b = 1;
				exinput.used = true;
			}
		}
		if (exinput.rbutton) {
			if (hdir == 1 * facing) {
				characterManager.keyCombination._236c = 1;
				exinput.used = true;
			}
			if (hdir == -1 * facing) {
				characterManager.keyCombination._214c = 1;
				exinput.used = true;
			}
			if (vdir == -1) {
				characterManager.keyCombination._623c = 1;
				// prevent jumping
				characterManager.keyMap.verticalAxis = 0;
				exinput.used = true;
			}
			if (vdir == 1) {
				characterManager.keyCombination._421c = 1;
				exinput.used = true;
			}
		}
	} else {
		// all this to distinguish hold from press..
		bool a = characterManager.keyMap.a;
		bool b = characterManager.keyMap.b;
		bool c = characterManager.keyMap.c;
		bool d = characterManager.keyMap.d;
		
		exinput.holdA = a;
		exinput.holdB = b;
		exinput.holdC = c;
		exinput.holdD = d;

		// a/b/c/d should be true only if pressed this frame.
		if (exinput.ignoreHoldA) a = false;
		if (exinput.ignoreHoldB) b = false;
		if (exinput.ignoreHoldC) c = false;
		if (exinput.ignoreHoldD) d = false;

		if (exinput.lbutton || exinput.rbutton) {
			// prevent jumping (unless dashing)
			if (characterManager.keyMap.verticalAxis < 0 && !d) {
				characterManager.keyMap.verticalAxis = 0;
			}
			if (hdir == 0 && vdir == 0) {
				if (a) characterManager.keyCombination._22a = true;
				if (b) characterManager.keyCombination._22b = true;
				if (c) characterManager.keyCombination._22c = true;
				if (d) characterManager.keyCombination._22d = true;
			} else if (vdir == 1) {
				if (a) characterManager.keyCombination._421a = true;
				if (b) characterManager.keyCombination._421b = true;
				if (c) characterManager.keyCombination._421c = true;
				if (d) characterManager.keyCombination._421d = true;
			} else if (vdir == -1) {
				if (a) characterManager.keyCombination._623a = true;
				if (b) characterManager.keyCombination._623b = true;
				if (c) characterManager.keyCombination._623c = true;
				if (d) characterManager.keyCombination._623d = true;
			} else if (hdir == 1 * facing) {
				if (a) characterManager.keyCombination._236a = true;
				if (b) characterManager.keyCombination._236b = true;
				if (c) characterManager.keyCombination._236c = true;
				if (d) characterManager.keyCombination._236d = true;
			} else if (hdir == -1 * facing) {
				if (a) characterManager.keyCombination._214a = true;
				if (b) characterManager.keyCombination._214b = true;
				if (c) characterManager.keyCombination._214c = true;
				if (d) characterManager.keyCombination._214d = true;
			}
		}
	}
}

__declspec(noinline) static void associateInputs() {

	int charButtonCount[2] = {0, 0};
	for (size_t slot = 0; slot <= 1; ++slot) {
		SokuLib::CharacterManager &characterManager = (slot == 0) ? battleMgr->leftCharacterManager : battleMgr->rightCharacterManager;
		if (characterManager.keyMap.a)
			charButtonCount[slot]++;
		if (characterManager.keyMap.b)
			charButtonCount[slot]++;
		if (characterManager.keyMap.c)
			charButtonCount[slot]++;
		if (characterManager.keyMap.d)
			charButtonCount[slot]++;
	}

	if (association == -1) {
		if (charButtonCount[0] != charButtonCount[1]) {
			if (extraInput[0].buttonspressed == charButtonCount[0] && extraInput[1].buttonspressed == charButtonCount[1])
				association = 0;
			if (extraInput[1].buttonspressed == charButtonCount[0] && extraInput[0].buttonspressed == charButtonCount[1])
				association = 1;
			if (association != -1) {
				printf("Associated %d\n", association);
			}
		}
	}
}

__declspec(noinline) static void altInput()
{
	 // (thanks PinkySmile!)
	 //Disable outside of VSPlayer and VSCom
	 if (SokuLib::sceneId != SokuLib::SCENE_BATTLE) return;
	 //Also disable in replays
	 if (SokuLib::subMode == SokuLib::BATTLE_SUBMODE_REPLAY)
		 return;

	 if (battleMgr && enable) {
		 associateInputs();
		 altInputPlayer(0);
		 altInputPlayer(1);
	 }
}

 void __fastcall MyChordInputDetect(unsigned int param_1,unsigned int param_2,int param_1_00,char param_2_00,char param_3)
 {
	altInput();

	cidfn(param_1, param_2, param_1_00, param_2_00, param_3);
 }

static int (SokuLib::BattleManager::*og_BattleManagerOnProcess)() = nullptr;

static int __fastcall BattleOnProcess(SokuLib::BattleManager *This)
{
	battleMgr = This;
	inputCounter = 0; 
	for (size_t i = 0; i <= 1; ++i) {
		 extraInput[i].ignore = extraInput[i].used;
		 extraInput[i].ignoreLastDir = extraInput[i].lastDir;
		 extraInput[i].ignoreHoldA = extraInput[i].holdA;
		 extraInput[i].ignoreHoldB = extraInput[i].holdB;
		 extraInput[i].ignoreHoldC = extraInput[i].holdC;
		 extraInput[i].ignoreHoldD = extraInput[i].holdD;
		 if (extraInput[i].stale < STALEMAX) {
			extraInput[i].stale ++;
		 }
		 extraInput[i].handled = false;
	}
	int ret = (This->*og_BattleManagerOnProcess)();
	battleMgr = nullptr;
	return ret;
}

static HRESULT WINAPI myGetDeviceState(LPVOID IDirectInputDevice8W, DWORD cbData, LPVOID lpvData) {
	uintptr_t returnaddress = reinterpret_cast<uintptr_t>(_ReturnAddress());

	HRESULT retValue = oldGetDeviceState(IDirectInputDevice8W, cbData, lpvData);

	if (retValue != DI_OK || !(cbData == sizeof(DIJOYSTATE) || cbData == sizeof(DIJOYSTATE2))) {
		return retValue;
	}

	// reset association in menu
	switch (SokuLib::sceneId) {
	case SokuLib::SCENE_LOGO:
	case SokuLib::SCENE_OPENING:
	case SokuLib::SCENE_TITLE:
		association = -1;
		break;
	default:
		break;
	}

	// reset both on first iteration
	// this is a hack to fix UB when no second controller is plugged in
	if (inputCounter == 0) {
		for (size_t i = 0; i <= 1; ++i) {
			extraInput[i].buttonspressed = 0;
		}
	}

	size_t slot = inputCounter++;
 
	if (slot < 2) {
		DIJOYSTATE *joystate = (DIJOYSTATE *)lpvData;

		ExtraInput &exin = extraInput[slot];
		bool prevl = exin.lbutton;
		bool prevr = exin.rbutton;
		exin.lbutton = false;
		exin.rbutton = false;
		exin.lbuttonRelease = false;
		exin.rbuttonRelease = false;
		exin.buttonspressed = 0;
		for (size_t i = 0; i <= 3; ++i) {
			if (joystate->rgbButtons[i])
				exin.buttonspressed++;
		}

		exin.stale = 0;

		if (gTriggersEnabled) {
			if (joystate->lZ < -gTriggersThreshold) {
				exin.rbutton = true;
			}

			if (joystate->lZ > gTriggersThreshold) {
				exin.lbutton = true;
			}

			if (!exin.used) {
				if (prevl && !exin.lbutton) {
					exin.lbuttonRelease = true;
				}

				if (prevr && !exin.rbutton) {
					exin.rbuttonRelease = true;
				}
			}
		}

		if (!exin.lbutton && !exin.rbutton) {
			exin.used = false;
		}
	}

	return retValue;
}

static void DummyDirectInput() {
	LPDIRECTINPUT8 pDI;
	DirectInput8Create(GetModuleHandle(NULL), DIRECTINPUT_VERSION, IID_IDirectInput8, (void **)&pDI, NULL);

	LPDIRECTINPUTDEVICE8 pKeyboard;
	pDI->CreateDevice(GUID_SysKeyboard, &pKeyboard, NULL);

	LPVOID ptrGetDeviceState = *(((LPVOID *)(*(LPVOID *)(pKeyboard))) + 9);

	oldGetDeviceState = (HRESULT(WINAPI *)(LPVOID, DWORD, LPVOID))ptrGetDeviceState;

	DetourRestoreAfterWith();

	DetourTransactionBegin();
	DetourUpdateThread(GetCurrentThread());
	DetourAttach(&(PVOID &)oldGetDeviceState, myGetDeviceState);
	DetourTransactionCommit();

	pKeyboard->Release();
	pDI->Release();
}

static void ChordInputDetectDetour()
{
    DetourTransactionBegin();
    DetourUpdateThread(GetCurrentThread());
    DetourAttach(&(PVOID&)cidfn, MyChordInputDetect);
    DetourTransactionCommit();
}

extern "C" {
__declspec(dllexport) bool CheckVersion(const BYTE hash[16]) {
	return true;
}

__declspec(dllexport) bool Initialize(HMODULE hMyModule, HMODULE hParentModule) {
	::GetModuleFileName(hMyModule, s_profilePath, 1024);
	::PathRemoveFileSpec(s_profilePath);
	::PathAppend(s_profilePath, "ChordMacro.ini");

	enable = ::GetPrivateProfileInt("ChordMacro", "Enabled", 1, s_profilePath) != 0;
	gDirectionalOnly = ::GetPrivateProfileInt("ChordMacro", "DirectionalOnly", 1, s_profilePath) != 0;
	gTriggersEnabled = ::GetPrivateProfileInt("TriggerInput", "Enabled", 0, s_profilePath) != 0;
	gTriggersThreshold = ::GetPrivateProfileInt("TriggerInput", "Threshold", 200, s_profilePath);
	association = -1;

	// load DirectInput library since it won't be otherwise loaded yet
    
	if (!LoadLibraryExW(L"dinput8.dll", NULL, 0)) {
		return false;
	}

    DWORD old;
    VirtualProtect((PVOID)RDATA_SECTION_OFFSET, RDATA_SECTION_SIZE, PAGE_EXECUTE_WRITECOPY, &old);
	og_BattleManagerOnProcess = SokuLib::TamperDword(&SokuLib::VTable_BattleManager.onProcess, BattleOnProcess);
	VirtualProtect((PVOID)RDATA_SECTION_OFFSET, RDATA_SECTION_SIZE, old, &old);
   
    ChordInputDetectDetour();
	DummyDirectInput();

	return true;
}
}
