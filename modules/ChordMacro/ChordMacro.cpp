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
#include <functional>

#pragma intrinsic(_ReturnAddress)

#ifndef vtbl_CBattleManagerStory
#define vtbl_CBattleManagerStory 0x858934
#endif

#ifndef vtbl_CBattleManagerArcade
#define vtbl_CBattleManagerArcade 0x85899c
#endif

SokuLib::BattleManager_VTABLE &VTable_BattleManagerStory = *reinterpret_cast<SokuLib::BattleManager_VTABLE *>(vtbl_CBattleManagerStory);
SokuLib::BattleManager_VTABLE &VTable_BattleManagerArcade = *reinterpret_cast<SokuLib::BattleManager_VTABLE *>(vtbl_CBattleManagerArcade);

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

const size_t MAX_RGBBUTTON_INDEX = sizeof(DIJOYSTATE::rgbButtons);

// how many frames we have to wait after pressing 3 before a 236 will register instead of a 623
#define DISTINCTION_FRAMES_236_623 11

struct KeyboardInputState {
	bool enabled = false;
	bool enabled2 = false;
	uint8_t iMacro = DIK_LSHIFT;
	uint8_t iMacro2 = DIK_LCONTROL;
	bool macroKeyDown;
	bool macroKeyRelease;
	bool macroKey2Down;
	bool macroKey2Release;

	// only used in virtual input mode.
	typedef std::function<void(char *)> InputCB;
	std::queue<InputCB> queuedInput;

	// which way the character using this input state is facing
	// (read from SokuLib::CharacterManager)
	bool facingRight = false;

	// keyboard mapping (DirectX keycodes)
	// (read from SokuLib::KeyManager)
	int iUp;
	int iDown;
	int iLeft;
	int iRight;
	int iA;
	int iB;
	int iC;
	int iD;

	int stale = 0;

	bool used = false; // in virtual directionalOnly mode, prevents triggering 22B/C if another chord was already performed.
	int newFrame = 0;

	bool virtualMacroBPrev = false;
	bool virtualMacroCPrev = false;
	int dxprevs[DISTINCTION_FRAMES_236_623];
};

static KeyboardInputState keyboardstate[2];

// for gamepads
struct VirtualInputState {
	//typedef void (*InputCB)(DIJOYSTATE *);
	typedef std::function<void(DIJOYSTATE *)> InputCB;
	std::queue<InputCB> queuedInput;

	// if this counter exceeds 256, then the game isn't using this input, so delete it.
	int newFrame = 0;

	// which way the character using this input state is facing
	// (read from SokuLib::CharacterManager)
	bool facingRight = false;

	// rgb button index mapping for gamepad buttons A/B/C/D.
	// (read from SokuLib::KeyManager)
	size_t iA = -1;
	size_t iB = -1;
	size_t iC = -1;
	size_t iD = -1;

	bool used = false;

	bool virtualMacroBPrev = false;
	bool virtualMacroCPrev = false;
	int dxprevs[DISTINCTION_FRAMES_236_623];
};

typedef int VirtualInputMapKey;

static std::map<VirtualInputMapKey, VirtualInputState> virtualInputStates;

#define STALEMAX 20

// this is only used in non-VirtualInput mode.
// one of these stored per gamepad.
struct ExtraInputGamepad {
	bool lbutton;
	bool rbutton;
	bool lbuttonRelease;
	bool rbuttonRelease;
	int stale;
	bool used;
};

// This is only used in non-virtual-input mode
// one of these is stored per character.
struct ExtraInput {
	int lastDir;
	int ignoreLastDir;
	bool holdA;
	bool holdB;
	bool holdC;
	bool holdD;
	bool ignoreHoldA;
	bool ignoreHoldB;
	bool ignoreHoldC;
	bool ignoreHoldD;
};

// this is only used in non-VirtualInput mode.
// one of these is used per character

enum class InputAssociation {
	UNKNOWN,
	COMPUTER,
	KEYBOARD,
	JOY0,
	JOY1
};

static InputAssociation getInputAssociationForCharacterManager(SokuLib::CharacterManager &characterManager) {
	if (!characterManager.keyManager || !characterManager.keyManager->keymapManager) {
		return InputAssociation::COMPUTER;
	} else switch (characterManager.keyManager->keymapManager->isPlayer) {
		case -1:
			return InputAssociation::KEYBOARD;
		case 0:
			return InputAssociation::JOY0;
		case 1:
			return InputAssociation::JOY1;
		default:
			return InputAssociation::UNKNOWN;
	}
}

// this is used for non-virtual input.
static ExtraInputGamepad extraInputGamepad[2];

// this is used for both virtual and non-virtual input.
// in the case of non-virtual input, one is used per player.
// in the case of virtual input, 2 are used for the gamepads, and another 2 for the keyboards.
static ExtraInput extraInput[2];

static ExtraInput extraInputVirtualKeyboards[2];

static int sign(int x) {
	if (x == 0)
		return 0;
	if (x > 0) return 1;
	return -1;
}

static void altInput(SokuLib::CharacterManager &characterManager, bool& used, int stale, bool lbutton, bool rbutton, bool lrelease, bool rrelease) {
	int facing = sign(characterManager.objectBase.direction);
	if (facing == 0)
		facing = 1;

	int hdir = sign(characterManager.keyMap.horizontalAxis);
	int vdir = sign(characterManager.keyMap.verticalAxis);

	// limit combo direction to non-diagonals
	if (vdir > 0 && hdir != 0)
		vdir = 0;
	if (vdir < 0 && hdir != 0)
		hdir = 0;

	ExtraInput &exinput = extraInput[characterManager.isRightPlayer ? 1 : 0];

	exinput.lastDir = 0;
	if (lbutton || rbutton) {
		if (vdir == -1)
			exinput.lastDir = 1;
		if (vdir == 1)
			exinput.lastDir = 2;
		if (hdir == 1)
			exinput.lastDir = 3;
		if (hdir == -1)
			exinput.lastDir = 4;
	} else {
		used = false;
	}

	if (stale >= STALEMAX)
		return;

	if (gDirectionalOnly) {
		// this style does not require pressing a face button,
		// however, it does not work with a/d buttons; it's limited to b/c only.
		if (lrelease) {
			characterManager.keyCombination._22b = 1;
		}
		if (rrelease) {
			characterManager.keyCombination._22c = 1;
		}

		if (lbutton || rbutton) {
			if (exinput.lastDir == exinput.ignoreLastDir)
				return;
		}

		if (lbutton) {
			if (hdir == 1 * facing) {
				characterManager.keyCombination._236b = 1;
				used = true;
			}
			if (hdir == -1 * facing) {
				characterManager.keyCombination._214b = 1;
				used = true;
			}
			if (vdir == -1) {
				characterManager.keyCombination._623b = 1;
				// prevent jumping
				characterManager.keyMap.verticalAxis = 0;
				used = true;
			}
			if (vdir == 1) {
				characterManager.keyCombination._421b = 1;
				used = true;
			}
		}
		if (rbutton) {
			if (hdir == 1 * facing) {
				characterManager.keyCombination._236c = 1;
				used = true;
			}
			if (hdir == -1 * facing) {
				characterManager.keyCombination._214c = 1;
				used = true;
			}
			if (vdir == -1) {
				characterManager.keyCombination._623c = 1;
				// prevent jumping
				characterManager.keyMap.verticalAxis = 0;
				used = true;
			}
			if (vdir == 1) {
				characterManager.keyCombination._421c = 1;
				used = true;
			}
		}
	} else {
		// all this to distinguish hold from press...
		bool a = characterManager.keyMap.a;
		bool b = characterManager.keyMap.b;
		bool c = characterManager.keyMap.c;
		bool d = characterManager.keyMap.d;

		exinput.holdA = a;
		exinput.holdB = b;
		exinput.holdC = c;
		exinput.holdD = d;

		// a/b/c/d should be true only if pressed this frame.
		if (exinput.ignoreHoldA)
			a = false;
		if (exinput.ignoreHoldB)
			b = false;
		if (exinput.ignoreHoldC)
			c = false;
		if (exinput.ignoreHoldD)
			d = false;

		if (lbutton || rbutton) {
			// prevent jumping
			if (characterManager.keyMap.verticalAxis < 0 && !d) {
				characterManager.keyMap.verticalAxis = 0;
			}
			if (hdir == 0 && vdir == 0) {
				if (a)
					characterManager.keyCombination._22a = true;
				if (b)
					characterManager.keyCombination._22b = true;
				if (c)
					characterManager.keyCombination._22c = true;
				if (d)
					characterManager.keyCombination._22d = true;
			} else if (vdir == 1) {
				if (a)
					characterManager.keyCombination._421a = true;
				if (b)
					characterManager.keyCombination._421b = true;
				if (c)
					characterManager.keyCombination._421c = true;
				if (d)
					characterManager.keyCombination._421d = true;
			} else if (vdir == -1) {
				if (a)
					characterManager.keyCombination._623a = true;
				if (b)
					characterManager.keyCombination._623b = true;
				if (c)
					characterManager.keyCombination._623c = true;
				if (d)
					characterManager.keyCombination._623d = true;
			} else if (hdir == 1 * facing) {
				if (a)
					characterManager.keyCombination._236a = true;
				if (b)
					characterManager.keyCombination._236b = true;
				if (c)
					characterManager.keyCombination._236c = true;
				if (d)
					characterManager.keyCombination._236d = true;
			} else if (hdir == -1 * facing) {
				if (a)
					characterManager.keyCombination._214a = true;
				if (b)
					characterManager.keyCombination._214b = true;
				if (c)
					characterManager.keyCombination._214c = true;
				if (d)
					characterManager.keyCombination._214d = true;
			}
		}
	}
}

static void altInputPlayerKeyboard(SokuLib::CharacterManager& characterManager) {
	KeyboardInputState &kis = keyboardstate[characterManager.isRightPlayer ? 1 : 0];
	if (kis.enabled || kis.enabled2) {
		altInput(characterManager, kis.used, kis.stale, kis.macroKeyDown, kis.macroKey2Down, kis.macroKeyRelease, kis.macroKey2Release);
	}
}

static void altInputPlayerGamepad(SokuLib::CharacterManager &characterManager, ExtraInputGamepad &exinput) {
	if (gTriggersEnabled) {
		altInput(characterManager, exinput.used, exinput.stale, exinput.lbutton, exinput.rbutton, exinput.lbuttonRelease, exinput.rbuttonRelease);
	}
}

__declspec(noinline) static void altInputPlayer(int slot) {
	if (slot == 1) {
		// player 1 only, except local vs mode
		switch (SokuLib::mainMode) {
		case SokuLib::BATTLE_MODE_VSPLAYER:
			break;
		default:
			return;
		}
	}

	// character manager and extra input for this slot.
	SokuLib::CharacterManager &characterManager = (slot == 0) ? battleMgr->leftCharacterManager : battleMgr->rightCharacterManager;

	switch (getInputAssociationForCharacterManager(characterManager)) {
	case InputAssociation::KEYBOARD:
		altInputPlayerKeyboard(characterManager);
		break;
	case InputAssociation::JOY0:
		altInputPlayerGamepad(characterManager, extraInputGamepad[0]);
		break;
	case InputAssociation::JOY1:
		altInputPlayerGamepad(characterManager, extraInputGamepad[1]);
		break;
	default:
		return;
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
		 altInputPlayer(0);
		 altInputPlayer(1);
	 }
}

void __fastcall MyChordInputDetect(unsigned int param_1,unsigned int param_2,int param_1_00,char param_2_00,char param_3)
{
	altInput();

	cidfn(param_1, param_2, param_1_00, param_2_00, param_3);
}

// figures out which controllers are facing left vs which are facing right.
// also figures out the button mapping for each controller, reading from SokuLib::KeyManager
static void setVirtualInputStateGameData(int slot) {
	if (battleMgr) {
		 SokuLib::CharacterManager &characterManager = (slot == 0) ? battleMgr->leftCharacterManager : battleMgr->rightCharacterManager;
		 bool facingRight = characterManager.objectBase.direction == SokuLib::Direction::RIGHT;
		 switch (InputAssociation ia = getInputAssociationForCharacterManager(characterManager)) {
		 case InputAssociation::KEYBOARD:
			keyboardstate[slot].facingRight = facingRight;
			keyboardstate[slot].iUp = characterManager.keyManager->keymapManager->bindingUp;
			keyboardstate[slot].iDown = characterManager.keyManager->keymapManager->bindingDown;
			keyboardstate[slot].iLeft = characterManager.keyManager->keymapManager->bindingLeft;
			keyboardstate[slot].iRight = characterManager.keyManager->keymapManager->bindingRight;
			keyboardstate[slot].iA = characterManager.keyManager->keymapManager->bindingA;
			keyboardstate[slot].iB = characterManager.keyManager->keymapManager->bindingB;
			keyboardstate[slot].iC = characterManager.keyManager->keymapManager->bindingC;
			keyboardstate[slot].iD = characterManager.keyManager->keymapManager->bindingD;
			break;
		 case InputAssociation::JOY0:
		 case InputAssociation::JOY1: {
			int i = (ia == InputAssociation::JOY0) ? 0 : 1;
			virtualInputStates[i].facingRight = facingRight;
			virtualInputStates[i].iA = characterManager.keyManager->keymapManager->bindingA;
			virtualInputStates[i].iB = characterManager.keyManager->keymapManager->bindingB;
			virtualInputStates[i].iC = characterManager.keyManager->keymapManager->bindingC;
			virtualInputStates[i].iD = characterManager.keyManager->keymapManager->bindingD;
			break;
		 }
		 default:
			return;
		 }
	}
}

static int (SokuLib::BattleManager::*og_BattleManagerOnProcess)() = nullptr;
static int (SokuLib::BattleManager::*og_BattleManagerStoryOnProcess)() = nullptr;
static int (SokuLib::BattleManager::*og_BattleManagerArcadeOnProcess)() = nullptr;

static void interceptBattleOnProcess() {
	inputCounter = 0;
	if (gVirtualInput) {
		 setVirtualInputStateGameData(0);
		 setVirtualInputStateGameData(1);

		 // set newFrame flag to allow next virtual input sequence iterand to be performed.
		 // remove any devices that haven't been seen in >= 200 frames.
		 keyboardstate[0].newFrame = 1;
		 keyboardstate[1].newFrame = 1;
		 for (auto it = virtualInputStates.begin(); it != virtualInputStates.end();) {
			if (it->second.newFrame++ >= 200) {
				it = virtualInputStates.erase(it);
			} else {
				++it;
			}
		 }
	} else {
		 for (size_t i = 0; i <= 1; ++i) {
			extraInput[i].ignoreLastDir = extraInput[i].lastDir;
			extraInput[i].ignoreHoldA = extraInput[i].holdA;
			extraInput[i].ignoreHoldB = extraInput[i].holdB;
			extraInput[i].ignoreHoldC = extraInput[i].holdC;
			extraInput[i].ignoreHoldD = extraInput[i].holdD;
			if (extraInputGamepad[i].stale < STALEMAX) {
				extraInputGamepad[i].stale++;
			}
			if (keyboardstate[i].stale < STALEMAX) {
				keyboardstate[i].stale++;
			}
		 }
	}
}

static int __fastcall BattleOnProcess(SokuLib::BattleManager *This)
{
	battleMgr = This;
	interceptBattleOnProcess();
	int ret = (This->*og_BattleManagerOnProcess)();
	battleMgr = nullptr;
	return ret;
}

static int __fastcall BattleOnProcessStory(SokuLib::BattleManager *This) {
	battleMgr = This;
	interceptBattleOnProcess();
	int ret = (This->*og_BattleManagerStoryOnProcess)();
	battleMgr = nullptr;
	return ret;
}

static int __fastcall BattleOnProcessArcade(SokuLib::BattleManager *This) {
	battleMgr = This;
	interceptBattleOnProcess();
	int ret = (This->*og_BattleManagerArcadeOnProcess)();
	battleMgr = nullptr;
	return ret;
}

#define AXIS_ORTHO 1000
#define AXIS_DIAG 707
#define BUTTON_PRESSED 0x80

struct KeyboardVirtualArgs {
	char *keystate;
	KeyboardInputState &is;
	ExtraInput &exin;

	// returns argument for queued input, and zeroes out relevant muddling input.
	char *prepareQueuedInput() const {
		keystate[is.iUp]    &= 0x7F;
		keystate[is.iDown]  &= 0x7F;
		keystate[is.iLeft]  &= 0x7F;
		keystate[is.iRight] &= 0x7F;
		keystate[is.iA]     &= 0x7F;
		keystate[is.iB]     &= 0x7F;
		keystate[is.iC]     &= 0x7F;
		keystate[is.iD]     &= 0x7F;

		return keystate;
	}

	KeyboardInputState::InputCB setDirCB(int dx, int dy) const {
		dx = sign(dx);
		dy = sign(dy);
		const int iUp = is.iUp;
		const int iDown = is.iDown;
		const int iLeft = is.iLeft;
		const int iRight = is.iRight;
		return [dx, dy, iUp, iDown, iLeft, iRight](char* keystate) {
			if (dx < 0) keystate[iLeft]  |= 0x80;
			if (dx > 0) keystate[iRight] |= 0x80;
			if (dy < 0) keystate[iUp]    |= 0x80;
			if (dy > 0) keystate[iDown]  |= 0x80;
		};
	}

	KeyboardInputState::InputCB setButtonCB(uint8_t i) const {
		return [i](char* keystate) { keystate[i] |= BUTTON_PRESSED; };
	}

	std::pair<int, int> getDirectionalInput() const {
		return {!!keystate[is.iRight] - !!keystate[is.iLeft], !!keystate[is.iDown] - !!keystate[is.iUp]};
	}

	void preventJumping() const {
		if (!keystate[is.iD]) {
			 keystate[is.iUp] &= 0x7f;
		}
	}

	bool getMacroInput() const {
		if (is.enabled && keystate[is.iMacro]) return true;
		if (is.enabled2 && keystate[is.iMacro2]) return true;

		return false;
	}

	bool getMacroBInput(bool directionalOnly) const {
		if (directionalOnly) {
			 return is.enabled && !!keystate[is.iMacro];
		} else {
			 return !!keystate[is.iB];
		}
	}

	bool getMacroCInput(bool directionalOnly) const {
		if (directionalOnly) {
			 return is.enabled2 && !!keystate[is.iMacro2];
		} else {
			 return !!keystate[is.iC];
		}
	}
};

struct GamepadVirtualArgs {
	DIJOYSTATE *joystate;
	size_t slot;
	VirtualInputState &is;
	ExtraInput &exin;

	static const int ySensitivity = 400;
	static const int xSensitivity = 400;

	// returns argument for queued input, and zeroes out relevant muddling input.
	DIJOYSTATE* prepareQueuedInput() const {
		if (is.iA < MAX_RGBBUTTON_INDEX) joystate->rgbButtons[is.iA] &= 0x7F;
		if (is.iB < MAX_RGBBUTTON_INDEX) joystate->rgbButtons[is.iB] &= 0x7F;
		if (is.iC < MAX_RGBBUTTON_INDEX) joystate->rgbButtons[is.iC] &= 0x7F;
		if (is.iD < MAX_RGBBUTTON_INDEX) joystate->rgbButtons[is.iD] &= 0x7F;

		joystate->lX = 0;
		joystate->lY = 0;

		return joystate;
	}

	VirtualInputState::InputCB setDirCB(int dx, int dy) const {
		if (dx > 0 && dy > 0)
			 return [](DIJOYSTATE *joystate) { joystate->lX = joystate->lY = AXIS_DIAG; };
		if (dx < 0 && dy > 0)
			 return [](DIJOYSTATE *joystate) {
				 joystate->lX = -AXIS_DIAG;
				 joystate->lY = AXIS_DIAG;
			 };
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

	VirtualInputState::InputCB setButtonCB(size_t i) const {
		if (i < MAX_RGBBUTTON_INDEX)
			 return [i](DIJOYSTATE *joystate) { joystate->rgbButtons[i] |= BUTTON_PRESSED; };

		// (paranoia)
		return [](DIJOYSTATE *joystate) {};
	}

	std::pair<int, int> getDirectionalInput() const {
		return {
			(joystate->lX > xSensitivity) - (joystate->lX < -xSensitivity),
			(joystate->lY > ySensitivity) - (joystate->lY < -ySensitivity)
		};
	}

	void preventJumping() const {
		if (is.iD < MAX_RGBBUTTON_INDEX && !joystate->rgbButtons[is.iD]) {
			if (joystate->lY < 0) {
				joystate->lY = 0;
			}
		}
	}

	bool getMacroInput() const {
		return joystate->lZ > gTriggersThreshold || joystate->lZ < -gTriggersThreshold;
	}

	bool getMacroBInput(bool directionalOnly) const {
		if (directionalOnly) {
			return joystate->lZ > gTriggersThreshold;
		} else {
			return is.iB < MAX_RGBBUTTON_INDEX ? !!joystate->rgbButtons[is.iB] : false;
		}
	}

	bool getMacroCInput(bool directionalOnly) const {
		if (directionalOnly) {
			return joystate->lZ < -gTriggersThreshold;
		} else {
			return is.iB < MAX_RGBBUTTON_INDEX ? !!joystate->rgbButtons[is.iB] : false;
		}
	}
};

template<typename C>
static void virtualChordInput(const C& args) {
	if (!macroInputEnabled())
		return;

	const size_t iA = args.is.iA;
	const size_t iB = args.is.iB;
	const size_t iC = args.is.iC;
	const size_t iD = args.is.iD;

	const bool facingRight = args.is.facingRight;
	const int facing = facingRight ? 1 : -1;

	// if there is no queued input yet, check for macro input.
	if (args.is.queuedInput.empty() && args.is.newFrame) {
		
		// macros can either be A or B, depending on buttons.
		bool macroB = args.getMacroBInput(gDirectionalOnly);
		bool macroC = args.getMacroCInput(gDirectionalOnly);
		std::pair<int, int> dxy = args.getDirectionalInput();
		int dx = dxy.first;
		int dy = dxy.second;

		args.exin.lastDir = 0;

		auto queue22 = [&args](int macroButton) {
			args.is.queuedInput.emplace(args.setDirCB(0, 1));
			args.is.queuedInput.emplace(args.setDirCB(0, 0));
			args.is.queuedInput.emplace(args.setDirCB(0, 1));
			args.is.queuedInput.emplace(args.setButtonCB(macroButton));
		};

		// returns a value in the range 0-11 inclusive
		auto get623WaitTime = [&args](int facing) -> int {
			for (size_t i = DISTINCTION_FRAMES_236_623; i-- > 0;) {
				if (args.is.dxprevs[i] == facing)
					return i + 1;
			}
			return 0;
		};

		if ((macroB || macroC) && args.getMacroInput()) {
			// okay girls and boys, we're doing the chord input so let's figure out which one it is exactly and then
			// queue up the input.

			int macroButton = macroB ? iB : iC;
			args.is.virtualMacroBPrev = macroB;
			args.is.virtualMacroCPrev = macroC;

			if (dy == -1)      args.exin.lastDir = 1;
			else if (dx == -1) args.exin.lastDir = 2;
			else if (dx == 1)  args.exin.lastDir = 3;
			else if (dy == 1)  args.exin.lastDir = 2;

			if (!gDirectionalOnly || args.exin.lastDir != args.exin.ignoreLastDir) {
				if (dx == 0 && dy == 0 && !gDirectionalOnly) {
					// 22
					queue22(macroButton);
				} else if (dy == -1) {
					// 623
					args.is.queuedInput.emplace(args.setDirCB(0, 1));
					args.is.queuedInput.emplace(args.setDirCB(facing, 1));
					args.is.queuedInput.emplace(args.setButtonCB(macroButton));
					args.is.used = true;
				} else if (dx == facing) {
					// 236
					const size_t wait_time = get623WaitTime(facing);
					for (size_t i = 0; i < wait_time; ++i) {
						args.is.queuedInput.emplace(args.setDirCB(0, 0));
					}
					args.is.queuedInput.emplace(args.setDirCB(0, 1));
					args.is.queuedInput.emplace(args.setDirCB(facing, 1));
					args.is.queuedInput.emplace(args.setDirCB(facing, 0));
					args.is.queuedInput.emplace(args.setButtonCB(macroButton));
					args.is.used = true;
				} else if (dx == -facing) {
					// 214
					const size_t wait_time = get623WaitTime(-facing);
					for (size_t i = 0; i < wait_time; ++i) {
						args.is.queuedInput.emplace(args.setDirCB(0, 0));
					}
					args.is.queuedInput.emplace(args.setDirCB(0, 1));
					args.is.queuedInput.emplace(args.setDirCB(-facing, 1));
					args.is.queuedInput.emplace(args.setDirCB(-facing, 0));
					args.is.queuedInput.emplace(args.setButtonCB(macroButton));
					args.is.used = true;
				} else if (dy == 1) {
					// 421
					args.is.queuedInput.emplace(args.setDirCB(-facing, 0));
					args.is.queuedInput.emplace(args.setDirCB(0, 1));
					args.is.queuedInput.emplace(args.setDirCB(-facing, 1));
					args.is.queuedInput.emplace(args.setButtonCB(macroButton));
					args.is.used = true;
				}
			}
		} else {
			if (gDirectionalOnly && dx == 0 && dy == 0 && !args.is.used) {
				if (args.is.virtualMacroBPrev)
					queue22(iB);
				else if (args.is.virtualMacroCPrev)
					queue22(iC);
			}

			args.is.used = false;

			args.is.virtualMacroBPrev = false;
			args.is.virtualMacroCPrev = false;
		}

		// record dx for the dxprevs buffer
		for (size_t i = 0; i < DISTINCTION_FRAMES_236_623 - 1; ++i) {
			args.is.dxprevs[i] = args.is.dxprevs[i + 1];
		}
		args.is.dxprevs[DISTINCTION_FRAMES_236_623 - 1] = dx;

		args.exin.ignoreLastDir = args.exin.lastDir;

		// prevent jumping if macro button is held unless the 'fly' button is also held.
		if (args.getMacroInput()) args.preventJumping();
	}

	// if there is queued input, perform that.
	// (That includes input that was queued this frame, just now, above.
	if (!args.is.queuedInput.empty()) {

		// zero out muddling input (e.g. joystick and face buttons.), and
		// perform queued input
		args.is.queuedInput.front()(args.prepareQueuedInput());

		// if no frame was skipped, pop this input and move on to the next.
		if (args.is.newFrame) {
			args.is.queuedInput.pop();
		}
	}

	// mark as processed so we don't skip a queued input if a frame is skipped.
	args.is.newFrame = 0;
}

static void interceptDeviceStateKeyboard(char *keystate) {
	for (size_t i = 0; i <= 1; ++i) {
		KeyboardInputState &kis = keyboardstate[i];
		if ((kis.enabled || kis.enabled2) && macroInputEnabled()) {
			if (gVirtualInput) {
				virtualChordInput<KeyboardVirtualArgs>({keystate, kis, extraInputVirtualKeyboards[i]});
			} else {
				bool prevdown = kis.macroKeyDown;
				bool prevdown2 = kis.macroKey2Down;
				kis.stale = 0;
				kis.macroKeyDown = kis.enabled && !!keystate[kis.iMacro];
				kis.macroKey2Down = kis.enabled2 && !!keystate[kis.iMacro2];
				if (kis.used) {
					kis.macroKeyRelease = false;
					kis.macroKey2Release = false;
				} else {
					kis.macroKeyRelease = prevdown && !kis.macroKeyDown;
					kis.macroKey2Release = prevdown2 && !kis.macroKey2Down;
				}
			}
		}
	}
}

static void interceptDeviceStateGamepad(DIJOYSTATE* joystate) {
	// reset state in menu
	switch (SokuLib::sceneId) {
	case SokuLib::SCENE_LOGO:
	case SokuLib::SCENE_OPENING:
	case SokuLib::SCENE_TITLE:
		virtualInputStates.clear();
		break;
	default:
		break;
	}

	size_t slot = inputCounter++;

	printf("Slot %d: %d %d\n", slot, joystate->rgbButtons[0], joystate->lZ);

	if (gVirtualInput && gTriggersEnabled && slot < 2) {
		virtualChordInput<GamepadVirtualArgs>({joystate, slot, virtualInputStates[slot], extraInput[slot]});
	}

	if (slot < 2 && !gVirtualInput) {
		ExtraInputGamepad &exin = extraInputGamepad[slot];
		bool prevl = exin.lbutton;
		bool prevr = exin.rbutton;
		exin.lbutton = false;
		exin.rbutton = false;
		exin.lbuttonRelease = false;
		exin.rbuttonRelease = false;

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
	}
}

static HRESULT WINAPI myGetDeviceState(LPVOID IDirectInputDevice8W, DWORD cbData, LPVOID lpvData) {
	uintptr_t returnaddress = reinterpret_cast<uintptr_t>(_ReturnAddress());

	HRESULT retValue = oldGetDeviceState(IDirectInputDevice8W, cbData, lpvData);

	if (retValue != DI_OK || cbData == sizeof(DIJOYSTATE2)) {
		return retValue;
	}

	if (cbData == sizeof(DIJOYSTATE)) {
		interceptDeviceStateGamepad((DIJOYSTATE *)lpvData);
	} else if (cbData == 0x100) {
		interceptDeviceStateKeyboard((char *)lpvData);
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
		gDirectionalOnly = ::GetPrivateProfileInt("ChordMacro", "DirectionalOnly", 1, s_profilePath) != 0;
		gTriggersEnabled = ::GetPrivateProfileInt("GamepadInput", "TriggerEnabled", 0, s_profilePath) != 0;
		gTriggersThreshold = ::GetPrivateProfileInt("GamepadInput", "TriggerThreshold", 200, s_profilePath);

		keyboardstate[0].enabled = ::GetPrivateProfileInt("KeyboardInput", "MacroKey", -1, s_profilePath) >= 0;
		keyboardstate[0].iMacro = ::GetPrivateProfileInt("KeyboardInput", "MacroKey", DIK_LSHIFT, s_profilePath);
		keyboardstate[0].enabled2 = ::GetPrivateProfileInt("KeyboardInput", "MacroKey2", -1, s_profilePath) >= 0;
		keyboardstate[0].iMacro2 = ::GetPrivateProfileInt("KeyboardInput", "MacroKey2", DIK_LSHIFT, s_profilePath);
		keyboardstate[1].enabled = ::GetPrivateProfileInt("KeyboardInput", "MacroKey", -1, s_profilePath) >= 0;
		keyboardstate[1].iMacro = ::GetPrivateProfileInt("KeyboardInputP2", "MacroKey", DIK_RSHIFT, s_profilePath);
		keyboardstate[1].enabled2 = ::GetPrivateProfileInt("KeyboardInputP2", "MacroKey2", -1, s_profilePath) >= 0;
		keyboardstate[1].iMacro2 = ::GetPrivateProfileInt("KeyboardInputP2", "MacroKey2", DIK_RSHIFT, s_profilePath);

		// load DirectInput library since it won't be otherwise loaded yet
    
		if (!LoadLibraryExW(L"dinput8.dll", NULL, 0)) {
			return false;
		}

		DWORD old;
		VirtualProtect((PVOID)RDATA_SECTION_OFFSET, RDATA_SECTION_SIZE, PAGE_EXECUTE_WRITECOPY, &old);
		og_BattleManagerOnProcess = SokuLib::TamperDword(&SokuLib::VTable_BattleManager.onProcess, BattleOnProcess);
		og_BattleManagerStoryOnProcess = SokuLib::TamperDword(&VTable_BattleManagerStory.onProcess, BattleOnProcessStory);
		og_BattleManagerArcadeOnProcess = SokuLib::TamperDword(&VTable_BattleManagerArcade.onProcess, BattleOnProcessArcade);
		VirtualProtect((PVOID)RDATA_SECTION_OFFSET, RDATA_SECTION_SIZE, old, &old);
   
		if (!gVirtualInput) {
			ChordInputDetectDetour();
		}
		if (gTriggersEnabled || keyboardstate[0].enabled || keyboardstate[1].enabled || keyboardstate[0].enabled2 || keyboardstate[1].enabled2) {
			DummyDirectInput();
		}

		return true;
	}
}
