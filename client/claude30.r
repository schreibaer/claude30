/* Resources for Claude30: menus, the window, dialogs, alerts, the Finder
   icon and the SIZE resource. Built with Retro68's Rez. */
#include "Processes.r"
#include "Menus.r"
#include "Windows.r"
#include "Dialogs.r"
#include "Finder.r"
#include "Icons.r"
#include "src/mac/resources.h"

/* ---- memory: 3.5 MB preferred, 2 MB minimum (six tabs, six file windows) */

resource 'SIZE' (-1) {
	reserved,
	acceptSuspendResumeEvents,
	reserved,
	canBackground,
	doesActivateOnFGSwitch,
	backgroundAndForeground,
	dontGetFrontClicks,
	ignoreChildDiedEvents,
	is32BitCompatible,
	notHighLevelEventAware,
	onlyLocalHLEvents,
	notStationeryAware,
	dontUseTextEditServices,
	reserved,
	reserved,
	reserved,
	3584 * 1024,
	2048 * 1024
};

/* ---- menus ------------------------------------------------------------- */

resource 'MBAR' (kMenuBar) {
	{ kAppleMenu, kFileMenu, kEditMenu, kSessionMenu, kWindowMenu }
};

resource 'MENU' (kAppleMenu) {
	kAppleMenu, textMenuProc, 0x7FFFFFFD, enabled, apple,
	{
		"About Claude30\0xC9", noIcon, noKey, noMark, plain;
		"-", noIcon, noKey, noMark, plain
	}
};

/* The enable flags are Rez's 31-bit field: bit 0 is item 1, bit 1 item 2,
   and so on (the menu's own flag is the boolean after it). Separators are
   off; adjust_menus() sets the rest at every menu pull. */
resource 'MENU' (kFileMenu) {
	kFileMenu, textMenuProc, 0x7FFFFF6F, enabled, "File",
	{
		"Open Project File\0xC9", noIcon, "O", noMark, plain;
		"Close", noIcon, "W", noMark, plain;
		"Save", noIcon, "S", noMark, plain;
		"Revert to Saved", noIcon, noKey, noMark, plain;
		"-", noIcon, noKey, noMark, plain;
		"Full Screen (Cmd-Shift-F)", noIcon, noKey, noMark, plain;
		"Compact Input", noIcon, "E", noMark, plain;
		"-", noIcon, noKey, noMark, plain;
		"Quit", noIcon, "Q", noMark, plain
	}
};

resource 'MENU' (kEditMenu) {
	kEditMenu, textMenuProc, 0x7FFFF77D, enabled, "Edit",
	{
		"Undo", noIcon, "Z", noMark, plain;
		"-", noIcon, noKey, noMark, plain;
		"Cut", noIcon, "X", noMark, plain;
		"Copy", noIcon, "C", noMark, plain;
		"Paste", noIcon, "V", noMark, plain;
		"Clear", noIcon, noKey, noMark, plain;
		"Select All", noIcon, noKey, noMark, plain;
		"-", noIcon, noKey, noMark, plain;
		"Find\0xC9", noIcon, "F", noMark, plain;
		"Find Again", noIcon, "G", noMark, plain;
		"Find Newer (Cmd-Shift-G)", noIcon, noKey, noMark, plain;
		"-", noIcon, noKey, noMark, plain;
		"Settings\0xC9", noIcon, noKey, noMark, plain
	}
};

resource 'MENU' (kSessionMenu) {
	kSessionMenu, textMenuProc, 0x7FFEF777, enabled, "Session",
	{
		"Connect", noIcon, "K", noMark, plain;
		"Disconnect", noIcon, "D", noMark, plain;
		"Stop", noIcon, ".", noMark, plain;
		"-", noIcon, noKey, noMark, plain;
		"Previous Prompt (Cmd-Up)", noIcon, noKey, noMark, plain;
		"Next Prompt (Cmd-Down)", noIcon, noKey, noMark, plain;
		"Open or Close Fold", noIcon, "J", noMark, plain;
		"-", noIcon, noKey, noMark, plain;
		"New Tab\0xC9", noIcon, "T", noMark, plain;
		"New Chat", noIcon, "N", noMark, plain;
		"Close Tab", noIcon, noKey, noMark, plain;
		"-", noIcon, noKey, noMark, plain;
		"Model and Effort\0xC9", noIcon, "M", noMark, plain;
		"Stream Answers", noIcon, noKey, noMark, plain;
		"Plan Mode", noIcon, noKey, noMark, plain;
		"Show Changes", noIcon, noKey, noMark, plain;
		"-", noIcon, noKey, noMark, plain;
		"Link Test\0xC9", noIcon, noKey, noMark, plain
	}
};

/* Filled at run time: the tabs (Cmd-1 to Cmd-6), then the file windows. */
resource 'MENU' (kWindowMenu) {
	kWindowMenu, textMenuProc, allEnabled, enabled, "Window",
	{
	}
};

/* ---- the conversation window: fills a 512 x 342 screen ------------------ */

resource 'WIND' (kMainWindow) {
	{ 38, 2, 340, 510 },
	noGrowDocProc,
	invisible,
	noGoAway,
	0,
	"Claude30",
	noAutoCenter
};

/* ---- alerts -------------------------------------------------------------- */

resource 'ALRT' (kAboutAlert) {
	{ 70, 66, 190, 446 },
	kAboutAlert,
	{ OK, visible, silent; OK, visible, silent; OK, visible, silent; OK, visible, silent },
	alertPositionMainScreen
};

resource 'DITL' (kAboutAlert) {
	{
		{ 88, 290, 108, 370 }, Button { enabled, "OK" };
		{ 10, 10, 80, 370 }, StaticText { disabled,
			"Claude30 2.0\r"
			"Use Claude Code from this Mac through se30bridge and a WiFi232 modem: "
			"up to six projects in tabs, and their files.\r"
			"Speaks protocol version 2." };
	}
};

resource 'ALRT' (kRedialAlert) {
	{ 80, 76, 196, 436 },
	kRedialAlert,
	{ OK, visible, silent; OK, visible, silent; OK, visible, silent; OK, visible, silent },
	alertPositionMainScreen
};

resource 'DITL' (kRedialAlert) {
	{
		{ 84, 270, 104, 350 }, Button { enabled, "Redial" };
		{ 84, 180, 104, 260 }, Button { enabled, "OK" };
		{ 10, 60, 74, 350 }, StaticText { disabled, "^0" };
		{ 10, 14, 42, 46 }, Icon { disabled, 2 };
	}
};

/* ---- Settings ------------------------------------------------------------ */

resource 'DLOG' (kSettingsDialog) {
	{ 50, 80, 302, 432 },
	dBoxProc, invisible, noGoAway, 0,
	kSettingsDialog, "", alertPositionMainScreen
};

resource 'DITL' (kSettingsDialog) {
	{
		{ 222, 262, 242, 332 }, Button { enabled, "OK" };
		{ 222, 172, 242, 242 }, Button { enabled, "Cancel" };
		{ 30, 20, 46, 170 }, RadioButton { enabled, "Modem port" };
		{ 50, 20, 66, 170 }, RadioButton { enabled, "Printer port" };
		{ 30, 190, 46, 330 }, RadioButton { enabled, "57,600 baud" };
		{ 50, 190, 66, 330 }, RadioButton { enabled, "38,400 baud" };
		{ 70, 190, 86, 330 }, RadioButton { enabled, "19,200 baud" };
		{ 116, 22, 132, 330 }, EditText { enabled, "" };
		{ 10, 20, 26, 170 }, StaticText { disabled, "Port" };
		{ 10, 190, 26, 330 }, StaticText { disabled, "Speed" };
		{ 94, 20, 110, 330 }, StaticText { disabled, "Dial string, sent to the modem as typed" };
		{ 166, 20, 214, 340 }, StaticText { disabled,
			"The speed must match the WiFi232 and run.sh --baud. Port, speed and dial string apply at the next Connect." };
		{ 142, 20, 158, 170 }, CheckBox { enabled, "Hide scroll bar" };
		{ 142, 190, 158, 330 }, CheckBox { enabled, "Hide cost" };
	}
};

/* ---- permission request: fits under the menu bar on a 512 x 342 screen -- */

resource 'DLOG' (kPermDialog) {
	{ 28, 6, 336, 506 },
	dBoxProc, invisible, noGoAway, 0,
	kPermDialog, "", noAutoCenter
};

resource 'DITL' (kPermDialog) {
	{
		{ 278, 424, 298, 490 }, Button { enabled, "Allow" };
		{ 278, 352, 298, 418 }, Button { enabled, "Deny" };
		{ 278, 280, 298, 346 }, Button { enabled, "Always" };
		{ 24, 10, 40, 490 }, UserItem { disabled };
		{ 44, 10, 270, 490 }, UserItem { disabled };
		{ 280, 10, 298, 200 }, StaticText { disabled, "\0x11Y \0x11N \0x11A   \0x11. stops" };
		{ 4, 10, 22, 490 }, UserItem { disabled };
		{ 278, 208, 298, 274 }, Button { enabled, "Rule..." };
	}
};

/* The rule to save with an allowed request. Item 1 is an inert placeholder
   so that no button is the default one and Return does nothing. */
resource 'DLOG' (kRuleDialog) {
	{ 80, 76, 236, 436 },
	dBoxProc, invisible, noGoAway, 0,
	kRuleDialog, "", alertPositionMainScreen
};

resource 'DITL' (kRuleDialog) {
	{
		{ 2, 2, 3, 3 }, UserItem { disabled };
		{ 122, 262, 142, 346 }, Button { enabled, "Save Rule" };
		{ 122, 172, 142, 252 }, Button { enabled, "Cancel" };
		{ 36, 12, 54, 348 }, EditText { enabled, "" };
		{ 62, 12, 110, 348 }, StaticText { disabled, "" };
		{ 10, 12, 30, 348 }, StaticText { disabled, "Always allow this in this project:" };
	}
};

/* ---- alerts for tabs and files ------------------------------------------ */

resource 'ALRT' (kNoteAlert) {
	{ 80, 76, 196, 436 },
	kNoteAlert,
	{ OK, visible, silent; OK, visible, silent; OK, visible, silent; OK, visible, silent },
	alertPositionMainScreen
};

resource 'DITL' (kNoteAlert) {
	{
		{ 84, 270, 104, 350 }, Button { enabled, "OK" };
		{ 10, 60, 74, 350 }, StaticText { disabled, "^0" };
		{ 10, 14, 42, 46 }, Icon { disabled, 1 };
	}
};

resource 'ALRT' (kCloseBusyAlert) {
	{ 80, 76, 186, 436 },
	kCloseBusyAlert,
	{ Cancel, visible, silent; Cancel, visible, silent; Cancel, visible, silent; Cancel, visible, silent },
	alertPositionMainScreen
};

resource 'DITL' (kCloseBusyAlert) {
	{
		{ 74, 270, 94, 350 }, Button { enabled, "Close" };
		{ 74, 180, 94, 260 }, Button { enabled, "Cancel" };
		{ 10, 60, 64, 350 }, StaticText { disabled,
			"Claude is still working in \"^0\". Close the tab and stop its turn?" };
		{ 10, 14, 42, 46 }, Icon { disabled, 2 };
	}
};

resource 'ALRT' (kSaveChangesAlert) {
	{ 80, 66, 186, 446 },
	kSaveChangesAlert,
	{ OK, visible, silent; OK, visible, silent; OK, visible, silent; OK, visible, silent },
	alertPositionMainScreen
};

resource 'DITL' (kSaveChangesAlert) {
	{
		{ 74, 290, 94, 370 }, Button { enabled, "Save" };
		{ 74, 200, 94, 280 }, Button { enabled, "Cancel" };
		{ 74, 60, 94, 150 }, Button { enabled, "Don\0xD5t Save" };
		{ 10, 60, 64, 370 }, StaticText { disabled,
			"Save changes to \"^0\" on the host?" };
		{ 10, 14, 42, 46 }, Icon { disabled, 2 };
	}
};

resource 'ALRT' (kCantSaveAlert) {
	{ 80, 66, 186, 446 },
	kCantSaveAlert,
	{ Cancel, visible, silent; Cancel, visible, silent; Cancel, visible, silent; Cancel, visible, silent },
	alertPositionMainScreen
};

resource 'DITL' (kCantSaveAlert) {
	{
		{ 74, 290, 94, 370 }, Button { enabled, "Don\0xD5t Save" };
		{ 74, 200, 94, 280 }, Button { enabled, "Cancel" };
		{ 10, 60, 64, 370 }, StaticText { disabled,
			"Can\0xD5t save \"^0\": not connected. Don\0xD5t Save throws away your changes." };
		{ 10, 14, 42, 46 }, Icon { disabled, 2 };
	}
};

resource 'ALRT' (kConflictAlert) {
	{ 80, 66, 186, 446 },
	kConflictAlert,
	{ OK, visible, silent; OK, visible, silent; OK, visible, silent; OK, visible, silent },
	alertPositionMainScreen
};

resource 'DITL' (kConflictAlert) {
	{
		{ 74, 290, 94, 370 }, Button { enabled, "Cancel" };
		{ 74, 200, 94, 280 }, Button { enabled, "Replace" };
		{ 10, 60, 64, 370 }, StaticText { disabled,
			"\"^0\" changed on the host since you opened it. Replace it with your version?" };
		{ 10, 14, 42, 46 }, Icon { disabled, 2 };
	}
};

resource 'ALRT' (kRevertAlert) {
	{ 80, 66, 186, 446 },
	kRevertAlert,
	{ Cancel, visible, silent; Cancel, visible, silent; Cancel, visible, silent; Cancel, visible, silent },
	alertPositionMainScreen
};

resource 'DITL' (kRevertAlert) {
	{
		{ 74, 290, 94, 370 }, Button { enabled, "Revert" };
		{ 74, 200, 94, 280 }, Button { enabled, "Cancel" };
		{ 10, 60, 64, 370 }, StaticText { disabled,
			"Throw away your changes to \"^0\" and load it again from the host?" };
		{ 10, 14, 42, 46 }, Icon { disabled, 2 };
	}
};

/* ---- project picker -------------------------------------------------------- */

resource 'DLOG' (kPickerDialog) {
	{ 50, 106, 290, 406 },
	dBoxProc, invisible, noGoAway, 0,
	kPickerDialog, "", alertPositionMainScreen
};

resource 'DITL' (kPickerDialog) {
	{
		{ 208, 210, 228, 290 }, Button { enabled, "Open" };
		{ 208, 120, 228, 200 }, Button { enabled, "Cancel" };
		{ 46, 12, 198, 288 }, UserItem { disabled };
		{ 8, 12, 40, 288 }, StaticText { disabled, "Open a project in a new tab:" };
		{ 208, 12, 228, 108 }, Button { enabled, "New Chat" };
	}
};

/* ---- model and effort ------------------------------------------------------ */

resource 'DLOG' (kModelDialog) {
	{ 50, 86, 290, 426 },
	dBoxProc, invisible, noGoAway, 0,
	kModelDialog, "", alertPositionMainScreen
};

resource 'DITL' (kModelDialog) {
	{
		{ 208, 254, 228, 330 }, Button { enabled, "OK" };
		{ 208, 166, 228, 242 }, Button { enabled, "Cancel" };
		{ 46, 12, 198, 170 }, UserItem { disabled };
		{ 8, 12, 40, 330 }, StaticText { disabled, "Model and effort for this tab:" };
		{ 46, 186, 62, 330 }, StaticText { disabled, "Effort:" };
		{ 66, 186, 84, 330 }, RadioButton { enabled, "Bridge default" };
		{ 86, 186, 104, 330 }, RadioButton { enabled, "Low" };
		{ 106, 186, 124, 330 }, RadioButton { enabled, "Medium" };
		{ 126, 186, 144, 330 }, RadioButton { enabled, "High" };
		{ 146, 186, 164, 330 }, RadioButton { enabled, "Extra high" };
		{ 166, 186, 184, 330 }, RadioButton { enabled, "Max" };
	}
};

/* ---- Open Project File ------------------------------------------------------ */

resource 'DLOG' (kOpenDialog) {
	{ 40, 66, 300, 446 },
	dBoxProc, invisible, noGoAway, 0,
	kOpenDialog, "", alertPositionMainScreen
};

resource 'DITL' (kOpenDialog) {
	{
		{ 228, 290, 248, 370 }, Button { enabled, "Open" };
		{ 228, 200, 248, 280 }, Button { enabled, "Cancel" };
		{ 34, 12, 188, 370 }, UserItem { disabled };
		{ 10, 12, 26, 300 }, StaticText { disabled, "" };
		{ 8, 310, 26, 370 }, Button { enabled, "Up" };
		{ 192, 12, 208, 370 }, StaticText { disabled, "" };
		{ 210, 12, 226, 370 }, StaticText { disabled, "" };
	}
};

/* ---- link test ----------------------------------------------------------- */

resource 'DLOG' (kLinkMinutesDialog) {
	{ 90, 120, 200, 392 },
	dBoxProc, invisible, noGoAway, 0,
	kLinkMinutesDialog, "", alertPositionMainScreen
};

resource 'DITL' (kLinkMinutesDialog) {
	{
		{ 78, 182, 98, 262 }, Button { enabled, "Start" };
		{ 78, 92, 98, 172 }, Button { enabled, "Cancel" };
		{ 44, 22, 60, 72 }, EditText { enabled, "60" };
		{ 10, 20, 38, 262 }, StaticText { disabled, "Run the link test against run.sh --demo for how long?" };
		{ 44, 84, 60, 200 }, StaticText { disabled, "minutes" };
	}
};

resource 'DLOG' (kLinkTestDialog) {
	{ 60, 100, 260, 412 },
	dBoxProc, invisible, noGoAway, 0,
	kLinkTestDialog, "", alertPositionMainScreen
};

resource 'DITL' (kLinkTestDialog) {
	{
		{ 168, 222, 188, 302 }, Button { enabled, "Stop" };
		{ 10, 10, 160, 302 }, UserItem { disabled };
	}
};

/* ---- Finder -------------------------------------------------------------- */

resource 'BNDL' (128) {
	kCreator, 0,
	{
		'ICN#', { 0, 128 };
		'FREF', { 0, 128 }
	}
};

resource 'FREF' (128) {
	'APPL', 0, ""
};

type kCreator as 'STR ';
resource kCreator (0, "Owner resource") {
	"Claude30 2.0"
};

resource 'ICN#' (128) {
	{
		$"0000000003FFFFC00200004002000040"
		$"027FFE4002400240025FFA4002500A40"
		$"02540A4002520A400254EA40025FFA40"
		$"02440240024802400240024002400240"
		$"027FFE40020000400200004002000040"
		$"020000400201FC400200004002000040"
		$"0200004002FFFF400280014002FFFF40"
		$"0200004003FFFFC00000000000000000",
		$"0000000003FFFFC003FFFFC003FFFFC0"
		$"03FFFFC003FFFFC003FFFFC003FFFFC0"
		$"03FFFFC003FFFFC003FFFFC003FFFFC0"
		$"03FFFFC003FFFFC003FFFFC003FFFFC0"
		$"03FFFFC003FFFFC003FFFFC003FFFFC0"
		$"03FFFFC003FFFFC003FFFFC003FFFFC0"
		$"03FFFFC003FFFFC003FFFFC003FFFFC0"
		$"03FFFFC003FFFFC00000000000000000"
	}
};

/* ---- Find ---------------------------------------------------------------- */

resource 'DLOG' (kFindDialog) {
	{ 90, 120, 180, 392 },
	dBoxProc, invisible, noGoAway, 0,
	kFindDialog, "", alertPositionMainScreen
};
resource 'DITL' (kFindDialog) {
	{
		{ 58, 192, 78, 262 }, Button { enabled, "Find" };
		{ 58, 110, 78, 180 }, Button { enabled, "Cancel" };
		{ 30, 14, 46, 258 }, EditText { enabled, "" };
		{ 8, 14, 26, 258 }, StaticText { disabled, "Find in this window:" };
	}
};
