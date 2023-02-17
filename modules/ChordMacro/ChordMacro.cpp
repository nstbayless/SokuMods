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
#include <map>
#include <queue>
#include <algorithm>

#pragma intrinsic(_ReturnAddress)

static char s_profilePath[1024 + MAX_PATH];

using namespace std;

#define printf(s, ...) \
	do { \
		if (gDebug) { \
			_tprintf(_T(s), ##__VA_ARGS__); \
		} \
	} while (0)

HRESULT(WINAPI *oldGetDeviceState)(LPVOID IDirectInputDevice8W, DWORD cbData, LPVOID lpvData) = NULL;

typedef void (__fastcall  *ChordInputDetect)(unsigned int param_1, unsigned int param_2, int param_1_00, char param_2_00, char param_3);
ChordInputDetect cidfn = reinterpret_cast<ChordInputDetect>((uintptr_t)(0x0046CC90));

SokuLib::BattleManager *battleMgr;

static bool enable = 1;
static int gTriggersThreshold = 500;
static int gTriggersEnabled = 0;
static size_t inputCounter = 0;
static int gDirectionalOnly = 0;
static bool gVirtualInput = true;
static bool gDebug = false;

struct VirtualInputState {
	typedef void (*InputCB)(DIJOYSTATE *);
	std::queue<InputCB> queuedInput;

	// if this counter exceeds 256, then the game isn't using this input, so delete it.
	int newFrame = 1;
};

typedef DIJOYSTATE *VirtualInputMapKey;

static std::map<VirtualInputMapKey, VirtualInputState> virtualInputStates;

#define STALEMAX 20

// this is only used in non-VirtualInput mode.
// one of these stored per player.
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
			// prevent jumping
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

// returns true if macro input should be performable at this time. (i.e. when in battle.)
static inline bool macroInputEnabled() {
	// (thanks PinkySmile!)
	// 
	// Disable outside of VSPlayer and VSCom
	if (SokuLib::sceneId != SokuLib::SCENE_BATTLE)
		return false;
	// Also disable in replays
	if (SokuLib::subMode == SokuLib::BATTLE_SUBMODE_REPLAY)
		return false;

	return enable;
}

__declspec(noinline) static void altInput()
{
	if (!macroInputEnabled())
		return;

	 if (battleMgr && !gVirtualInput) {
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
	if (gVirtualInput) {
		 // set newFrame flag to allow next virtual input sequence iterand to be performed.
		 // remove any elements that haven't been updated in >= 200 frames.
		 for (auto it = virtualInputStates.begin(); it != virtualInputStates.end();) {
			if (it->second.newFrame++ >= 200) {
				it = virtualInputStates.erase(it);
			} else {
				++it;
			}
		 }
	} else {
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
		}
	}
	int ret = (This->*og_BattleManagerOnProcess)();
	battleMgr = nullptr;
	return ret;
}

#define YAXISMULT (1)
#define AXIS_ORTHO 1000
#define AXIS_DIAG 707

static VirtualInputState::InputCB setGamepadDirCB(int dx, int dy) {
	if (dx > 0 && dy > 0)
		return [](DIJOYSTATE *joystate) { joystate->lX = joystate->lY = AXIS_DIAG; };
	if (dx < 0 && dy > 0)
		return [](DIJOYSTATE *joystate) { joystate->lX = -AXIS_DIAG; joystate->lY = AXIS_DIAG; };
	if (dx > 0)
		return [](DIJOYSTATE *joystate) { joystate->lX = AXIS_ORTHO; };
	if (dx < 0)
		return [](DIJOYSTATE *joystate) { joystate->lX = -AXIS_ORTHO; };
	if (dy > 0)
		return [](DIJOYSTATE *joystate) { joystate->lY = AXIS_ORTHO; };
	if (dy < 0)
		return [](DIJOYSTATE *joystate) { joystate->lY = -AXIS_ORTHO; };

	return [](DIJOYSTATE *joystate) {};
}

#define BUTTON_PRESSED 0x80

// Note: i <= 3 is required.
static VirtualInputState::InputCB setGamepadFaceButtonCB(size_t i) {
	if (i == 0)
		return [](DIJOYSTATE *joystate) { joystate->rgbButtons[0] |= BUTTON_PRESSED; };
	if (i == 1)
		return [](DIJOYSTATE *joystate) { joystate->rgbButtons[1] |= BUTTON_PRESSED; };
	if (i == 2)
		return [](DIJOYSTATE *joystate) { joystate->rgbButtons[2] |= BUTTON_PRESSED; };
	if (i == 3)
		return [](DIJOYSTATE *joystate) { joystate->rgbButtons[3] |= BUTTON_PRESSED; };
	
	// (paranoia)
	return [](DIJOYSTATE *joystate) {};
}

static void gamepadVirtualChordInput(DIJOYSTATE* joystate, size_t slot) {
	if (!macroInputEnabled())
		return;

	VirtualInputState &vis = virtualInputStates[joystate];

	const int iA = 0;
	const int iB = 1;
	const int iC = 2;
	const int iD = 3;
	const int ySensitivity = 400;
	const int xSensitivity = 400;

	// TODO
	const bool facingRight = false;
	const int facing = facingRight ? 1 : -1;

	// if there is no queued input yet, check for macro input.
	if (vis.queuedInput.empty() && (joystate->lZ < -gTriggersThreshold || joystate->lZ > gTriggersThreshold)) {
		
		// macros can either be A or B, depending on buttons.
		bool macroB, macroC;

		if (gDirectionalOnly) {
			 macroB = joystate->lZ > gTriggersThreshold;
			 macroC = joystate->lZ < -gTriggersThreshold;
		} else {
			 macroB = !!joystate->rgbButtons[iB];
			 macroC = !!joystate->rgbButtons[iC];
		}

		int dx = (joystate->lX > xSensitivity) - (joystate->lX < -xSensitivity);
		int dy = (joystate->lY * YAXISMULT > ySensitivity) - (joystate->lY * YAXISMULT < -ySensitivity);

		if (macroB || macroC) {
			// okay girls and boys, we're doing the chord input so let's figure out which one it is exactly and then
			// queue up the input.

			int macroButton = macroB ? iB : iC;

			if (dx == 0 && dy == 0) {
				 // 22
				vis.queuedInput.emplace(setGamepadDirCB(0, YAXISMULT));
				vis.queuedInput.emplace(setGamepadDirCB(0, 0));
				vis.queuedInput.emplace(setGamepadDirCB(0, YAXISMULT));
				vis.queuedInput.emplace(setGamepadFaceButtonCB(macroButton));
			} else if (dy == -1) {
				 // 623
				vis.queuedInput.emplace(setGamepadDirCB(facing, 0));
				vis.queuedInput.emplace(setGamepadDirCB(0, YAXISMULT));
				vis.queuedInput.emplace(setGamepadDirCB(facing, YAXISMULT));
				vis.queuedInput.emplace(setGamepadFaceButtonCB(macroButton));
			} else if (dx == facing) {
				 // 236
				vis.queuedInput.emplace(setGamepadDirCB(0, YAXISMULT));
				vis.queuedInput.emplace(setGamepadDirCB(facing, YAXISMULT)); // FIXME: is this actually needed?
				vis.queuedInput.emplace(setGamepadDirCB(facing, 0));
				vis.queuedInput.emplace(setGamepadFaceButtonCB(macroButton));
			} else if (dx == -facing) {
				// 214
				vis.queuedInput.emplace(setGamepadDirCB(0, YAXISMULT));
				vis.queuedInput.emplace(setGamepadDirCB(-facing, YAXISMULT)); // FIXME: is this actually needed?
				vis.queuedInput.emplace(setGamepadDirCB(-facing, 0));
				vis.queuedInput.emplace(setGamepadFaceButtonCB(macroButton));
			} else if (dy == 1) {
				 // 421
				vis.queuedInput.emplace(setGamepadDirCB(-facing, 0));
				vis.queuedInput.emplace(setGamepadDirCB(0, YAXISMULT));
				vis.queuedInput.emplace(setGamepadDirCB(-facing, YAXISMULT));
				vis.queuedInput.emplace(setGamepadFaceButtonCB(macroButton));
			}
		}

		// prevent jumping unless the 'fly' button is also held.
		if (!joystate->rgbButtons[iD]) {
			 if (joystate->lY * YAXISMULT < 0) {
				joystate->lY = 0;
			 }
		}
	}

	// if there is queued input, perform that.
	// (That includes input that was just queued now (above).
	if (!vis.queuedInput.empty()) {

		// zero out joystick and face buttons.
		joystate->lX = 0;
		joystate->lY = 0;
		for (size_t i = 0; i <= 3; ++i) {
			 joystate->rgbButtons[i] &= 0x7F;
		}

		// perform queued input

		auto cb = vis.queuedInput.front();
		cb(joystate);

		// if no frame was skipped, pop this input.
		if (vis.newFrame) {
			 vis.queuedInput.pop();
		}
	}

	// mark as processed so we don't skip a queued input if a frame is skipped.
	vis.newFrame = 0;
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
		virtualInputStates.clear();
		break;
	default:
		break;
	}

	// reset both on first iteration
	// this is a hack to fix UB when no second controller is plugged in
	if (inputCounter == 0 && !gVirtualInput) {
		for (size_t i = 0; i <= 1; ++i) {
			extraInput[i].buttonspressed = 0;
		}
	}

	size_t slot = inputCounter++;
	DIJOYSTATE *joystate = (DIJOYSTATE *)lpvData;

	printf("Slot %d: %d %d\n", slot, joystate->rgbButtons[0], joystate->lZ);
 
	if (gVirtualInput && gTriggersEnabled) {
		gamepadVirtualChordInput(joystate, slot);
	}

	if (slot < 2 && !gVirtualInput) {
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
		gVirtualInput = ::GetPrivateProfileInt("ChordMacro", "VirtualInput", 1, s_profilePath) != 0;
		gDebug = ::GetPrivateProfileInt("ChordMacro", "Debug", 0, s_profilePath) != 0;
		gDirectionalOnly = ::GetPrivateProfileInt("TriggerInput", "DirectionalOnly", 1, s_profilePath) != 0;
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
   
		if (!gVirtualInput) {
			ChordInputDetectDetour();
		}
		if (gTriggersEnabled) {
			DummyDirectInput();
		}

		return true;
	}
}
