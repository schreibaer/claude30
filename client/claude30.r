/* Resources for Claude30: menus, the window, dialogs, alerts, the Finder
   icon and the SIZE resource. Built with Retro68's Rez. */
#include "Processes.r"
#include "Menus.r"
#include "Windows.r"
#include "Dialogs.r"
#include "Finder.r"
#include "Icons.r"
#include "src/mac/resources.h"

/* ---- memory: 2 MB preferred, 1 MB minimum ----------------------------- */

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
	2048 * 1024,
	1024 * 1024
};

/* ---- menus ------------------------------------------------------------- */

resource 'MBAR' (kMenuBar) {
	{ kAppleMenu, kFileMenu, kEditMenu, kSessionMenu }
};

resource 'MENU' (kAppleMenu) {
	kAppleMenu, textMenuProc, 0x7FFFFFFD, enabled, apple,
	{
		"About Claude30\0xC9", noIcon, noKey, noMark, plain;
		"-", noIcon, noKey, noMark, plain
	}
};

resource 'MENU' (kFileMenu) {
	kFileMenu, textMenuProc, 0x7FFFFFFD, enabled, "File",
	{
		"Full Screen", noIcon, "F", noMark, plain;
		"-", noIcon, noKey, noMark, plain;
		"Quit", noIcon, "Q", noMark, plain
	}
};

resource 'MENU' (kEditMenu) {
	kEditMenu, textMenuProc, 0x7FFFFF7D, enabled, "Edit",
	{
		"Undo", noIcon, "Z", noMark, plain;
		"-", noIcon, noKey, noMark, plain;
		"Cut", noIcon, "X", noMark, plain;
		"Copy", noIcon, "C", noMark, plain;
		"Paste", noIcon, "V", noMark, plain;
		"Clear", noIcon, noKey, noMark, plain;
		"Select All", noIcon, noKey, noMark, plain;
		"-", noIcon, noKey, noMark, plain;
		"Settings\0xC9", noIcon, noKey, noMark, plain
	}
};

resource 'MENU' (kSessionMenu) {
	kSessionMenu, textMenuProc, 0x7FFFFFF7, enabled, "Session",
	{
		"Connect", noIcon, "K", noMark, plain;
		"Disconnect", noIcon, noKey, noMark, plain;
		"Stop", noIcon, ".", noMark, plain;
		"-", noIcon, noKey, noMark, plain;
		"Link Test\0xC9", noIcon, noKey, noMark, plain
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
			"Claude30 1.0\r"
			"Use Claude Code from this Mac through se30bridge and a WiFi232 modem.\r"
			"Speaks app mode of protocol version 1." };
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
	{ 60, 80, 260, 432 },
	dBoxProc, invisible, noGoAway, 0,
	kSettingsDialog, "", alertPositionMainScreen
};

resource 'DITL' (kSettingsDialog) {
	{
		{ 168, 262, 188, 332 }, Button { enabled, "OK" };
		{ 168, 172, 188, 242 }, Button { enabled, "Cancel" };
		{ 30, 20, 46, 170 }, RadioButton { enabled, "Modem port" };
		{ 50, 20, 66, 170 }, RadioButton { enabled, "Printer port" };
		{ 30, 190, 46, 330 }, RadioButton { enabled, "57,600 baud" };
		{ 50, 190, 66, 330 }, RadioButton { enabled, "38,400 baud" };
		{ 70, 190, 86, 330 }, RadioButton { enabled, "19,200 baud" };
		{ 116, 22, 132, 330 }, EditText { enabled, "" };
		{ 10, 20, 26, 170 }, StaticText { disabled, "Port" };
		{ 10, 190, 26, 330 }, StaticText { disabled, "Speed" };
		{ 94, 20, 110, 330 }, StaticText { disabled, "Dial string, sent to the modem as typed" };
		{ 138, 20, 162, 340 }, StaticText { disabled,
			"The speed must match the WiFi232 and run.sh --baud. Changes apply at the next Connect." };
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
		{ 278, 410, 298, 490 }, Button { enabled, "Allow" };
		{ 278, 320, 298, 400 }, Button { enabled, "Deny" };
		{ 278, 230, 298, 310 }, Button { enabled, "Always" };
		{ 6, 10, 42, 490 }, StaticText { disabled, "" };
		{ 46, 10, 270, 490 }, UserItem { disabled };
		{ 280, 10, 298, 224 }, StaticText { disabled, "\0x11Y \0x11N \0x11A   \0x11. stops" };
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
	"Claude30 1.0"
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
