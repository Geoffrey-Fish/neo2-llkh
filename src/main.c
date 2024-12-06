#define UNICODE

#include <windows.h>
#include <stdio.h>
#include <stdint.h>
#include <time.h>
#include <wchar.h>
#include <stdbool.h>
#include "trayicon.h"
#include "resources.h"
#include <io.h>

typedef struct ModState {
	bool shift, mod3;
} ModState;

HHOOK keyhook = NULL;
HANDLE hConsole;
#define APPNAME "gay Board"
#define LEN 103
#define SCANCODE_TAB_KEY 15
#define SCANCODE_CAPSLOCK_KEY 58
#define SCANCODE_LOWER_THAN_KEY 86 // <
#define SCANCODE_QUOTE_KEY 40      // Ä
#define SCANCODE_HASH_KEY 43       // #
#define SCANCODE_RETURN_KEY 28
#define SCANCODE_ANY_ALT_KEY 56        // Alt or AltGr

enum modTapModifier {
	MT_NONE,
	MT_CTRL,
	MT_SHIFT,
	MT_MOD3,
	MT_ALT,
	MT_WIN
};
#define FG_WHITE 15
#define FG_YELLOW 14
#define FG_CYAN 11
#define FG_GRAY 8

/**
 * Some global settings.
 * These values can be set in a configuration file (settings.ini)
 */
char layout[100];                    // keyboard layout by name (default: neo)
char customLayout[65];               // custom keyboard layout (32 symbols but probably more than 32 bytes)
TCHAR customLayoutWcs[33];           // custom keyboard layout in UTF-16 (32 symbols)
bool debugWindow = false;            // show debug output in a separate console window
bool quoteAsMod3R = false;           // use quote/ä as right level 3 modifier
bool returnAsMod3R = false;          // use return as right level 3 modifier
DWORD scanCodeMod3L = SCANCODE_CAPSLOCK_KEY;
DWORD scanCodeMod3R = SCANCODE_HASH_KEY;       // depends on quoteAsMod3R and returnAsMod3R
bool capsLockEnabled = false;        // enable (allow) caps lock
bool shiftLockEnabled = false;       // enable (allow) shift lock (disabled if capsLockEnabled is true)
bool qwertzForShortcuts = false;     // use QWERTZ when Ctrl, Alt or Win is involved
bool swapLeftCtrlAndLeftAlt = false; // swap left Ctrl and left Alt key
bool swapLeftCtrlLeftAltAndLeftWin = false;  // swap left Ctrl, left Alt key and left Win key. Resulting order: Win, Alt, Ctrl (on a standard Windows keyboard)
bool supportLevels5and6 = false;     // support levels five and six (greek letters and mathematical symbols)
bool capsLockAsEscape = false;       // if true, hitting CapsLock alone sends Esc
bool mod3RAsReturn = false;          // if true, hitting Mod3R alone sends Return
int modTapTimeout = 0;               // if >0, hitting a modifier alone only sends the alternative key if the press was shorter than the timeout
bool preferDeadKeyPlusSpace = false; // if true, send dead "^" (caret) followed by space instead of Unicode "^" (same for "`" (backtick))
bool capsLockAndQuoteAsShift = false; // if true, treat CapsLock and quote (QWERTZ: Ä) key as shift keys (undocumented option, might not work with other options)
/**
 * True if no mapping should be done
 */
bool bypassMode = false;
/**
 * States of some keys and shift lock.
 */
bool shiftLeftPressed = false;
bool shiftRightPressed = false;
bool shiftLockActive = false;
bool capsLockActive = false;
bool level3modLeftPressed = false;
bool level3modRightPressed = false;
bool level3modLeftAndNoOtherKeyPressed = false;
bool level3modRightAndNoOtherKeyPressed = false;

clock_t level3modLeftPressedInstant = 0;
clock_t level3modRightPressedInstant = 0;
bool ctrlLeftPressed = false;
bool ctrlRightPressed = false;
bool altLeftPressed = false;
bool winLeftPressed = false;
bool winRightPressed = false;

ModState modState = { false, false };

int mapCharacterToScanCode[256] = {0};
/**
 * Mapping tables for four levels.
 * They will be defined in initLayout().
 */
TCHAR mappingTableLevel1[LEN] = {0};
TCHAR mappingTableLevel2[LEN] = {0};
TCHAR mappingTableLevel3[LEN] = {0};
TCHAR mappingTapNextRelease[LEN] = {0};
TCHAR numpadSlashKey[7];

/**
 * When a key with TapNextRelease function is pressed, the key to emit depends
 * on the order this and succesive keys are being released. Thus all keys
 * are stored in the keyQueue in the first place.
 */
#define QUEUE_SIZE 50
KBDLLHOOKSTRUCT keyQueue[QUEUE_SIZE];
int keyQueueLength;
int keyQueueFirst;
int keyQueueLast;
int keyQueueStatus[QUEUE_SIZE]; // 0=empty/handled, 1=regular key pressed, 2=TapNextRelease key not activated, 3=TapNextRelease key activated
char *MT_MODIFIER_STRING[7] = {"", "CTRL", "SHIFT", "MOD3", "ALT", "WIN"};

typedef struct ModTap {
	int modifier;
	int keycode;
} ModTap;
#define MOD_TAP_LEN 12
ModTap modTap[MOD_TAP_LEN];
int modTapKeyCount = 0;  // how many ModTap keys are defined

bool handleSystemKey(KBDLLHOOKSTRUCT keyInfo, bool isKeyUp);
void handleShiftKey(KBDLLHOOKSTRUCT keyInfo, bool isKeyUp);
void handleMod3Key(KBDLLHOOKSTRUCT keyInfo, bool isKeyUp);
bool updateStatesAndWriteKey(KBDLLHOOKSTRUCT keyInfo, bool isKeyUp);

void convertToUTF8(TCHAR *wide, char *utf8) {
	int sizeRequired = WideCharToMultiByte(
			CP_UTF8, 0, &wide[0], -1, NULL,
			0, NULL, NULL);
	int bytesWritten = WideCharToMultiByte(
			CP_UTF8, 0, &wide[0], -1, &utf8[0],
			sizeRequired, NULL, NULL);
}

void resetKeyQueue() {
	keyQueueLength = 0;
	keyQueueFirst = 0;
	keyQueueLast = -1;
	memset(keyQueueStatus, 0, sizeof keyQueueStatus);
}

void cleanupKeyQueue() {
	printf("\nkeyQueueStatus:");
	for (int i = keyQueueFirst; i <= keyQueueLast; i++) {
		printf(" %i", keyQueueStatus[i]);
	}
	printf("\n");

	if (keyQueueFirst == 0) {
		int firstEmptyPosition = 0;
		while (keyQueueStatus[firstEmptyPosition])
			firstEmptyPosition++;
		int nextNonEmptyPosition = firstEmptyPosition;
		while (!keyQueueStatus[nextNonEmptyPosition])
			nextNonEmptyPosition++;
		int delta = nextNonEmptyPosition - firstEmptyPosition;
		SetConsoleTextAttribute(hConsole, FG_GRAY);
		printf("cleanupKeyQueue: Move all entries starting from index %i back by %i\n", nextNonEmptyPosition, delta);
		SetConsoleTextAttribute(hConsole, FG_WHITE);
		// if keyQueueFirst = 45, move all entries back by 45
		for (int i = nextNonEmptyPosition; i <= keyQueueLast; i++) {
			keyQueue[i-delta] = keyQueue[i];
			keyQueueStatus[i-delta];
			keyQueueStatus[i] = 0;
		}
		keyQueueLast -= delta;
		return;
	}
	// if keyQueueFirst = 45, move all entries back by 45
	int delta = keyQueueFirst;
	SetConsoleTextAttribute(hConsole, FG_GRAY);
	printf("cleanupKeyQueue: Move all entries back by %i\n", delta);
	SetConsoleTextAttribute(hConsole, FG_WHITE);
	for (int i = keyQueueFirst; i <= keyQueueLast; i++) {
		keyQueue[i-delta] = keyQueue[i];
		// keyQueue[i] = 0;
		keyQueueStatus[i-delta];
		keyQueueStatus[i] = 0;
	}
	keyQueueLast -= delta;
	keyQueueFirst = 0;
}

void SetStdOutToNewConsole() {
	// allocate a console for this app
	AllocConsole();
	// redirect unbuffered STDOUT to the console
	HANDLE consoleHandle = GetStdHandle(STD_OUTPUT_HANDLE);
	int fileDescriptor = _open_osfhandle((intptr_t)consoleHandle, _A_SYSTEM);
	FILE *fp = _fdopen(fileDescriptor, "w");
	*stdout = *fp;
	setvbuf(stdout, NULL, _IONBF, 0);
	// give the console window a nicer title
	SetConsoleTitle(L"Debug Output");
	// give the console window a bigger buffer size
	CONSOLE_SCREEN_BUFFER_INFO csbi;
	if (GetConsoleScreenBufferInfo(consoleHandle, &csbi)) {
		COORD bufferSize;
		bufferSize.X = csbi.dwSize.X;
		bufferSize.Y = 9999;
		SetConsoleScreenBufferSize(consoleHandle, bufferSize);
	}
}

BOOL WINAPI CtrlHandler(DWORD fdwCtrlType) {
	switch (fdwCtrlType) {
		// Handle the Ctrl-c signal.
		case CTRL_C_EVENT:
			printf("\nCtrl-c detected!\n");
			if (swapLeftCtrlAndLeftAlt || swapLeftCtrlLeftAltAndLeftWin) {
				printf("Please quit by using the tray icon!\n\n");
				return TRUE;
			} else {
				printf("Exit\n\n");
				trayicon_remove();
				return FALSE;
			}
		// Remove tray icon when terminal (debug window) is being closed
		case CTRL_CLOSE_EVENT:
			printf("Exit\n\n");
			trayicon_remove();
			return FALSE;

		default:
			return FALSE;
	}
}

/**
 * Convert UTF-8 (char) string to UTF-16 (TCHAR) string.
 */
void str2wcs(TCHAR *dest, char *src, size_t n) {
	TCHAR result[n];
	int i = 0;
	int pos = 0;

	for (int i = 0; pos < n && src[i] != 0; i++) {
		int c = src[i]>0 ? (int)src[i] : (int)src[i]+256;
		switch (c) {
			case 0xc3: continue;
			case 0xa4: result[pos] = 0xe4; break; // ä
			case 0xb6: result[pos] = 0xf6; break; // ö
			case 0xbc: result[pos] = 0xfc; break; // ü
			case 0x9f: result[pos] = 0xdf; break; // ß
			default: result[pos] = c;
		}
		pos++;
	}
	result[pos] = 0;
	wcsncpy(dest, result, pos);
}

void handleTapNextReleaseKey(int keyCode, bool isKeyUp) {
	KBDLLHOOKSTRUCT tapNextReleaseKey;
	// memset(&tapNextReleaseKey, 0, sizeof(tapNextReleaseKey));
	switch(keyCode) {
		case MT_CTRL:
			// simulate ctrl key pressed or released
			tapNextReleaseKey.vkCode = VK_LCONTROL;
			tapNextReleaseKey.scanCode = 29;
			tapNextReleaseKey.flags = 0;
			handleSystemKey(tapNextReleaseKey, isKeyUp);
			break;
		case MT_SHIFT:
			// simulate shift key pressed or released
			tapNextReleaseKey.vkCode = VK_SHIFT;
			handleShiftKey(tapNextReleaseKey, isKeyUp);
			break;
		case MT_MOD3:
			// simulate mod3Key pressed or released
			tapNextReleaseKey.scanCode = scanCodeMod3L;
			handleMod3Key(tapNextReleaseKey, isKeyUp);
			handleMod3Key(tapNextReleaseKey, isKeyUp);
			break;
		case MT_ALT:
			// simulate alt key pressed or released
			tapNextReleaseKey.vkCode = VK_LMENU;
			/* tapNextReleaseKey.scanCode = 29; */
			tapNextReleaseKey.flags = 0;
			handleSystemKey(tapNextReleaseKey, isKeyUp);
			break;
		case MT_WIN:
			// simulate windows key pressed or released
			tapNextReleaseKey.vkCode = VK_LWIN;
			/* tapNextReleaseKey.scanCode = 29; */
			tapNextReleaseKey.flags = 0;
			handleSystemKey(tapNextReleaseKey, isKeyUp);
			break;
	}
}

void appendToQueue(KBDLLHOOKSTRUCT keyInfo) {
	// only keyDown events
	if (keyQueueLength && keyInfo.scanCode == keyQueue[keyQueueLast].scanCode)
		return;
	if (keyQueueLast >= QUEUE_SIZE - 1)
		cleanupKeyQueue();
	keyQueueLast++;
	keyQueueLength++;
	int character = MapVirtualKeyA(keyInfo.vkCode, MAPVK_VK_TO_CHAR);
	int tapNextRelease = mappingTapNextRelease[keyInfo.scanCode];
	SetConsoleTextAttribute(hConsole, FG_GRAY);
	printf("Append key '%c%s%s' to queue at index %i ", character, tapNextRelease ? "|" : "", MT_MODIFIER_STRING[tapNextRelease], keyQueueLast);
	if (keyQueueLength == 1)
		printf("(1 key is pressed)\n");
	else if (keyQueueLength > 1)
		printf("(%i keys are pressed)\n", keyQueueLength);
	else
		printf("\n");
	SetConsoleTextAttribute(hConsole, FG_WHITE);
	keyQueue[keyQueueLast] = keyInfo;
	keyQueueStatus[keyQueueLast] = tapNextRelease ? 2 : 1;
}
// returns true, key release has been handled
bool checkQueue(KBDLLHOOKSTRUCT keyInfo) {
	// only keyUp events
	// definiton: tap = press + release
	bool keyFoundInQueue = false;
	int i = keyQueueFirst;
	while (i <= keyQueueLast) {
		// find released key in queue
		if (keyQueueStatus[i] > 0 && keyInfo.scanCode == keyQueue[i].scanCode) {
			keyFoundInQueue = true;
			// printf("Key released is at index %i in queue.\n", i);
			// no matter what type of key it is:
			// check if keys in the queue pressed earlier are unactivated tap-next-release keys
			for (int j=keyQueueFirst; j<i; j++) {
				if (keyQueueStatus[j] == 2) {
					// send key down for tap-next-release function of this key
					handleTapNextReleaseKey(mappingTapNextRelease[keyQueue[j].scanCode], false);
					// update status (mark as activated)
					keyQueueStatus[j] = 3;
				}
			}
			// depending on key type
			if (keyQueueStatus[i] <= 2) {
				// regular key (no tap-next-release function) or
				// tap-next-release key which has not been activated
				updateStatesAndWriteKey(keyQueue[i], false); // key down
				// release key
				keyQueue[i].flags += 0x80;
				updateStatesAndWriteKey(keyQueue[i], true); // key up
			} else {
				// tap-next-release key which was activated
				// send key up for alternative mapping
				handleTapNextReleaseKey(mappingTapNextRelease[keyQueue[i].scanCode], true);
			}
			// set status to 0 (=handled)
			keyQueueStatus[i] = 0;
			int character = MapVirtualKeyA(keyInfo.vkCode, MAPVK_VK_TO_CHAR);
			int tapNextRelease = mappingTapNextRelease[keyInfo.scanCode];
			SetConsoleTextAttribute(hConsole, FG_GRAY);
			printf("Remove key '%c%s%s' from queue at index %i ", character, tapNextRelease ? "|" : "", MT_MODIFIER_STRING[tapNextRelease], i);
			if (keyQueueLength-1 == 1)
				printf("(1 key is pressed)\n");
			else if (keyQueueLength-1 > 1)
				printf("(%i keys are pressed)\n", keyQueueLength);
			else
				printf("\n");
			SetConsoleTextAttribute(hConsole, FG_WHITE);
			// if beginning of queue, move it to next tap-next-release key
			if (i == keyQueueFirst) {
				// queue always begins with tap-next-release keys
				// for (int j=i+1; j<keyQueueLast; j++) {
				int j = i + 1;
				while (j <= keyQueueLast) {
					if (keyQueueStatus[j] >= 2) {
						// make this position the beginning of the queue
						keyQueueFirst = j;
						keyQueueLength--;
						break;
					} else if (keyQueueStatus[j] == 1) {
						// press this key (key down was held back, now it does not depend of other key states anymore)
						updateStatesAndWriteKey(keyQueue[j], false); // key down
						keyQueueLength--;
					}
					j++;
				}
				if (j > keyQueueLast)
					resetKeyQueue();
			} else if (i == keyQueueLast) {
				int j = i - 1;
				while (j >= keyQueueFirst) {
					if (keyQueueStatus[j] > 0) {
						keyQueueLength--;
						keyQueueLast = j;
						break;
					}
					j--;
				}
				if (j < keyQueueFirst)
					resetKeyQueue();
			} else {
				// key released was neither first nor last in queue
				keyQueueLength--;
			}
			break;
		}
		i++;
	}
	return keyFoundInQueue;
}
void mapLevels_2_5_6(TCHAR * mappingTableOutput, TCHAR * newChars) {
	TCHAR * l1_lowercase = L"abcdefghijklmnopqrstuvwxyzäöüß.,";
	TCHAR *ptr;
	for (int i = 0; i < LEN; i++) {
		ptr = wcschr(l1_lowercase, mappingTableLevel1[i]);
		if (ptr != NULL && ptr < &l1_lowercase[32]) {
			mappingTableOutput[i] = newChars[ptr-l1_lowercase];
		}
	}
}
void initCharacterToScanCodeMap() {
	mapCharacterToScanCode['q'] = 0x10;
	mapCharacterToScanCode['w'] = 0x11;
	mapCharacterToScanCode['e'] = 0x12;
	mapCharacterToScanCode['r'] = 0x13;
	mapCharacterToScanCode['t'] = 0x14;
	mapCharacterToScanCode['z'] = 0x15;
	mapCharacterToScanCode['u'] = 0x16;
	mapCharacterToScanCode['i'] = 0x17;
	mapCharacterToScanCode['o'] = 0x18;
	mapCharacterToScanCode['p'] = 0x19;
	mapCharacterToScanCode[0xfc] = 0x1a; // ü
	mapCharacterToScanCode['+'] = 0x1b;
	mapCharacterToScanCode['a'] = 0x1e;
	mapCharacterToScanCode['s'] = 0x1f;
	mapCharacterToScanCode['d'] = 0x20;
	mapCharacterToScanCode['f'] = 0x21;
	mapCharacterToScanCode['g'] = 0x22;
	mapCharacterToScanCode['h'] = 0x23;
	mapCharacterToScanCode['j'] = 0x24;
	mapCharacterToScanCode['k'] = 0x25;
	mapCharacterToScanCode['l'] = 0x26;
	mapCharacterToScanCode[0xf6] = 0x27; // ö
	mapCharacterToScanCode[0xe4] = 0x28; // ä
	mapCharacterToScanCode['y'] = 0x2c;
	mapCharacterToScanCode['x'] = 0x2d;
	mapCharacterToScanCode['c'] = 0x2e;
	mapCharacterToScanCode['v'] = 0x2f;
	mapCharacterToScanCode['b'] = 0x30;
	mapCharacterToScanCode['n'] = 0x31;
	mapCharacterToScanCode['m'] = 0x32;
	mapCharacterToScanCode[','] = 0x33;
	mapCharacterToScanCode['.'] = 0x34;
	mapCharacterToScanCode['-'] = 0x35;
}

void initLayout() {
	// same for all layouts
	wcscpy(mappingTableLevel1 +  2, L"1234567890-`");
	wcscpy(mappingTableLevel1 + 71, L"789-456+1230.");
	mappingTableLevel1[57] = L' '; // Spacebar → space
    mappingTableLevel1[SCANCODE_CAPSLOCK_KEY] = L'\t'; // Tab
	mappingTableLevel1[SCANCODE_CAPSLOCK_KEY] = L'?';
    mappingTableLevel1[SCANCODE_QUOTE_KEY] = L'!'; // 'ä' → '!'

	mappingTableLevel2[41] = L'\u030C'; // key to the left of the "1" key, "Combining Caron"
	wcscpy(mappingTableLevel2 +  2, L"°§ℓ»«$€„“”—̧");
	wcscpy(mappingTableLevel2 + 71, L"✔✘†-♣€‣+♦♥♠␣."); // numeric keypad
	mappingTableLevel2[57] = L' '; // Spacebar → space
	mappingTableLevel2[69] = L'\t'; // NumLock key → tabulator

	wcscpy(mappingTableLevel3 +  2, L"¹²³›‹¢¥‚‘’—̊");
	wcscpy(mappingTableLevel3 + 16, L"@|€{}<*789%");
	wcscpy(mappingTableLevel3 + 30, L"\\/()>-456=&");
	wcscpy(mappingTableLevel3 + 41, L"^");
	wcscpy(mappingTableLevel3 + 44, L"#~[]$_1230");
	wcscpy(mappingTableLevel3 + 71, L"↕↑↨−←:→±↔↓⇌%,"); // numeric keypad
	mappingTableLevel3[27] = L'\u0337'; // "Combining Short Solidus Overlay"
	mappingTableLevel3[55] = L'⋅'; // *-key on numeric keypad
	mappingTableLevel3[57] = L' '; // Spacebar → space
	mappingTableLevel3[69] = L'='; // num-lock-key
	// layout dependent
	if ( strcmp(layout, "gay")== 0) {
		wcscpy(mappingTableLevel1 + 16, L"jäouk.clvxß");
		wcscpy(mappingTableLevel1 + 30, L"haeigdtnrs");
		wcscpy(mappingTableLevel1 + 44, L"zy,öübpmwf");
	} else if (strcmp(layout, "qwertz") == 0) {
		wcscpy(mappingTableLevel1 + 12, L"ß");
		wcscpy(mappingTableLevel1 + 16, L"qwertzuiopü+");
		wcscpy(mappingTableLevel1 + 30, L"asdfghjklöä");
		wcscpy(mappingTableLevel1 + 44, L"yxcvbnm,.-");
	} 
	// use custom layout if it was defined
	if (wcslen(customLayoutWcs) != 0) {
		if (wcslen(customLayoutWcs) == 32) {
			// custom layout
			wcsncpy(mappingTableLevel1 + 16, customLayoutWcs, 11);
			wcsncpy(mappingTableLevel1 + 30, customLayoutWcs + 11, 11);
			wcsncpy(mappingTableLevel1 + 44, customLayoutWcs + 22, 10);
		} else {
			printf("\ncustomLayout given but its length is %i (expected: 32).\n", wcslen(customLayoutWcs));
		}
	}

	// same for all layouts
	wcscpy(mappingTableLevel1 + 27, L"´");
	wcscpy(mappingTableLevel2 + 27, L"~");
	// slash key is special: it has the same scan code in the main block and the numpad
	wcscpy(numpadSlashKey, L"//÷∕⌀∣");

	// map letters of level 2
	TCHAR * charsLevel2;
	charsLevel2 = L"ABCDEFGHIJKLMNOPQRSTUVWXYZÄÖÜẞ•–";
	mapLevels_2_5_6(mappingTableLevel2, charsLevel2);

	// if quote/ä is the right level 3 modifier, copy symbol of quote/ä key to backslash/# key
	if (quoteAsMod3R) {
		mappingTableLevel1[43] = mappingTableLevel1[40];
		mappingTableLevel2[43] = mappingTableLevel2[40];
		mappingTableLevel3[43] = mappingTableLevel3[40];
	}
	mappingTableLevel2[8] = 0x20AC;  // €

	// apply modTap modifiers
	// puts("\nModTap keys:");
	for (int i=0; i<MOD_TAP_LEN && modTap[i].modifier; i++) {
		unsigned int scanCode = mapCharacterToScanCode[(unsigned char)modTap[i].keycode];
		mappingTapNextRelease[scanCode] = modTap[i].modifier;
		// printf("%s (%i), %c (%i), sc=0x%X (%i)\n", MT_MODIFIER_STRING[modTap[i].modifier], modTap[i].modifier, modTap[i].keycode, (unsigned char)modTap[i].keycode, scanCode, scanCode);
    }
}

void toggleBypassMode() {
	bypassMode = !bypassMode;
	HINSTANCE hInstance = GetModuleHandle(NULL);
	HICON icon = bypassMode
		? LoadIcon(hInstance, MAKEINTRESOURCE(IDI_APPICON_DISABLED))
		: LoadIcon(hInstance, MAKEINTRESOURCE(IDI_APPICON));
	trayicon_change_icon(icon);
	printf("%i bypass mode \n", bypassMode);
}

/**
 * Map a key scancode to the char that should be displayed after typing
 **/
TCHAR mapScanCodeToChar(unsigned level, char in) {
	switch (level) {
		case 2:
			return mappingTableLevel2[in];
		case 3:
			return mappingTableLevel3[in];
		default: // level 1
			return mappingTableLevel1[in];
	}
}

DWORD dwFlagsFromKeyInfo(KBDLLHOOKSTRUCT keyInfo) {
	DWORD dwFlags = 0;
	if (keyInfo.flags & LLKHF_EXTENDED) dwFlags |= KEYEVENTF_EXTENDEDKEY;
	if (keyInfo.flags & LLKHF_UP) dwFlags |= KEYEVENTF_KEYUP;
	return dwFlags;
}

void sendDown(BYTE vkCode, BYTE scanCode, bool isExtendedKey) {
	keybd_event(vkCode, scanCode, (isExtendedKey ? KEYEVENTF_EXTENDEDKEY : 0), 0);
}

void sendUp(BYTE vkCode, BYTE scanCode, bool isExtendedKey) {
	keybd_event(vkCode, scanCode, (isExtendedKey ? KEYEVENTF_EXTENDEDKEY : 0) | KEYEVENTF_KEYUP, 0);
}

void sendDownUp(BYTE vkCode, BYTE scanCode, bool isExtendedKey) {
	sendDown(vkCode, scanCode, isExtendedKey);
	sendUp(vkCode, scanCode, isExtendedKey);
}

void sendUnicodeChar(TCHAR key, KBDLLHOOKSTRUCT keyInfo) {
	KEYBDINPUT kb={0};
	INPUT Input={0};

	kb.wScan = key;
	kb.dwFlags = KEYEVENTF_UNICODE | dwFlagsFromKeyInfo(keyInfo);
	Input.type = INPUT_KEYBOARD;
	Input.ki = kb;
	SendInput(1, &Input, sizeof(Input));
}

/**
 * Sends a char using emulated keyboard input
 * This works for most cases, but not for dead keys etc
 **/
void sendChar(TCHAR key, KBDLLHOOKSTRUCT keyInfo) {
	SHORT keyScanResult = VkKeyScanEx(key, GetKeyboardLayout(0));

	if (keyScanResult == -1 || shiftLockActive || capsLockActive
		|| (keyInfo.vkCode >= 0x30 && keyInfo.vkCode <= 0x39)) {
		sendUnicodeChar(key, keyInfo);
	} else {
		keyInfo.vkCode = keyScanResult & 0xff;
		char modifiers = keyScanResult >> 8;
		bool shift = ((modifiers & 1) != 0);
		bool alt = ((modifiers & 2) != 0);
		bool ctrl = ((modifiers & 3) != 0);
		bool altgr = alt && ctrl;
		if (altgr) {
			ctrl = false;
			alt = false;
		}

		if (altgr) sendDown(VK_RMENU, 56, true);
		if (ctrl) sendDown(VK_CONTROL, 29, false);
		if (alt) sendDown(VK_MENU, 56, false); // ALT
		if (shift) sendDown(VK_SHIFT, 42, false);

		keybd_event(keyInfo.vkCode, keyInfo.scanCode, dwFlagsFromKeyInfo(keyInfo), keyInfo.dwExtraInfo);

		if (altgr) sendUp(VK_RMENU, 56, true);
		if (alt) sendUp(VK_MENU, 56, false); // ALT
		if (ctrl) sendUp(VK_CONTROL, 29, false);
		if (shift) sendUp(VK_SHIFT, 42, false);
	}
}

/**
 * Send a usually dead key by injecting space after (on down).
 * This will add an actual space if actual dead key is followed by "dead" key with this
 **/
void commitDeadKey(KBDLLHOOKSTRUCT keyInfo) {
	if (!(keyInfo.flags & LLKHF_UP)) sendDownUp(VK_SPACE, 57, false);
}

bool handleLayer2SpecialCases(KBDLLHOOKSTRUCT keyInfo) {
	switch(keyInfo.scanCode) {
		case 27:
			sendChar(L'\u0303', keyInfo);  // perispomene (Tilde)
			return true;
		case 41:
			sendChar(L'\u030C', keyInfo);  // caron, wedge, háček (Hatschek)
			return true;
		default:
			return false;
	}
}

bool handleLayer3SpecialCases(KBDLLHOOKSTRUCT keyInfo) {
	switch(keyInfo.scanCode) {
		case 13:
			sendChar(L'\u030A', keyInfo);  // overring
			return true;
		case 20:
			if (preferDeadKeyPlusSpace) {
				sendChar(L'^', keyInfo);
				sendChar(L' ', keyInfo);
			} else {
				sendUnicodeChar(L'^', keyInfo);
			}
			return true;
		case 27:
			sendChar(L'\u0337', keyInfo);  // bar (diakritischer Schrägstrich)
			return true;
		default:
			return false;
	}
}

bool isShift(KBDLLHOOKSTRUCT keyInfo) {
	return keyInfo.vkCode == VK_SHIFT
	    || keyInfo.vkCode == VK_LSHIFT
	    || keyInfo.vkCode == VK_RSHIFT;
}

bool isMod3(KBDLLHOOKSTRUCT keyInfo) {
	return keyInfo.scanCode == scanCodeMod3L
	    || keyInfo.scanCode == scanCodeMod3R;
}

bool isSystemKeyPressed() {
	return ctrlLeftPressed || ctrlRightPressed
	    || altLeftPressed
	    || winLeftPressed || winRightPressed;
}

bool isLetter(TCHAR key) {
	return (key >= 65 && key <= 90  // A-Z
	     || key >= 97 && key <= 122 // a-z
	     || key == L'ä' || key == L'Ä'
	     || key == L'ö' || key == L'Ö'
	     || key == L'ü' || key == L'Ü'
	     || key == L'ß' || key == L'ẞ');
}

void toggleShiftLock() {
	shiftLockActive = !shiftLockActive;
	printf("Shift lock %s!\n", shiftLockActive ? "activated" : "deactivated");
}

void toggleCapsLock() {
	capsLockActive = !capsLockActive;
	printf("Caps lock %s!\n", capsLockActive ? "activated" : "deactivated");
}

void logKeyEvent(char *desc, KBDLLHOOKSTRUCT keyInfo, int color) {
	char vkCodeLetter[4] = {'(', keyInfo.vkCode, ')', 0};
	char *keyName;
	switch (keyInfo.vkCode) {
		case VK_LSHIFT:
			keyName = "(Shift left)";
			break;
		case VK_RSHIFT:
			keyName = "(Shift right)";
			break;
		case VK_SHIFT:
			keyName = "(Shift)";
			break;
		case VK_CAPITAL:
			keyName = "(M3 left)";
			break;
		case 0xde:  // ä
			keyName = quoteAsMod3R ? "(M3 right)" : "";
			break;
		case 0xbf:  // #
			keyName = quoteAsMod3R ? "" : "(M3 right)";
			break;
		case VK_OEM_102:
			keyName = "(M4 left [<])";
			break;
		case VK_CONTROL:
			keyName = "(Ctrl)";
			break;
		case VK_LCONTROL:
			keyName = "(Ctrl left)";
			break;
		case VK_RCONTROL:
			keyName = "(Ctrl right)";
			break;
		case VK_MENU:
			keyName = "(Alt)";
			break;
		case VK_LMENU:
			keyName = "(Alt left)";
			break;
		case VK_RMENU:
			keyName = "(Alt right)";
			break;
		case VK_LWIN:
			keyName = "(Win left)";
			break;
		case VK_RWIN:
			keyName = "(Win right)";
			break;
		case VK_BACK:
			keyName = "(Backspace)";
			break;
		case VK_RETURN:
			keyName = "(Return)";
			break;
		case VK_SPACE:
			keyName = "(Spacebar)";
			break;
		case 0x41 ... 0x5A:
			keyName = vkCodeLetter;
			break;
		default:
			keyName = "";
			//keyName = MapVirtualKeyA(keyInfo.vkCode, MAPVK_VK_TO_CHAR);
	}
	char *shiftLockCapsLockInfo = shiftLockActive ? " [shift lock active]"
						: (capsLockActive ? " [caps lock active]" : "");
	char *vkPacket = (desc=="injected" && keyInfo.vkCode == VK_PACKET) ? " (VK_PACKET)" : "";

	if (color < 0)
		color = FG_WHITE;
	SetConsoleTextAttribute(hConsole, color);
	printf(
		"%-13s | sc:%03u vk:0x%02X flags:0x%02X extra:%d %s%s%s%s\n",
		desc, keyInfo.scanCode, keyInfo.vkCode, keyInfo.flags, keyInfo.dwExtraInfo,
		keyName, shiftLockCapsLockInfo, vkPacket
	);
	// reset color
	SetConsoleTextAttribute(hConsole, FG_WHITE);
}

unsigned getLevel() {
	unsigned level = 1;

	if (modState.shift != shiftLockActive) // (modState.shift) XOR (shiftLockActive)
		level = 2;
	if (modState.mod3)
		level =  3;
	return level;
}

void handleShiftKey(KBDLLHOOKSTRUCT keyInfo, bool isKeyUp) {
	bool *pressedShift = keyInfo.vkCode == VK_RSHIFT ? &shiftRightPressed : &shiftLeftPressed;
	bool *otherShift = keyInfo.vkCode == VK_RSHIFT ? &shiftLeftPressed : &shiftRightPressed;

	modState.shift = !isKeyUp;
	*pressedShift = !isKeyUp;

	if (isKeyUp) {
		if (*otherShift && !bypassMode) {
			if (shiftLockEnabled) {
				sendDownUp(VK_CAPITAL, 58, false);
				toggleShiftLock();
			} else if (capsLockEnabled) {
				sendDownUp(VK_CAPITAL, 58, false);
				toggleCapsLock();
			}
		}
		sendUp(keyInfo.vkCode, keyInfo.scanCode, false);
	} else { // key down
		sendDown(keyInfo.vkCode, keyInfo.scanCode, false);
	}
}

/**
 * returns `true` if no systemKey was pressed -> continue execution, `false` otherwise
 **/
bool handleSystemKey(KBDLLHOOKSTRUCT keyInfo, bool isKeyUp) {
	bool newStateValue = !isKeyUp;
	DWORD dwFlags = isKeyUp ? (keyInfo.flags | KEYEVENTF_KEYUP) : keyInfo.flags;

	// Check also the scan code because AltGr sends VK_LCONTROL with scanCode 541
	if (keyInfo.vkCode == VK_LCONTROL && keyInfo.scanCode == 29) {
		if (swapLeftCtrlAndLeftAlt) {
			altLeftPressed = newStateValue;
			keybd_event(VK_LMENU, 56, dwFlags, 0);
		} else if (swapLeftCtrlLeftAltAndLeftWin) {
			winLeftPressed = newStateValue;
			keybd_event(VK_LWIN, 91, dwFlags | LLKHF_EXTENDED, 0);
		} else {
			ctrlLeftPressed = newStateValue;
			keybd_event(VK_LCONTROL, 29, dwFlags, 0);
		}
		return false;
	} else if (keyInfo.vkCode == VK_RCONTROL) {
		ctrlRightPressed = newStateValue;
		keybd_event(VK_RCONTROL, 29, dwFlags | LLKHF_EXTENDED, 0);
	} else if (keyInfo.vkCode == VK_LMENU) {
		if (swapLeftCtrlAndLeftAlt || swapLeftCtrlLeftAltAndLeftWin) {
			ctrlLeftPressed = newStateValue;
			keybd_event(VK_LCONTROL, 29, dwFlags, 0);
		} else {
			altLeftPressed = newStateValue;
			keybd_event(VK_LMENU, 56, dwFlags, 0);
		}
		return false;
	} else if (keyInfo.vkCode == VK_LWIN) {
		if (swapLeftCtrlLeftAltAndLeftWin) {
			altLeftPressed = newStateValue;
			keybd_event(VK_LMENU, 56, dwFlags & ~LLKHF_EXTENDED, 0);
		} else {
			winLeftPressed = newStateValue;
			keybd_event(VK_LWIN, 91, dwFlags, 0);
		}
		return false;
	} else if (keyInfo.vkCode == VK_RWIN) {
		winRightPressed = newStateValue;
		keybd_event(VK_RWIN, 92, dwFlags, 0);
		return false;
	}

	return true;
}

bool wasTap(clock_t pressInstant, clock_t releaseInstant) {
	double pressSeconds = (double) (releaseInstant - pressInstant) / CLOCKS_PER_SEC;
	return modTapTimeout == 0 || 1000 * pressSeconds < modTapTimeout;
}

void handleMod3Key(KBDLLHOOKSTRUCT keyInfo, bool isKeyUp) {
	if (isKeyUp) {
		clock_t releaseInstant = clock();

		if (keyInfo.scanCode == scanCodeMod3R) {
			level3modRightPressed = false;
			modState.mod3 = level3modLeftPressed | level3modRightPressed;
			if (mod3RAsReturn && level3modRightAndNoOtherKeyPressed) {
				sendUp(keyInfo.vkCode, keyInfo.scanCode, false); // release Mod3_R
				level3modRightAndNoOtherKeyPressed = false;
				if (wasTap(level3modRightPressedInstant, releaseInstant)) {
					sendDownUp(VK_RETURN, 28, true); // send Return
				}
			}
		} else { // scanCodeMod3L (CapsLock)
			level3modLeftPressed = false;
			modState.mod3 = level3modLeftPressed | level3modRightPressed;
			if (capsLockAsEscape && level3modLeftAndNoOtherKeyPressed) {
				sendUp(VK_CAPITAL, 58, false); // release Mod3_R
				level3modLeftAndNoOtherKeyPressed = false;
				if (wasTap(level3modLeftPressedInstant, releaseInstant)) {
					sendDownUp(VK_ESCAPE, 1, true); // send Escape
				}
			}
		}
	} else { // keyDown
		if (keyInfo.scanCode == scanCodeMod3R) {
			if (!level3modRightPressed) {
				level3modRightPressed = true;
				level3modRightPressedInstant = clock();
			}
			if (mod3RAsReturn)
				level3modRightAndNoOtherKeyPressed = true;
		} else { // VK_CAPITAL (CapsLock)
			if (!level3modLeftPressed) {
				level3modLeftPressed = true;
				level3modLeftPressedInstant = clock();
			}
			if (capsLockAsEscape)
				level3modLeftAndNoOtherKeyPressed = true;
		}
		modState.mod3 = level3modLeftPressed | level3modRightPressed;
	}
}

/**
 * updates system key and layerLock states; writes key
 * returns `true` if next hook should be called, `false` otherwise
 **/
bool updateStatesAndWriteKey(KBDLLHOOKSTRUCT keyInfo, bool isKeyUp) {
	bool continueExecution = handleSystemKey(keyInfo, isKeyUp);
	if (!continueExecution) return false;

	unsigned level = getLevel();
    // Handle Caps Lock remapping to Tab
    if (keyInfo.scanCode == SCANCODE_CAPSLOCK_KEY) {
        sendChar(L'\t', keyInfo); // Send Tab for Caps Lock
        return false;
    }

    // Handle 'ä' remapping to '!'
    if (keyInfo.scanCode == SCANCODE_QUOTE_KEY) {
        sendChar(L'!', keyInfo); // Send '!' for 'ä' key
        return false;
    }
	if (isMod3(keyInfo)) {
		// if (keyQueueLength)
		// 	return false;
		handleMod3Key(keyInfo, isKeyUp);
		return false;
	} else if ((keyInfo.flags & LLKHF_EXTENDED) && keyInfo.scanCode != 53) {
		// handle numpad slash key (scanCode=53 + extended bit) later
		return true;
	} else if (level == 2 && handleLayer2SpecialCases(keyInfo)) {
		return false;
	} else if (level == 3 && handleLayer3SpecialCases(keyInfo)) {
		return false;
	} else if (level == 1 && keyInfo.vkCode >= 0x30 && keyInfo.vkCode <= 0x39) {
		// numbers 0 to 9 -> don't remap
	} else if (!(qwertzForShortcuts && isSystemKeyPressed())) {
		TCHAR key;
		if ((keyInfo.flags & LLKHF_EXTENDED) && keyInfo.scanCode == 53) {
			// slash key ("/") on numpad
			key = numpadSlashKey[level-1];
			keyInfo.flags = 0;
		} else {
			key = mapScanCodeToChar(level, keyInfo.scanCode);
		}
		if (capsLockActive && (level == 1 || level == 2) && isLetter(key)) {
			key = mapScanCodeToChar(level==1 ? 2 : 1, keyInfo.scanCode);
		}
		if (key != 0 && (keyInfo.flags & LLKHF_INJECTED) == 0) {
			// if key must be mapped
			int character = MapVirtualKeyA(keyInfo.vkCode, MAPVK_VK_TO_CHAR);
			TCHAR keyUTF16[] = {key, 0};
			char keyUTF8[4];
			convertToUTF8(keyUTF16, keyUTF8);
			printf("%-13s | sc:%03d %c->%s [0x%04X] (level %u)\n", " mapped", keyInfo.scanCode, character, keyUTF8, key, level);
			sendChar(key, keyInfo);
			return false;
		}
	}
	return true;
}

__declspec(dllexport)
LRESULT CALLBACK keyevent(int code, WPARAM wparam, LPARAM lparam) {

	if (code != HC_ACTION ||
			!(wparam == WM_SYSKEYUP || wparam == WM_KEYUP ||
			  wparam == WM_SYSKEYDOWN || wparam == WM_KEYDOWN)) {
		return CallNextHookEx(NULL, code, wparam, lparam);
	}

	KBDLLHOOKSTRUCT keyInfo = *((KBDLLHOOKSTRUCT *) lparam);

	if (keyInfo.flags & LLKHF_INJECTED) {
		// process injected events like normal, because most probably we are injecting them
		logKeyEvent((keyInfo.flags & LLKHF_UP) ? "injected up" : "injected down", keyInfo, FG_YELLOW);
		return CallNextHookEx(NULL, code, wparam, lparam);
	}

	bool isKeyUp = (wparam == WM_KEYUP || wparam == WM_SYSKEYUP);

	if (isShift(keyInfo)) {
		// if (keyQueueLength)
		// 	return false;
		handleShiftKey(keyInfo, isKeyUp);
		return -1;
	}

	// Shift + Pause
	if (wparam == WM_KEYDOWN && keyInfo.vkCode == VK_PAUSE && modState.shift) {
		toggleBypassMode();
		return -1;
	}

	if (bypassMode) {
		if (keyInfo.vkCode == VK_CAPITAL && !(keyInfo.flags & LLKHF_UP)) {
			// synchronize with capsLock state during bypass
			if (shiftLockEnabled) {
				toggleShiftLock();
			} else if (capsLockEnabled) {
				toggleCapsLock();
			}
		}
		return CallNextHookEx(NULL, code, wparam, lparam);
	}

	// treat CapsLock and Ä as shift keys
	if (capsLockAndQuoteAsShift) {
		if (keyInfo.vkCode == VK_CAPITAL) {
			if (isKeyUp)
				sendUp(VK_LSHIFT, 0x2a, false);
			else
				sendDown(VK_LSHIFT, 0x2a, false);
			return -1;
		} else if (keyInfo.vkCode == VK_OEM_7) {
			if (isKeyUp)
				sendUp(VK_RSHIFT, 0x36, false);
			else
				sendDown(VK_RSHIFT, 0x36, false);
			return -1;
		}
	}

	if (isKeyUp) {
		logKeyEvent("key up", keyInfo, FG_CYAN);

		if (keyQueueLength) {
			// int index;
			bool keyReleasedHandled = checkQueue(keyInfo);
			if (keyReleasedHandled)
				return -1;
				// return CallNextHookEx(NULL, code, wparam, lparam);
		}
		bool callNext = updateStatesAndWriteKey(keyInfo, true);
		if (!callNext) return -1;

	} else {  // key down
		unsigned level = getLevel();
		printf("\nLEVEL %i", level);
		printf("\n");

		logKeyEvent("key down", keyInfo, FG_CYAN);

		if (keyQueueLength || mappingTapNextRelease[keyInfo.scanCode]) {
			appendToQueue(keyInfo);
			return -1;
		}

		level3modLeftAndNoOtherKeyPressed = false;
		level3modRightAndNoOtherKeyPressed = false;

		bool callNext = updateStatesAndWriteKey(keyInfo, false);
		if (!callNext) return -1;
	}

	return CallNextHookEx(NULL, code, wparam, lparam);
}

DWORD WINAPI hookThreadMain(void *user) {
	HINSTANCE base = GetModuleHandle(NULL);
	MSG msg;

	if (!base) {
		if (!(base = LoadLibrary((wchar_t *) user))) {
			return 1;
		}
	}
	keyhook = SetWindowsHookEx(WH_KEYBOARD_LL, keyevent, base, 0);
	while (GetMessage(&msg, 0, 0, 0) > 0) {

		DispatchMessage(&msg);
	}
	UnhookWindowsHookEx(keyhook);

	return 0;
}

void exitApplication() {
	printf("Clicked Exit button!\n");
	trayicon_remove();
	PostQuitMessage(0);
}

bool fileExists(LPCSTR szPath) {
	DWORD dwAttrib = GetFileAttributesA(szPath);

	return (dwAttrib != INVALID_FILE_ATTRIBUTES
	    && !(dwAttrib & FILE_ATTRIBUTE_DIRECTORY));
}

bool checkSetting(char *keyword, char *filename) {
	char returnValue[100];
	GetPrivateProfileStringA("Settings", keyword, "0", returnValue, 100, filename);
	return (strcmp(returnValue, "1") == 0);
}

int getSettingInt(char *keyword, char *filename) {
	char returnValue[100];
	GetPrivateProfileStringA("Settings", keyword, "0", returnValue, 100, filename);
	return atoi(returnValue);
}

int main(int argc, char *argv[]) {
	setbuf(stdout, NULL);

	char ini[256];
	GetModuleFileNameA(NULL, ini, 256);

	char * pch;
	// find last \ in path
	pch = strrchr(ini, '\\');

	strcpy(pch+1, "settings.ini");
	if (fileExists(ini)) {
		char returnValue[100];

		GetPrivateProfileStringA("Settings", "layout", "gay", layout, 100, ini);

		GetPrivateProfileStringA("Settings", "customLayout", "", customLayout, 65, ini);

		quoteAsMod3R = checkSetting("symmetricalLevel3Modifiers", ini);
		returnAsMod3R = checkSetting("returnKeyAsMod3R", ini);
		capsLockEnabled = checkSetting("capsLockEnabled", ini);
		shiftLockEnabled = checkSetting("shiftLockEnabled", ini);
		qwertzForShortcuts = checkSetting("qwertzForShortcuts", ini);
		swapLeftCtrlAndLeftAlt = checkSetting("swapLeftCtrlAndLeftAlt", ini);
		swapLeftCtrlLeftAltAndLeftWin = checkSetting("swapLeftCtrlLeftAltAndLeftWin", ini);
		capsLockAsEscape = checkSetting("capsLockAsEscape", ini);
		mod3RAsReturn = checkSetting("mod3RAsReturn", ini);
		modTapTimeout = getSettingInt("modTapTimeout", ini);
		preferDeadKeyPlusSpace = checkSetting("preferDeadKeyPlusSpace", ini);
		capsLockAndQuoteAsShift = checkSetting("capsLockAndQuoteAsShift", ini);
		debugWindow = checkSetting("debugWindow", ini);

		if (capsLockEnabled)
			shiftLockEnabled = false;

		if (swapLeftCtrlLeftAltAndLeftWin)
			swapLeftCtrlAndLeftAlt = false;

		if (debugWindow) {
			// Open Console Window to see printf output
			SetStdOutToNewConsole();
		}

		printf("\nEinstellungen aus %s:\n", ini);
		printf(" Layout: %s\n", layout);
		printf(" customLayout: %s\n", customLayout);
		printf(" symmetricalLevel3Modifiers: %d\n", quoteAsMod3R);
		printf(" returnKeyAsMod3R: %d\n", returnAsMod3R);
		printf(" capsLockEnabled: %d\n", capsLockEnabled);
		printf(" shiftLockEnabled: %d\n", shiftLockEnabled);
		printf(" qwertzForShortcuts: %d\n", qwertzForShortcuts);
		printf(" swapLeftCtrlAndLeftAlt: %d\n", swapLeftCtrlAndLeftAlt);
		printf(" swapLeftCtrlLeftAltAndLeftWin: %d\n", swapLeftCtrlLeftAltAndLeftWin);
		printf(" capsLockAsEscape: %d\n", capsLockAsEscape);
		printf(" mod3RAsReturn: %d\n", mod3RAsReturn);
		printf(" modTapTimeout: %d\n", modTapTimeout);
		printf(" preferDeadKeyPlusSpace: %d\n", preferDeadKeyPlusSpace);
		printf(" capsLockAndQuoteAsShift: %d\n", capsLockAndQuoteAsShift);
		printf(" debugWindow: %d\n\n", debugWindow);

		// char const* const fileName = argv[1]; /* should check that argc > 1 */
		FILE* file = fopen(ini, "r"); /* should check the result */
		char line[256];
		int i = 0;

		while (fgets(line, sizeof(line), file) && i < MOD_TAP_LEN) {
			if (strstr(line, "=ModTap(") != NULL) {
				printf("%s", line);
				char *token = strtok(line, "=");
				if (token == NULL) continue;
				int keycode = (int)token[0];
				uint8_t c = token[0];
				//2-byte UTF sequence
				if (c >> 5 == 0b110) {
					keycode = (int)((token[0] << 6) & 0b0000011111000000) | ((token[1] << 0) & 0b0000000000111111);
				}
				token = strtok(NULL, "(");
				if (token == NULL) continue;
				token = strtok(NULL, ")");
				if (token == NULL) continue;
				if (strcmp(token, "ctrl") == 0) {
					modTap[i].modifier = MT_CTRL;
				} else if (strcmp(token, "shift") == 0) {
					modTap[i].modifier = MT_SHIFT;
				} else if (strcmp(token, "mod3") == 0) {
					modTap[i].modifier = MT_MOD3;
				} else if (strcmp(token, "alt") == 0) {
					modTap[i].modifier = MT_ALT;
				} else if (strcmp(token, "win") == 0) {
					modTap[i].modifier = MT_WIN;
				} else {
					printf(line);
					printf("Unknown modifier %s\n", token);
					printf("Please use one of these: ctrl, shift, mod3, alt, win.\n");
					continue;
				}
				modTap[i].keycode = keycode;
				i++;
			}
		}
		fclose(file);

	} else {
		printf("\nKeine settings.ini gefunden: %s\n\n", ini);
	}

	if (argc >= 2) {
		printf("\nEinstellungen von der Kommandozeile:");
		const char delimiter[] = "=";
		char *param, *value;
		for (int i = 1; i < argc; i++) {
			if (strcmp(argv[i], "gay") == 0 || strcmp(argv[i], "qwertz") == 0) {
				strncpy(layout, argv[i], 100);
				printf("\n Layout: %s", layout);
				continue;
			}

			if (strstr(argv[i], delimiter) == NULL) {
				printf("\nUnbekannter Parameter: %s", argv[i]);
				continue;
			}

			//printf("\narg%d: %s", i, argv[i]);
			param = strtok(argv[i], delimiter);
			if (param == NULL) {
				printf("\nUnbekannter Parameter: %s", argv[i]);
				continue;
			}

			value = strtok(NULL, delimiter);
			if (value == NULL) {
				printf("\nUnbekannter Parameter: %s", argv[i]);
				continue;
			}

			if (strcmp(param, "debugWindow") == 0) {
				bool debugWindowAlreadyStarted = debugWindow;
				debugWindow = (strcmp(value, "1") == 0);
				if (debugWindow && !debugWindowAlreadyStarted) {
					// Open Console Window to see printf output
					SetStdOutToNewConsole();
				}
				printf("\n debugWindow: %d", debugWindow);

			} else if (strcmp(param, "layout") == 0) {
				strncpy(layout, value, 100);
				printf("\n Layout: %s", layout);

			} else if (strcmp(param, "customLayout") == 0) {
				strncpy(customLayout, value, 65);
				printf("\n Custom layout: %s", customLayout);

			} else if (strcmp(param, "symmetricalLevel3Modifiers") == 0) {
				quoteAsMod3R = (strcmp(value, "1") == 0);
				printf("\n symmetricalLevel3Modifiers: %d", quoteAsMod3R);

			} else if (strcmp(param, "returnKeyAsMod3R") == 0) {
				returnAsMod3R = (strcmp(value, "1") == 0);
				printf("\n returnKeyAsMod3R: %d", returnAsMod3R);
			} else if (strcmp(param, "capsLockEnabled") == 0) {
				capsLockEnabled = (strcmp(value, "1") == 0);
				printf("\n capsLockEnabled: %d", capsLockEnabled);

			} else if (strcmp(param, "shiftLockEnabled") == 0) {
				shiftLockEnabled = (strcmp(value, "1") == 0);
				printf("\n shiftLockEnabled: %d", shiftLockEnabled);
			} else if (strcmp(param, "qwertzForShortcuts") == 0) {
				qwertzForShortcuts = (strcmp(value, "1") == 0);
				printf("\n qwertzForShortcuts: %d", qwertzForShortcuts);

			} else if (strcmp(param, "swapLeftCtrlAndLeftAlt") == 0) {
				swapLeftCtrlAndLeftAlt = (strcmp(value, "1") == 0);
				printf("\n swapLeftCtrlAndLeftAlt: %d", swapLeftCtrlAndLeftAlt);

			} else if (strcmp(param, "swapLeftCtrlLeftAltAndLeftWin") == 0) {
				swapLeftCtrlLeftAltAndLeftWin = (strcmp(value, "1") == 0);
				printf("\n swapLeftCtrlLeftAltAndLeftWin: %d", swapLeftCtrlLeftAltAndLeftWin);
			} else if (strcmp(param, "capsLockAsEscape") == 0) {
				capsLockAsEscape = (strcmp(value, "1") == 0);
				printf("\n capsLockAsEscape: %d", capsLockAsEscape);

			} else if (strcmp(param, "mod3RAsReturn") == 0) {
				mod3RAsReturn = (strcmp(value, "1") == 0);
				printf("\n mod3RAsReturn: %d", mod3RAsReturn);
			} else if (strcmp(param, "modTapTimeout") == 0) {
				modTapTimeout = atoi(value);
				printf("\n modTapTimeout: %d", modTapTimeout);

			} else if (strcmp(param, "preferDeadKeyPlusSpace") == 0) {
				preferDeadKeyPlusSpace = (strcmp(value, "1") == 0);
				printf("\n preferDeadKeyPlusSpace: %d", preferDeadKeyPlusSpace);

			} else if (strcmp(param, "capsLockAndQuoteAsShift") == 0) {
				capsLockAndQuoteAsShift = (strcmp(value, "1") == 0);
				printf("\n capsLockAndQuoteAsShift: %d", capsLockAndQuoteAsShift);

			} else {
				printf("\nUnbekannter Parameter:%s", param);
			}
		}
	}
	// transform possibly UTF-8 encoded custom layout string to UTF-16
	str2wcs(customLayoutWcs, customLayout, 33);

	if (quoteAsMod3R)
		// use ä/quote key instead of #/backslash key as right level 3 modifier
		scanCodeMod3R = SCANCODE_QUOTE_KEY;
	else if (returnAsMod3R)
		// use return key instead of #/backslash as right level 3 modifier
		// (might be useful for US keyboards because the # key is missing there)
		scanCodeMod3R = SCANCODE_RETURN_KEY;
	// console handle: needed for coloring the output
	hConsole = GetStdHandle(STD_OUTPUT_HANDLE);
	SetConsoleTextAttribute(hConsole, FG_WHITE);
	// Catch ctrl-c because it will send keydown for ctrl
	// but then keyup for alt. Then ctrl would be locked.
	// Also needed for removing tray icon when quitting with ctrl-c.
	SetConsoleCtrlHandler(CtrlHandler, TRUE);

	SetConsoleCP(CP_UTF8);
	SetConsoleOutputCP(CP_UTF8);

	initCharacterToScanCodeMap();
	initLayout();

	resetKeyQueue();

	HINSTANCE hInstance = GetModuleHandle(NULL);
	trayicon_init(LoadIcon(hInstance, MAKEINTRESOURCE(IDI_APPICON)), APPNAME);
	trayicon_add_item(NULL, &toggleBypassMode);
	trayicon_add_item("Exit", &exitApplication);

	DWORD tid;
	HANDLE thread = CreateThread(0, 0, hookThreadMain, argv[0], 0, &tid);

	MSG msg;
	while (GetMessage(&msg, 0, 0, 0) > 0) {
		// this seems to be necessary only for clicking exit in the system tray menu
		DispatchMessage(&msg);
	}
	return 0;
}
