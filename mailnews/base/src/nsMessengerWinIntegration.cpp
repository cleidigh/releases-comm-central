/* -*- Mode: C++; tab-width: 2; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include <windows.h>
#include <shellapi.h>

#include "nsMessengerWinIntegration.h"
#include "nsIMsgAccountManager.h"
#include "nsIMsgMailSession.h"
#include "nsIMsgIncomingServer.h"
#include "nsIMsgIdentity.h"
#include "nsIMsgAccount.h"
#include "nsIMsgFolder.h"
#include "nsMsgDBFolder.h"
#include "nsIMsgWindow.h"
#include "nsCOMPtr.h"
#include "nsMsgBaseCID.h"
#include "nsMsgFolderFlags.h"
#include "nsDirectoryServiceDefs.h"
#include "nsAppDirectoryServiceDefs.h"
#include "nsIDirectoryService.h"
#include "nsIWindowWatcher.h"
#include "nsIWindowMediator.h"
#include "mozIDOMWindow.h"
#include "nsPIDOMWindow.h"
#include "nsIDocShell.h"
#include "nsIBaseWindow.h"
#include "nsIWidget.h"
#include "nsWidgetsCID.h"
#include "MailNewsTypes.h"
#include "nsIMessengerWindowService.h"
#include "prprf.h"
#include "nsIStringBundle.h"
#include "nsIAlertsService.h"
#include "nsIPrefService.h"
#include "nsIPrefBranch.h"
#include "nsIProperties.h"
#include "nsISupportsPrimitives.h"
#include "nsIInterfaceRequestorUtils.h"
#include "nsIWeakReferenceUtils.h"
#include "nsComponentManagerUtils.h"
#include "nsNativeCharsetUtils.h"
#include "nsMsgUtils.h"
#include "mozilla/LookAndFeel.h"
#include "mozilla/Services.h"
#include "mozilla/DebugOnly.h"
#include "nsIMutableArray.h"
#include "nsArrayUtils.h"

#include "mozilla/Components.h"
#include <stdlib.h>
#define PROFILE_COMMANDLINE_ARG u" -profile "

#define NOTIFICATIONCLASSNAME L"MailBiffNotificationMessageWindow"
#define UNREADMAILNODEKEY \
  u"Software\\Microsoft\\Windows\\CurrentVersion\\UnreadMail\\"
#define DOUBLE_QUOTE '"'
#define MAIL_COMMANDLINE_ARG u" -mail"
#define IDI_MAILBIFF 32576
#define UNREAD_UPDATE_INTERVAL (20 * 1000)  // 20 seconds
#define ALERT_CHROME_URL "chrome://messenger/content/newmailalert.xhtml"
#define NEW_MAIL_ALERT_ICON "chrome://messenger/skin/icons/new-mail-alert.png"
#define SHOW_ALERT_PREF "mail.biff.show_alert"
#define SHOW_TRAY_ICON_PREF "mail.biff.show_tray_icon"
#define SHOW_BALLOON_PREF "mail.biff.show_balloon"
#define SHOW_NEW_ALERT_PREF "mail.biff.show_new_alert"
#define ALERT_ORIGIN_PREF "ui.alertNotificationOrigin"

// since we are including windows.h in this file, undefine get user name....
#ifdef GetUserName
#  undef GetUserName
#endif

#ifndef NIIF_USER
#  define NIIF_USER 0x00000004
#endif

#ifndef NIIF_NOSOUND
#  define NIIF_NOSOUND 0x00000010
#endif

#ifndef NIN_BALOONUSERCLICK
#  define NIN_BALLOONUSERCLICK (WM_USER + 5)
#endif

using namespace mozilla;

// begin shameless copying from nsNativeAppSupportWin
static HWND hwndForDOMWindow(mozIDOMWindowProxy *window) {
  if (!window) {
    return 0;
  }
  nsCOMPtr<nsPIDOMWindowOuter> pidomwindow = nsPIDOMWindowOuter::From(window);

  nsCOMPtr<nsIBaseWindow> ppBaseWindow =
      do_QueryInterface(pidomwindow->GetDocShell());
  if (!ppBaseWindow) return 0;

  nsCOMPtr<nsIWidget> ppWidget;
  ppBaseWindow->GetMainWidget(getter_AddRefs(ppWidget));

  return (HWND)(ppWidget->GetNativeData(NS_NATIVE_WIDGET));
}

static void activateWindow(mozIDOMWindowProxy *win) {
  // Try to get native window handle.
  HWND hwnd = hwndForDOMWindow(win);
  if (hwnd) {
    // Restore the window if it is minimized.
    if (::IsIconic(hwnd)) ::ShowWindow(hwnd, SW_RESTORE);
    // Use the OS call, if possible.
    ::SetForegroundWindow(hwnd);
  } else {
    // Use internal method.
    nsCOMPtr<nsPIDOMWindowOuter> privateWindow = nsPIDOMWindowOuter::From(win);
    privateWindow->Focus(mozilla::dom::CallerType::System);
  }
}
// end shameless copying from nsNativeAppWinSupport.cpp

static void openMailWindow(const nsACString &aFolderUri) {
  nsresult rv;
  nsCOMPtr<nsIMsgMailSession> mailSession(
      do_GetService(NS_MSGMAILSESSION_CONTRACTID, &rv));
  if (NS_FAILED(rv)) return;

  nsCOMPtr<nsIMsgWindow> topMostMsgWindow;
  rv = mailSession->GetTopmostMsgWindow(getter_AddRefs(topMostMsgWindow));
  if (topMostMsgWindow) {
    nsCOMPtr<mozIDOMWindowProxy> domWindow;
    topMostMsgWindow->GetDomWindow(getter_AddRefs(domWindow));
    if (domWindow) {
      if (!aFolderUri.IsEmpty()) {
        nsCOMPtr<nsIMsgWindowCommands> windowCommands;
        topMostMsgWindow->GetWindowCommands(getter_AddRefs(windowCommands));
        if (windowCommands) windowCommands->SelectFolder(aFolderUri);
      }
      activateWindow(domWindow);
      return;
    }
  }

  {
    // the user doesn't have a mail window open already so open one for them...
    nsCOMPtr<nsIMessengerWindowService> messengerWindowService =
        do_GetService(NS_MESSENGERWINDOWSERVICE_CONTRACTID);
    // if we want to preselect the first account with new mail,
    // here is where we would try to generate a uri to pass in
    // (and add code to the messenger window service to make that work)
    if (messengerWindowService)
      messengerWindowService->OpenMessengerWindowWithUri(
          "mail:3pane", nsCString(aFolderUri).get(), nsMsgKey_None);
  }
}

static void CALLBACK delayedSingleClick(HWND msgWindow, UINT msg,
                                        INT_PTR idEvent, DWORD dwTime) {
  ::KillTimer(msgWindow, idEvent);

  // single clicks on the biff icon should re-open the alert notification
  nsresult rv = NS_OK;
  nsCOMPtr<nsIMessengerOSIntegration> integrationService =
      do_GetService(NS_MESSENGEROSINTEGRATION_CONTRACTID, &rv);
  if (NS_SUCCEEDED(rv)) {
    // we know we are dealing with the windows integration object
    nsMessengerWinIntegration *winIntegrationService =
        static_cast<nsMessengerWinIntegration *>(
            static_cast<nsIMessengerOSIntegration *>(integrationService.get()));
    winIntegrationService->ShowNewAlertNotification(true, EmptyString(),
                                                    EmptyString());
  }
}

// Window proc.
static LRESULT CALLBACK MessageWindowProc(HWND msgWindow, UINT msg, WPARAM wp,
                                          LPARAM lp) {
  if (msg == WM_USER) {
    if (lp == WM_LBUTTONDOWN) {
      // the only way to tell a single left click
      // from a double left click is to fire a timer which gets cleared if we
      // end up getting a WM_LBUTTONDBLK event.
      ::SetTimer(msgWindow, 1, GetDoubleClickTime(),
                 (TIMERPROC)delayedSingleClick);
    } else if (lp == WM_LBUTTONDBLCLK || lp == NIN_BALLOONUSERCLICK) {
      ::KillTimer(msgWindow, 1);
      openMailWindow(EmptyCString());
    }
  }

  return TRUE;
}

static HWND msgWindow;

// Create: Register class and create window.
static nsresult Create() {
  if (msgWindow) return NS_OK;

  WNDCLASS classStruct = {0,                       // style
                          &MessageWindowProc,      // lpfnWndProc
                          0,                       // cbClsExtra
                          0,                       // cbWndExtra
                          0,                       // hInstance
                          0,                       // hIcon
                          0,                       // hCursor
                          0,                       // hbrBackground
                          0,                       // lpszMenuName
                          NOTIFICATIONCLASSNAME};  // lpszClassName

  // Register the window class.
  NS_ENSURE_TRUE(::RegisterClass(&classStruct), NS_ERROR_FAILURE);
  // Create the window.
  NS_ENSURE_TRUE(msgWindow = ::CreateWindow(NOTIFICATIONCLASSNAME,
                                            0,           // title
                                            WS_CAPTION,  // style
                                            0, 0, 0, 0,  // x, y, cx, cy
                                            0,           // parent
                                            0,           // menu
                                            0,           // instance
                                            0),          // create struct
                 NS_ERROR_FAILURE);
  return NS_OK;
}

nsMessengerWinIntegration::nsMessengerWinIntegration() {
  mUnreadTimerActive = false;

  mBiffIconVisible = false;
  mSuppressBiffIcon = false;
  mAlertInProgress = false;
  mBiffIconInitialized = false;
  mFoldersWithNewMail = do_CreateInstance(NS_ARRAY_CONTRACTID);
}

nsMessengerWinIntegration::~nsMessengerWinIntegration() {
  if (mUnreadCountUpdateTimer) {
    mUnreadCountUpdateTimer->Cancel();
    mUnreadCountUpdateTimer = nullptr;
  }

  // one last attempt, update the registry
  mozilla::DebugOnly<nsresult> rv = UpdateRegistryWithCurrent();
  NS_ASSERTION(NS_SUCCEEDED(rv), "failed to update registry on shutdown");
  DestroyBiffIcon();
}

NS_IMPL_ADDREF(nsMessengerWinIntegration)
NS_IMPL_RELEASE(nsMessengerWinIntegration)

NS_INTERFACE_MAP_BEGIN(nsMessengerWinIntegration)
  NS_INTERFACE_MAP_ENTRY_AMBIGUOUS(nsISupports, nsIMessengerOSIntegration)
  NS_INTERFACE_MAP_ENTRY(nsIMessengerWindowsIntegration)
  NS_INTERFACE_MAP_ENTRY(nsIMessengerOSIntegration)
  NS_INTERFACE_MAP_ENTRY(nsIFolderListener)
  NS_INTERFACE_MAP_ENTRY(nsIObserver)
NS_INTERFACE_MAP_END

nsresult nsMessengerWinIntegration::ResetCurrent() {
  mInboxURI.Truncate();
  mEmail.Truncate();

  mCurrentUnreadCount = -1;
  mLastUnreadCountWrittenToRegistry = -1;

  mDefaultAccountMightHaveAnInbox = true;
  return NS_OK;
}

NOTIFYICONDATAW sBiffIconData = {(DWORD)NOTIFYICONDATAW_V2_SIZE,
                                 0,
                                 2,
                                 NIF_ICON | NIF_MESSAGE | NIF_TIP | NIF_INFO,
                                 WM_USER,
                                 0,
                                 L"",
                                 0,
                                 0,
                                 L"",
                                 {30000},
                                 L"",
                                 NIIF_USER | NIIF_NOSOUND};
// allow for the null terminator
static const uint32_t kMaxTooltipSize =
    sizeof(sBiffIconData.szTip) / sizeof(sBiffIconData.szTip[0]) - 1;
static const uint32_t kMaxBalloonSize =
    sizeof(sBiffIconData.szInfo) / sizeof(sBiffIconData.szInfo[0]) - 1;
static const uint32_t kMaxBalloonTitle =
    sizeof(sBiffIconData.szInfoTitle) / sizeof(sBiffIconData.szInfoTitle[0]) -
    1;

void nsMessengerWinIntegration::InitializeBiffStatusIcon() {
  // initialize our biff status bar icon
  Create();

  sBiffIconData.hWnd = (HWND)msgWindow;
  sBiffIconData.hIcon =
      ::LoadIcon(::GetModuleHandle(NULL), MAKEINTRESOURCE(IDI_MAILBIFF));

  mBiffIconInitialized = true;
}

nsresult nsMessengerWinIntegration::Init() {
  nsresult rv;

  nsCOMPtr<nsIMsgAccountManager> accountManager =
      do_GetService(NS_MSGACCOUNTMANAGER_CONTRACTID, &rv);
  NS_ENSURE_SUCCESS(rv, rv);

  // because we care if the default server changes
  rv = accountManager->AddRootFolderListener(this);
  NS_ENSURE_SUCCESS(rv, rv);

  nsCOMPtr<nsIMsgMailSession> mailSession =
      do_GetService(NS_MSGMAILSESSION_CONTRACTID, &rv);
  NS_ENSURE_SUCCESS(rv, rv);

  // because we care if the unread total count changes
  rv = mailSession->AddFolderListener(
      this, nsIFolderListener::boolPropertyChanged |
                nsIFolderListener::intPropertyChanged);
  NS_ENSURE_SUCCESS(rv, rv);

  // get current profile path for the commandliner
  nsCOMPtr<nsIProperties> directoryService =
      do_GetService(NS_DIRECTORY_SERVICE_CONTRACTID, &rv);
  NS_ENSURE_SUCCESS(rv, rv);

  nsCOMPtr<nsIFile> profilePath;
  rv = directoryService->Get(NS_APP_USER_PROFILE_50_DIR, NS_GET_IID(nsIFile),
                             getter_AddRefs(profilePath));
  NS_ENSURE_SUCCESS(rv, rv);

  rv = profilePath->GetPath(mProfilePath);
  NS_ENSURE_SUCCESS(rv, rv);

  // get application path
  WCHAR appPath[_MAX_PATH] = {0};
  ::GetModuleFileNameW(nullptr, appPath, sizeof(appPath));
  mAppName.Assign((char16_t *)appPath);

  rv = ResetCurrent();
  NS_ENSURE_SUCCESS(rv, rv);

  return NS_OK;
}

NS_IMETHODIMP
nsMessengerWinIntegration::OnItemPropertyChanged(nsIMsgFolder *,
                                                 const nsACString &,
                                                 const nsACString &,
                                                 const nsACString &) {
  return NS_OK;
}

NS_IMETHODIMP
nsMessengerWinIntegration::OnItemUnicharPropertyChanged(nsIMsgFolder *,
                                                        const nsACString &,
                                                        const nsAString &,
                                                        const nsAString &) {
  return NS_OK;
}

NS_IMETHODIMP
nsMessengerWinIntegration::OnItemRemoved(nsIMsgFolder *, nsISupports *) {
  return NS_OK;
}

nsresult nsMessengerWinIntegration::GetStringBundle(nsIStringBundle **aBundle) {
  NS_ENSURE_ARG_POINTER(aBundle);
  nsCOMPtr<nsIStringBundleService> bundleService =
      mozilla::services::GetStringBundleService();
  NS_ENSURE_TRUE(bundleService, NS_ERROR_UNEXPECTED);
  nsCOMPtr<nsIStringBundle> bundle;
  bundleService->CreateBundle("chrome://messenger/locale/messenger.properties",
                              getter_AddRefs(bundle));
  bundle.forget(aBundle);
  return NS_OK;
}

#ifndef MOZ_THUNDERBIRD
nsresult nsMessengerWinIntegration::ShowAlertMessage(
    const nsString &aAlertTitle, const nsString &aAlertText,
    const nsACString &aFolderURI) {
  nsresult rv;

  // if we are already in the process of showing an alert, don't try to show
  // another....
  if (mAlertInProgress) return NS_OK;

  nsCOMPtr<nsIPrefBranch> prefBranch(
      do_GetService(NS_PREFSERVICE_CONTRACTID, &rv));
  NS_ENSURE_SUCCESS(rv, rv);

  bool showBalloon = false;
  prefBranch->GetBoolPref(SHOW_BALLOON_PREF, &showBalloon);
  sBiffIconData.szInfo[0] = '\0';
  if (showBalloon) {
    ::wcsncpy(sBiffIconData.szInfoTitle, aAlertTitle.get(), kMaxBalloonTitle);
    ::wcsncpy(sBiffIconData.szInfo, aAlertText.get(), kMaxBalloonSize);
  }

  bool showAlert = true;
  prefBranch->GetBoolPref(SHOW_ALERT_PREF, &showAlert);

  if (showAlert) {
    nsCOMPtr<nsIAlertsService> alertsService =
        mozilla::components::Alerts::Service();
    if (alertsService) {
      rv = alertsService->ShowAlertNotification(
          NS_LITERAL_STRING(NEW_MAIL_ALERT_ICON), aAlertTitle, aAlertText, true,
          NS_ConvertASCIItoUTF16(aFolderURI), this, EmptyString(),
          NS_LITERAL_STRING("auto"), EmptyString(), EmptyString(), nullptr,
          false, false);
      mAlertInProgress = true;
    }
  }

  if (!showAlert ||
      NS_FAILED(rv))  // go straight to showing the system tray icon.
    AlertFinished();

  return rv;
}
#endif
// Opening Thunderbird's new mail alert notification window
// aUserInitiated --> true if we are opening the alert notification in response
//                    to a user action like clicking on the biff icon
nsresult nsMessengerWinIntegration::ShowNewAlertNotification(
    bool aUserInitiated, const nsString &aAlertTitle,
    const nsString &aAlertText) {
  nsresult rv;

  // if we are already in the process of showing an alert, don't try to show
  // another....
  if (mAlertInProgress) return NS_OK;

  nsCOMPtr<nsIPrefBranch> prefBranch(
      do_GetService(NS_PREFSERVICE_CONTRACTID, &rv));
  NS_ENSURE_SUCCESS(rv, rv);

  bool showBalloon = false;
  prefBranch->GetBoolPref(SHOW_BALLOON_PREF, &showBalloon);
  sBiffIconData.szInfo[0] = '\0';
  if (showBalloon) {
    ::wcsncpy(sBiffIconData.szInfoTitle, aAlertTitle.get(), kMaxBalloonTitle);
    ::wcsncpy(sBiffIconData.szInfo, aAlertText.get(), kMaxBalloonSize);
  }

  bool showAlert = true;

  if (prefBranch) prefBranch->GetBoolPref(SHOW_ALERT_PREF, &showAlert);

  // check if we are allowed to show a notification
  if (showAlert) {
    QUERY_USER_NOTIFICATION_STATE qstate;

    if (SUCCEEDED(SHQueryUserNotificationState(&qstate))) {
      if (qstate != QUNS_ACCEPTS_NOTIFICATIONS) {
        showAlert = false;
      }
    }
  }

  if (showAlert) {
    nsCOMPtr<nsIMutableArray> argsArray(
        do_CreateInstance(NS_ARRAY_CONTRACTID, &rv));
    NS_ENSURE_SUCCESS(rv, rv);

    // pass in the array of folders with unread messages
    nsCOMPtr<nsISupportsInterfacePointer> ifptr =
        do_CreateInstance(NS_SUPPORTS_INTERFACE_POINTER_CONTRACTID, &rv);
    NS_ENSURE_SUCCESS(rv, rv);
    ifptr->SetData(mFoldersWithNewMail);
    ifptr->SetDataIID(&NS_GET_IID(nsIArray));
    rv = argsArray->AppendElement(ifptr);
    NS_ENSURE_SUCCESS(rv, rv);

    // pass in the observer
    ifptr = do_CreateInstance(NS_SUPPORTS_INTERFACE_POINTER_CONTRACTID, &rv);
    NS_ENSURE_SUCCESS(rv, rv);
    nsCOMPtr<nsISupports> supports =
        do_QueryInterface(static_cast<nsIMessengerOSIntegration *>(this));
    ifptr->SetData(supports);
    ifptr->SetDataIID(&NS_GET_IID(nsIObserver));
    rv = argsArray->AppendElement(ifptr);
    NS_ENSURE_SUCCESS(rv, rv);

    // pass in the animation flag
    nsCOMPtr<nsISupportsPRBool> scriptableUserInitiated(
        do_CreateInstance(NS_SUPPORTS_PRBOOL_CONTRACTID, &rv));
    NS_ENSURE_SUCCESS(rv, rv);
    scriptableUserInitiated->SetData(aUserInitiated);
    rv = argsArray->AppendElement(scriptableUserInitiated);
    NS_ENSURE_SUCCESS(rv, rv);

    // pass in the alert origin
    nsCOMPtr<nsISupportsPRUint8> scriptableOrigin(
        do_CreateInstance(NS_SUPPORTS_PRUINT8_CONTRACTID));
    NS_ENSURE_TRUE(scriptableOrigin, NS_ERROR_FAILURE);
    scriptableOrigin->SetData(0);
    int32_t origin = 0;
    origin = LookAndFeel::GetInt(LookAndFeel::eIntID_AlertNotificationOrigin);
    scriptableOrigin->SetData(origin);

    rv = argsArray->AppendElement(scriptableOrigin);
    NS_ENSURE_SUCCESS(rv, rv);

    nsCOMPtr<nsIWindowWatcher> wwatch(
        do_GetService(NS_WINDOWWATCHER_CONTRACTID));
    nsCOMPtr<mozIDOMWindowProxy> newWindow;
    rv = wwatch->OpenWindow(0, ALERT_CHROME_URL, "_blank",
                            "chrome,dialog=yes,titlebar=no,popup=yes",
                            argsArray, getter_AddRefs(newWindow));

    mAlertInProgress = true;
  }

  // if the user has turned off the mail alert, or  openWindow generated an
  // error, then go straight to the system tray.
  if (!showAlert || NS_FAILED(rv)) AlertFinished();

  return rv;
}

nsresult nsMessengerWinIntegration::AlertFinished() {
  // okay, we are done showing the alert
  // now put an icon in the system tray, if allowed
  bool showTrayIcon = !mSuppressBiffIcon || sBiffIconData.szInfo[0];
  if (showTrayIcon) {
    nsCOMPtr<nsIPrefBranch> prefBranch(
        do_GetService(NS_PREFSERVICE_CONTRACTID));
    if (prefBranch) prefBranch->GetBoolPref(SHOW_TRAY_ICON_PREF, &showTrayIcon);
  }
  if (showTrayIcon) {
    GenericShellNotify(NIM_ADD);
    mBiffIconVisible = true;
  }
  mSuppressBiffIcon = false;
  mAlertInProgress = false;
  return NS_OK;
}

nsresult nsMessengerWinIntegration::AlertClicked() {
  nsresult rv;
  nsCOMPtr<nsIMsgMailSession> mailSession =
      do_GetService(NS_MSGMAILSESSION_CONTRACTID, &rv);
  NS_ENSURE_SUCCESS(rv, rv);
  nsCOMPtr<nsIMsgWindow> topMostMsgWindow;
  rv = mailSession->GetTopmostMsgWindow(getter_AddRefs(topMostMsgWindow));
  if (topMostMsgWindow) {
    nsCOMPtr<mozIDOMWindowProxy> domWindow;
    topMostMsgWindow->GetDomWindow(getter_AddRefs(domWindow));
    if (domWindow) {
      activateWindow(domWindow);
      return NS_OK;
    }
  }
  // make sure we don't insert the icon in the system tray since the user
  // clicked on the alert.
  mSuppressBiffIcon = true;
  nsCString folderURI;
  GetFirstFolderWithNewMail(folderURI);
  openMailWindow(folderURI);
  return NS_OK;
}

#ifdef MOZ_SUITE
nsresult nsMessengerWinIntegration::AlertClickedSimple() {
  mSuppressBiffIcon = true;
  return NS_OK;
}
#endif

NS_IMETHODIMP
nsMessengerWinIntegration::Observe(nsISupports *aSubject, const char *aTopic,
                                   const char16_t *aData) {
  if (strcmp(aTopic, "alertfinished") == 0) return AlertFinished();

  if (strcmp(aTopic, "alertclickcallback") == 0) return AlertClicked();

#ifdef MOZ_SUITE
  // SeaMonkey does most of the GUI work in JS code when clicking on a mail
  // notification, so it needs an extra function here
  if (strcmp(aTopic, "alertclicksimplecallback") == 0)
    return AlertClickedSimple();
#endif

  return NS_OK;
}

static void EscapeAmpersands(nsString &aToolTip) {
  // First, check to see whether we have any ampersands.
  int32_t pos = aToolTip.FindChar('&');
  if (pos == kNotFound) return;

  // Next, see if we only have bare ampersands.
  pos = aToolTip.Find("&&", false, pos);

  // Windows tooltip code removes one ampersand from each run,
  // then collapses pairs of amperands. This means that in the easy case,
  // we need to replace each ampersand with three.
  aToolTip.ReplaceSubstring(NS_LITERAL_STRING("&"), NS_LITERAL_STRING("&&&"));
  if (pos == kNotFound) return;

  // We inserted too many ampersands. Remove some.
  for (;;) {
    pos = aToolTip.Find("&&&&&&", false, pos);
    if (pos == kNotFound) return;

    aToolTip.Cut(pos, 1);
    pos += 2;
  }
}

void nsMessengerWinIntegration::FillToolTipInfo() {
  // iterate over all the folders in mFoldersWithNewMail
  nsString accountName;
  nsCString hostName;
  nsString toolTipLine;
  nsAutoString toolTipText;
  nsAutoString animatedAlertText;
  nsCOMPtr<nsIMsgFolder> folder;
  nsWeakPtr weakReference;
  int32_t numNewMessages = 0;

  uint32_t count = 0;
  NS_ENSURE_SUCCESS_VOID(mFoldersWithNewMail->GetLength(&count));

  for (uint32_t index = 0; index < count; index++) {
    weakReference = do_QueryElementAt(mFoldersWithNewMail, index);
    folder = do_QueryReferent(weakReference);
    if (folder) {
      folder->GetPrettyName(accountName);

      numNewMessages = 0;
      folder->GetNumNewMessages(true, &numNewMessages);
      nsCOMPtr<nsIStringBundle> bundle;
      GetStringBundle(getter_AddRefs(bundle));
      if (bundle) {
        nsAutoString numNewMsgsText;
        numNewMsgsText.AppendInt(numNewMessages);

        AutoTArray<nsString, 1> formatStrings = {numNewMsgsText};

        nsString finalText;
        if (numNewMessages == 1)
          bundle->FormatStringFromName("biffNotification_message",
                                       formatStrings, finalText);
        else
          bundle->FormatStringFromName("biffNotification_messages",
                                       formatStrings, finalText);

        // the alert message is special...we actually only want to show the
        // first account with new mail in the alert.
        if (animatedAlertText.IsEmpty())  // if we haven't filled in the
                                          // animated alert text yet
          animatedAlertText = finalText;

        toolTipLine.Append(accountName);
        toolTipLine.Append(' ');
        toolTipLine.Append(finalText);
        EscapeAmpersands(toolTipLine);

        // only add this new string if it will fit without truncation....
        if (toolTipLine.Length() + toolTipText.Length() <= kMaxTooltipSize)
          toolTipText.Append(toolTipLine);

        // clear out the tooltip line for the next folder
        toolTipLine.Assign('\n');
      }  // if we got a bundle
    }    // if we got a folder
  }      // for each folder

  ::wcsncpy(sBiffIconData.szTip, toolTipText.get(), kMaxTooltipSize);

  if (!mBiffIconVisible) {
#ifndef MOZ_THUNDERBIRD
    nsresult rv;
    bool showNewAlert = false;
    nsCOMPtr<nsIPrefBranch> prefBranch(
        do_GetService(NS_PREFSERVICE_CONTRACTID, &rv));
    NS_ENSURE_SUCCESS_VOID(rv);

    prefBranch->GetBoolPref(SHOW_NEW_ALERT_PREF, &showNewAlert);
    if (!showNewAlert)
      ShowAlertMessage(accountName, animatedAlertText, EmptyCString());
    else
#endif
      ShowNewAlertNotification(false, accountName, animatedAlertText);
  } else
    GenericShellNotify(NIM_MODIFY);
}

// Get the first top level folder which we know has new mail, then enumerate
// over all the subfolders looking for the first real folder with new mail.
// Return the folderURI for that folder.
nsresult nsMessengerWinIntegration::GetFirstFolderWithNewMail(
    nsACString &aFolderURI) {
  NS_ENSURE_TRUE(mFoldersWithNewMail, NS_ERROR_FAILURE);

  nsCOMPtr<nsIMsgFolder> folder;
  nsWeakPtr weakReference;
  int32_t numNewMessages = 0;

  uint32_t count = 0;
  nsresult rv = mFoldersWithNewMail->GetLength(&count);
  // kick out if we don't have any folders with new mail
  if (NS_FAILED(rv) || !count) return NS_OK;

  weakReference = do_QueryElementAt(mFoldersWithNewMail, 0);
  folder = do_QueryReferent(weakReference);

  if (folder) {
    // enumerate over the folders under this root folder till we find one with
    // new mail....
    nsTArray<RefPtr<nsIMsgFolder>> allFolders;
    rv = folder->GetDescendants(allFolders);
    NS_ENSURE_SUCCESS(rv, rv);

    for (auto msgFolder : allFolders) {
      numNewMessages = 0;
      msgFolder->GetNumNewMessages(false, &numNewMessages);
      if (numNewMessages) {
        msgFolder->GetURI(aFolderURI);
        return NS_OK;
      }
    }
  }
  return NS_OK;
}

void nsMessengerWinIntegration::DestroyBiffIcon() {
  GenericShellNotify(NIM_DELETE);
  // Don't call DestroyIcon().
  // See http://bugzilla.mozilla.org/show_bug.cgi?id=134745
}

void nsMessengerWinIntegration::GenericShellNotify(DWORD aMessage) {
  ::Shell_NotifyIconW(aMessage, &sBiffIconData);
}

NS_IMETHODIMP
nsMessengerWinIntegration::OnItemPropertyFlagChanged(nsIMsgDBHdr *item,
                                                     const nsACString &property,
                                                     uint32_t oldFlag,
                                                     uint32_t newFlag) {
  return NS_OK;
}

NS_IMETHODIMP
nsMessengerWinIntegration::OnItemAdded(nsIMsgFolder *, nsISupports *) {
  return NS_OK;
}

NS_IMETHODIMP
nsMessengerWinIntegration::OnItemBoolPropertyChanged(
    nsIMsgFolder *aItem, const nsACString &aProperty, bool aOldValue,
    bool aNewValue) {
  if (aProperty.Equals(kDefaultServer)) {
    nsresult rv;

    // this property changes multiple times
    // on account deletion or when the user changes their
    // default account.  ResetCurrent() will set
    // mInboxURI to null, so we use that
    // to prevent us from attempting to remove
    // something from the registry that has already been removed
    if (!mInboxURI.IsEmpty() && !mEmail.IsEmpty()) {
      rv = RemoveCurrentFromRegistry();
      NS_ENSURE_SUCCESS(rv, rv);
    }

    // reset so we'll go get the new default server next time
    rv = ResetCurrent();
    NS_ENSURE_SUCCESS(rv, rv);

    rv = UpdateUnreadCount();
    NS_ENSURE_SUCCESS(rv, rv);
  }
  return NS_OK;
}

NS_IMETHODIMP
nsMessengerWinIntegration::OnItemEvent(nsIMsgFolder *, const nsACString &) {
  return NS_OK;
}

NS_IMETHODIMP
nsMessengerWinIntegration::OnItemIntPropertyChanged(nsIMsgFolder *aItem,
                                                    const nsACString &aProperty,
                                                    int64_t aOldValue,
                                                    int64_t aNewValue) {
  // if we got new mail show a icon in the system tray
  if (aProperty.Equals(kBiffState) && mFoldersWithNewMail) {
    nsWeakPtr weakFolder = do_GetWeakReference(aItem);
    uint32_t indexInNewArray;
    nsresult rv = mFoldersWithNewMail->IndexOf(0, weakFolder, &indexInNewArray);
    bool folderFound = NS_SUCCEEDED(rv);

    if (!mBiffIconInitialized) InitializeBiffStatusIcon();

    if (aNewValue == nsIMsgFolder::nsMsgBiffState_NewMail) {
      // if the icon is not already visible, only show a system tray icon iff
      // we are performing biff (as opposed to the user getting new mail)
      if (!mBiffIconVisible) {
        bool performingBiff = false;
        nsCOMPtr<nsIMsgIncomingServer> server;
        aItem->GetServer(getter_AddRefs(server));
        if (server) server->GetPerformingBiff(&performingBiff);
        if (!performingBiff) return NS_OK;  // kick out right now...
      }
      if (!folderFound) mFoldersWithNewMail->InsertElementAt(weakFolder, 0);
      // now regenerate the tooltip
      FillToolTipInfo();
    } else if (aNewValue == nsIMsgFolder::nsMsgBiffState_NoMail) {
      // we are always going to remove the icon whenever we get our first no
      // mail notification.

      // avoid a race condition where we are told to remove the icon before
      // we've actually added it to the system tray. This happens when the user
      // reads a new message before the animated alert has gone away.
      if (mAlertInProgress) mSuppressBiffIcon = true;

      if (folderFound) mFoldersWithNewMail->RemoveElementAt(indexInNewArray);
      if (mBiffIconVisible) {
        mBiffIconVisible = false;
        GenericShellNotify(NIM_DELETE);
      }
    }
  }  // if the biff property changed

  if (aProperty.Equals(kTotalUnreadMessages)) {
    nsCString itemURI;
    nsresult rv;
    rv = aItem->GetURI(itemURI);
    NS_ENSURE_SUCCESS(rv, rv);

    if (mInboxURI.Equals(itemURI)) mCurrentUnreadCount = aNewValue;

    // If the timer isn't running yet, then we immediately update the
    // registry and then start a one-shot timer. If the Unread counter
    // has toggled zero / nonzero, we also update immediately.
    // Otherwise, if the timer is running, defer the update. This means
    // that all counter updates that occur within the timer interval are
    // batched into a single registry update, to avoid hitting the
    // registry too frequently. We also do a final update on shutdown,
    // regardless of the timer.
    if (!mUnreadTimerActive ||
        (!mCurrentUnreadCount && mLastUnreadCountWrittenToRegistry) ||
        (mCurrentUnreadCount && mLastUnreadCountWrittenToRegistry < 1)) {
      rv = UpdateUnreadCount();
      NS_ENSURE_SUCCESS(rv, rv);
      // If timer wasn't running, start it.
      if (!mUnreadTimerActive) rv = SetupUnreadCountUpdateTimer();
    }
  }
  return NS_OK;
}

void nsMessengerWinIntegration::OnUnreadCountUpdateTimer(nsITimer *timer,
                                                         void *osIntegration) {
  nsMessengerWinIntegration *winIntegration =
      (nsMessengerWinIntegration *)osIntegration;

  winIntegration->mUnreadTimerActive = false;
  mozilla::DebugOnly<nsresult> rv = winIntegration->UpdateUnreadCount();
  NS_ASSERTION(NS_SUCCEEDED(rv), "updating unread count failed");
}

nsresult nsMessengerWinIntegration::RemoveCurrentFromRegistry() {
  // If Windows XP, open the registry and get rid of old account registry
  // entries If there is a email prefix, get it and use it to build the registry
  // key. Otherwise, just the email address will be the registry key.
  nsAutoString currentUnreadMailCountKey;
  if (!mEmailPrefix.IsEmpty()) {
    currentUnreadMailCountKey.Assign(mEmailPrefix);
    currentUnreadMailCountKey.Append(NS_ConvertASCIItoUTF16(mEmail));
  } else
    CopyASCIItoUTF16(mEmail, currentUnreadMailCountKey);

  WCHAR registryUnreadMailCountKey[_MAX_PATH] = {0};
  // Enumerate through registry entries to delete the key matching
  // currentUnreadMailCountKey
  int index = 0;
  while (SUCCEEDED(SHEnumerateUnreadMailAccountsW(
      HKEY_CURRENT_USER, index, registryUnreadMailCountKey,
      sizeof(registryUnreadMailCountKey)))) {
    if (wcscmp(registryUnreadMailCountKey, currentUnreadMailCountKey.get()) ==
        0) {
      nsAutoString deleteKey;
      deleteKey.Assign(UNREADMAILNODEKEY);
      deleteKey.Append(currentUnreadMailCountKey.get());

      if (!deleteKey.IsEmpty()) {
        // delete this key and break out of the loop
        RegDeleteKey(HKEY_CURRENT_USER, deleteKey.get());
        break;
      } else {
        index++;
      }
    } else {
      index++;
    }
  }
  return NS_OK;
}

nsresult nsMessengerWinIntegration::UpdateRegistryWithCurrent() {
  if (mInboxURI.IsEmpty() || mEmail.IsEmpty()) return NS_OK;

  // only update the registry if the count has changed
  // and if the unread count is valid
  if ((mCurrentUnreadCount < 0) ||
      (mCurrentUnreadCount == mLastUnreadCountWrittenToRegistry))
    return NS_OK;

  // commandliner has to be built in the form of statement
  // which can be open the mailer app to the default user account
  // For given profile 'foo', commandliner will be built as
  // ""<absolute path to application>" -p foo -mail" where absolute
  // path to application is extracted from mAppName
  nsAutoString commandLinerForAppLaunch;
  commandLinerForAppLaunch.Assign(DOUBLE_QUOTE);
  commandLinerForAppLaunch.Append(mAppName);
  commandLinerForAppLaunch.Append(DOUBLE_QUOTE);
  commandLinerForAppLaunch.AppendLiteral(PROFILE_COMMANDLINE_ARG);
  commandLinerForAppLaunch.Append(DOUBLE_QUOTE);
  commandLinerForAppLaunch.Append(mProfilePath);
  commandLinerForAppLaunch.Append(DOUBLE_QUOTE);
  commandLinerForAppLaunch.AppendLiteral(MAIL_COMMANDLINE_ARG);

  if (!commandLinerForAppLaunch.IsEmpty()) {
    nsAutoString pBuffer;

    if (!mEmailPrefix.IsEmpty()) {
      pBuffer.Assign(mEmailPrefix);
      pBuffer.Append(NS_ConvertASCIItoUTF16(mEmail));
    } else
      CopyASCIItoUTF16(mEmail, pBuffer);

    // Write the info into the registry
    SHSetUnreadMailCountW(pBuffer.get(), mCurrentUnreadCount,
                          commandLinerForAppLaunch.get());
  }

  // do this last
  mLastUnreadCountWrittenToRegistry = mCurrentUnreadCount;

  return NS_OK;
}

nsresult nsMessengerWinIntegration::SetupInbox() {
  nsresult rv;

  // get default account
  nsCOMPtr<nsIMsgAccountManager> accountManager =
      do_GetService(NS_MSGACCOUNTMANAGER_CONTRACTID, &rv);
  NS_ENSURE_SUCCESS(rv, rv);

  nsCOMPtr<nsIMsgAccount> account;
  rv = accountManager->GetDefaultAccount(getter_AddRefs(account));
  NS_ENSURE_SUCCESS(rv, rv);
  if (!account) {
    // this can happen if we launch mail on a new profile
    // we don't have a default account yet
    mDefaultAccountMightHaveAnInbox = false;
    return NS_OK;
  }

  // get incoming server
  nsCOMPtr<nsIMsgIncomingServer> server;
  rv = account->GetIncomingServer(getter_AddRefs(server));
  NS_ENSURE_SUCCESS(rv, rv);
  if (!server) return NS_ERROR_FAILURE;

  nsCString type;
  rv = server->GetType(type);
  NS_ENSURE_SUCCESS(rv, rv);

  // we only care about imap and pop3
  if (type.EqualsLiteral("imap") || type.EqualsLiteral("pop3")) {
    // imap and pop3 account should have an Inbox
    mDefaultAccountMightHaveAnInbox = true;

    mEmailPrefix.Truncate();

    // Get user's email address
    nsCOMPtr<nsIMsgIdentity> identity;
    rv = account->GetDefaultIdentity(getter_AddRefs(identity));
    NS_ENSURE_SUCCESS(rv, rv);

    if (!identity) return NS_ERROR_FAILURE;

    rv = identity->GetEmail(mEmail);
    NS_ENSURE_SUCCESS(rv, rv);

    nsCOMPtr<nsIMsgFolder> rootMsgFolder;
    rv = server->GetRootMsgFolder(getter_AddRefs(rootMsgFolder));
    NS_ENSURE_SUCCESS(rv, rv);

    if (!rootMsgFolder) return NS_ERROR_FAILURE;

    nsCOMPtr<nsIMsgFolder> inboxFolder;
    rootMsgFolder->GetFolderWithFlags(nsMsgFolderFlags::Inbox,
                                      getter_AddRefs(inboxFolder));
    NS_ENSURE_TRUE(inboxFolder, NS_ERROR_FAILURE);

    rv = inboxFolder->GetURI(mInboxURI);
    NS_ENSURE_SUCCESS(rv, rv);

    rv = inboxFolder->GetNumUnread(false, &mCurrentUnreadCount);
    NS_ENSURE_SUCCESS(rv, rv);
  } else {
    // the default account is valid, but it's not something
    // that we expect to have an inbox.  (local folders, news accounts)
    // set this flag to avoid calling SetupInbox() every time
    // the timer goes off.
    mDefaultAccountMightHaveAnInbox = false;
  }

  return NS_OK;
}

nsresult nsMessengerWinIntegration::UpdateUnreadCount() {
  nsresult rv;

  if (mDefaultAccountMightHaveAnInbox && mInboxURI.IsEmpty()) {
    rv = SetupInbox();
    NS_ENSURE_SUCCESS(rv, rv);
  }

  return UpdateRegistryWithCurrent();
}

nsresult nsMessengerWinIntegration::SetupUnreadCountUpdateTimer() {
  mUnreadTimerActive = true;
  if (mUnreadCountUpdateTimer)
    mUnreadCountUpdateTimer->Cancel();
  else
    mUnreadCountUpdateTimer = do_CreateInstance("@mozilla.org/timer;1");

  mUnreadCountUpdateTimer->InitWithNamedFuncCallback(
      OnUnreadCountUpdateTimer, (void *)this, UNREAD_UPDATE_INTERVAL,
      nsITimer::TYPE_ONE_SHOT,
      "nsMessengerWinIntegration::OnUnreadCountUpdateTimer");

  return NS_OK;
}

NOTIFYICONDATAW sMailIconData = {
    /* cbSize */ (DWORD)NOTIFYICONDATAW_V2_SIZE,
    /* hWnd */ 0,
    /* uID */ 2,
    /* uFlags */ NIF_ICON | NIF_MESSAGE | NIF_TIP | NIF_INFO,
    /* uCallbackMessage */ WM_USER,
    /* hIcon */ 0,
    /* szTip */ L"",
    /* dwState */ 0,
    /* dwStateMask */ 0,
    /* szInfo */ L"",
    /* uVersion */ {30000},
    /* szInfoTitle */ L"",
    /* dwInfoFlags */ NIIF_USER | NIIF_NOSOUND};

static nsCOMArray<nsIBaseWindow> sHiddenWindows;
static HWND sIconWindow;
static LRESULT CALLBACK IconWindowProc(HWND msgWindow, UINT msg, WPARAM wp,
                                       LPARAM lp) {
  if (msg == WM_USER && lp == WM_LBUTTONDOWN) {
    ::Shell_NotifyIconW(NIM_DELETE, &sMailIconData);

    uint32_t count = sHiddenWindows.Length();
    for (uint32_t i = 0; i < count; i++) {
      sHiddenWindows[i]->SetVisibility(true);

      nsCOMPtr<nsIWidget> widget;
      sHiddenWindows[i]->GetMainWidget(getter_AddRefs(widget));

      HWND hwnd = (HWND)(widget->GetNativeData(NS_NATIVE_WIDGET));
      ::ShowWindow(hwnd, SW_RESTORE);
      ::SetForegroundWindow(hwnd);
    }

    sHiddenWindows.Clear();
  }
  return TRUE;
}

WNDCLASS sClassStruct = {
    /* style */ 0,
    /* lpfnWndProc */ &IconWindowProc,
    /* cbClsExtra */ 0,
    /* cbWndExtra */ 0,
    /* hInstance */ 0,
    /* hIcon */ 0,
    /* hCursor */ 0,
    /* hbrBackground */ 0,
    /* lpszMenuName */ 0,
    /* lpszClassName */ L"IconWindowClass"};

nsresult nsMessengerWinIntegration::HideWindow(nsIBaseWindow *aWindow) {
  aWindow->SetVisibility(false);
  sHiddenWindows.AppendElement(aWindow);

  if (sMailIconData.hWnd == 0) {
    // Register the window class.
    NS_ENSURE_TRUE(::RegisterClass(&sClassStruct), NS_ERROR_FAILURE);
    // Create the window.
    NS_ENSURE_TRUE(sIconWindow = ::CreateWindow(
                       /* className */ L"IconWindowClass",
                       /* title */ 0,
                       /* style */ WS_CAPTION,
                       /* x, y, cx, cy */ 0, 0, 0, 0,
                       /* parent */ 0,
                       /* menu */ 0,
                       /* instance */ 0,
                       /* create struct */ 0),
                   NS_ERROR_FAILURE);
    sMailIconData.hWnd = sIconWindow;
    sMailIconData.hIcon =
        ::LoadIcon(::GetModuleHandle(NULL), MAKEINTRESOURCE(IDI_APPLICATION));

    nsCOMPtr<nsIStringBundleService> bundleService =
        mozilla::services::GetStringBundleService();
    NS_ENSURE_TRUE(bundleService, NS_ERROR_UNEXPECTED);
    nsCOMPtr<nsIStringBundle> bundle;
    bundleService->CreateBundle("chrome://branding/locale/brand.properties",
                                getter_AddRefs(bundle));
    nsString brandShortName;
    bundle->GetStringFromName("brandShortName", brandShortName);
    ::wcsncpy(sMailIconData.szTip, brandShortName.get(),
              brandShortName.Length());
  }

  ::Shell_NotifyIconW(NIM_ADD, &sMailIconData);
  ::Shell_NotifyIconW(NIM_SETVERSION, &sMailIconData);
  return NS_OK;
}
