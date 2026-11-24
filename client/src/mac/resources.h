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

#define kAboutItem      1
#define kQuitItem       1
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
#define kLinkTestItem   5

/* Window */
#define kMainWindow     128

/* Dialogs and alerts */
#define kAboutAlert     128
#define kRedialAlert    130     /* ^0 with Redial and OK */
#define kSettingsDialog 131
#define kPermDialog     132
#define kLinkTestDialog 133
#define kLinkMinutesDialog 134

#define kRedialButton   1

#define kSetOK          1
#define kSetCancel      2
#define kSetModem       3
#define kSetPrinter     4
#define kSet57600       5
#define kSet38400       6
#define kSet19200       7
#define kSetDial        8

#define kPermAllow      1
#define kPermDeny       2
#define kPermAlways     3
#define kPermSummary    4
#define kPermList       5
#define kPermHint       6

#define kLinkStop       1
#define kLinkInfo       2

#define kMinStart       1
#define kMinCancel      2
#define kMinField       3

#endif
