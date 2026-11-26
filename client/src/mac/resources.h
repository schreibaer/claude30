/* Resource IDs shared by the C code and claude30.r. Plain #defines only, so
   Rez can include this file too. */
#ifndef RESOURCES_H
#define RESOURCES_H

#define kCreator        'Cl30'
#define kPrefsType      'PREF'
#define kPrefsID        128

/* Menus */
#define kMenuBar        128
#define kAppleMenu      128
#define kFileMenu       129
#define kEditMenu       130
#define kSessionMenu    131
#define kWindowMenu     132

#define kAboutItem      1

/* File menu, as numbered while Open Project File is shown (files=ro|rw);
   under files=off the item is removed and the rest move up by one. */
#define kOpenFileItem   1
#define kCloseItem      2
#define kSaveItem       3
#define kRevertItem     4
#define kFullScreenItem 6
#define kCompactItem    7
#define kQuitItem       9

#define kUndoItem       1
#define kCutItem        3
#define kCopyItem       4
#define kPasteItem      5
#define kClearItem      6
#define kSelectAllItem  7
#define kSettingsItem   9

#define kConnectItem    1
#define kDisconnectItem 2
#define kStopItem       3
#define kPrevPromptItem 5
#define kNextPromptItem 6
#define kFoldItem       7
#define kNewTabItem     9
#define kNewChatItem    10
#define kCloseTabItem   11
#define kModelItem      13
#define kLinkTestItem   15

/* Window */
#define kMainWindow     128

/* Dialogs and alerts */
#define kAboutAlert     128
#define kRedialAlert    130     /* ^0 with Redial and OK */
#define kSettingsDialog 131
#define kPermDialog     132
#define kLinkTestDialog 133
#define kLinkMinutesDialog 134
#define kPickerDialog   135
#define kOpenDialog     136
#define kNoteAlert      137     /* ^0 with OK */
#define kCloseBusyAlert 138     /* 1 Close, 2 Cancel */
#define kSaveChangesAlert 139   /* 1 Save, 2 Cancel, 3 Don't Save */
#define kCantSaveAlert  140     /* 1 Don't Save, 2 Cancel */
#define kConflictAlert  141     /* 1 Cancel, 2 Replace */
#define kRevertAlert    142     /* 1 Revert, 2 Cancel */
#define kModelDialog    143

#define kRedialButton   1

#define kSetOK          1
#define kSetCancel      2
#define kSetModem       3
#define kSetPrinter     4
#define kSet57600       5
#define kSet38400       6
#define kSet19200       7
#define kSetDial        8
#define kSetHideBar     13

#define kPermAllow      1
#define kPermDeny       2
#define kPermAlways     3
#define kPermSummary    4
#define kPermList       5
#define kPermHint       6
#define kPermHeadline   7

#define kLinkStop       1
#define kLinkInfo       2

#define kMinStart       1
#define kMinCancel      2
#define kMinField       3

#define kPickOpen       1
#define kPickCancel     2
#define kPickList       3
#define kPickMessage    4
#define kPickNewChat    5

#define kOpenOpen       1
#define kOpenCancel     2
#define kOpenList       3
#define kOpenPath       4
#define kOpenUp         5
#define kOpenMessage    6
#define kOpenWaiting    7

#define kModelOK        1
#define kModelCancel    2
#define kModelList      3
#define kModelTitle     4
#define kModelEffortLabel 5
#define kModelEffort0   6       /* six radio buttons: default, low, medium, high, xhigh, max */
#define kModelEffort5   11

#endif
