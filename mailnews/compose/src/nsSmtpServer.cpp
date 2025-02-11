/* -*- Mode: C++; tab-width: 2; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "nsIPrefService.h"
#include "nsIPrefBranch.h"
#include "nsSmtpServer.h"
#include "nsNetUtil.h"
#include "nsIAuthPrompt.h"
#include "nsMsgUtils.h"
#include "nsIMsgAccountManager.h"
#include "nsMsgBaseCID.h"
#include "nsISmtpService.h"
#include "nsMsgCompCID.h"
#include "nsILoginInfo.h"
#include "nsILoginManager.h"
#include "nsIArray.h"
#include "nsArrayUtils.h"
#include "nsMemory.h"
#include "nsIObserverService.h"

NS_IMPL_ADDREF(nsSmtpServer)
NS_IMPL_RELEASE(nsSmtpServer)
NS_INTERFACE_MAP_BEGIN(nsSmtpServer)
  NS_INTERFACE_MAP_ENTRY(nsISmtpServer)
  NS_INTERFACE_MAP_ENTRY(nsISupportsWeakReference)
  NS_INTERFACE_MAP_ENTRY(nsIObserver)
  NS_INTERFACE_MAP_ENTRY_AMBIGUOUS(nsISupports, nsISmtpServer)
NS_INTERFACE_MAP_END

nsSmtpServer::nsSmtpServer() : mKey("") {
  m_logonFailed = false;
  getPrefs();
}

nsSmtpServer::~nsSmtpServer() {}

nsresult nsSmtpServer::Init() {
  // We need to know when the password manager changes.
  nsCOMPtr<nsIObserverService> observerService =
      mozilla::services::GetObserverService();
  NS_ENSURE_TRUE(observerService, NS_ERROR_UNEXPECTED);

  observerService->AddObserver(this, "passwordmgr-storage-changed", false);
  observerService->AddObserver(this, NS_XPCOM_SHUTDOWN_OBSERVER_ID, false);

  return NS_OK;
}

NS_IMETHODIMP
nsSmtpServer::Observe(nsISupports *aSubject, const char *aTopic,
                      const char16_t *aData) {
  // When the state of the password manager changes we need to clear the
  // password from the cache in case the user just removed it.
  if (strcmp(aTopic, "passwordmgr-storage-changed") == 0) {
    m_password.Truncate();
  } else if (strcmp(aTopic, NS_XPCOM_SHUTDOWN_OBSERVER_ID) == 0) {
    // Now remove ourselves from the observer service as well.
    nsCOMPtr<nsIObserverService> observerService =
        mozilla::services::GetObserverService();
    NS_ENSURE_TRUE(observerService, NS_ERROR_UNEXPECTED);

    observerService->RemoveObserver(this, "passwordmgr-storage-changed");
    observerService->RemoveObserver(this, NS_XPCOM_SHUTDOWN_OBSERVER_ID);
  }

  return NS_OK;
}

NS_IMETHODIMP
nsSmtpServer::GetKey(char **aKey) {
  if (!aKey) return NS_ERROR_NULL_POINTER;
  if (mKey.IsEmpty())
    *aKey = nullptr;
  else
    *aKey = ToNewCString(mKey);
  return NS_OK;
}

NS_IMETHODIMP
nsSmtpServer::SetKey(const char *aKey) {
  NS_ASSERTION(aKey, "Bad key pointer");
  mKey = aKey;
  return getPrefs();
}

nsresult nsSmtpServer::getPrefs() {
  nsresult rv;
  nsCOMPtr<nsIPrefService> prefs(do_GetService(NS_PREFSERVICE_CONTRACTID, &rv));
  if (NS_FAILED(rv)) return rv;

  nsAutoCString branchName;
  branchName.AssignLiteral("mail.smtpserver.");
  branchName += mKey;
  branchName.Append('.');
  rv = prefs->GetBranch(branchName.get(), getter_AddRefs(mPrefBranch));
  if (NS_FAILED(rv)) return rv;

  if (!mDefPrefBranch) {
    branchName.AssignLiteral("mail.smtpserver.default.");
    rv = prefs->GetBranch(branchName.get(), getter_AddRefs(mDefPrefBranch));
    if (NS_FAILED(rv)) return rv;
  }

  return NS_OK;
}

// This function is intentionally called the same as in nsIMsgIncomingServer.
nsresult nsSmtpServer::OnUserOrHostNameChanged(const nsACString &oldName,
                                               const nsACString &newName,
                                               bool hostnameChanged) {
  // Reset password so that users are prompted for new password for the new
  // user/host.
  (void)ForgetPassword();

  return NS_OK;
}

NS_IMETHODIMP
nsSmtpServer::GetHostname(nsACString &aHostname) {
  nsCString result;
  nsresult rv = mPrefBranch->GetCharPref("hostname", result);
  if (NS_FAILED(rv))
    aHostname.Truncate();
  else
    aHostname = result;

  return NS_OK;
}

NS_IMETHODIMP
nsSmtpServer::SetHostname(const nsACString &aHostname) {
  nsCString oldName;
  nsresult rv = GetHostname(oldName);
  NS_ENSURE_SUCCESS(rv, rv);

  // A few things to take care of if we're changing the hostname.
  if (!oldName.Equals(aHostname, nsCaseInsensitiveCStringComparator)) {
    rv = OnUserOrHostNameChanged(oldName, aHostname, true);
    NS_ENSURE_SUCCESS(rv, rv);
  }

  if (!aHostname.IsEmpty())
    return mPrefBranch->SetCharPref("hostname", aHostname);

  // If the pref value is already empty, ClearUserPref will return
  // NS_ERROR_UNEXPECTED, so don't check the rv here.
  (void)mPrefBranch->ClearUserPref("hostname");
  return NS_OK;
}

NS_IMETHODIMP
nsSmtpServer::GetDescription(nsACString &aDescription) {
  nsCString temp;
  mPrefBranch->GetCharPref("description", temp);
  aDescription.Assign(temp);
  return NS_OK;
}

NS_IMETHODIMP
nsSmtpServer::SetDescription(const nsACString &aDescription) {
  if (!aDescription.IsEmpty())
    return mPrefBranch->SetCharPref("description", aDescription);
  else
    mPrefBranch->ClearUserPref("description");
  return NS_OK;
}

// if GetPort returns 0, it means default port
NS_IMETHODIMP
nsSmtpServer::GetPort(int32_t *aPort) {
  NS_ENSURE_ARG_POINTER(aPort);
  if (NS_FAILED(mPrefBranch->GetIntPref("port", aPort))) *aPort = 0;
  return NS_OK;
}

NS_IMETHODIMP
nsSmtpServer::SetPort(int32_t aPort) {
  if (aPort) return mPrefBranch->SetIntPref("port", aPort);

  mPrefBranch->ClearUserPref("port");
  return NS_OK;
}

NS_IMETHODIMP
nsSmtpServer::GetDisplayname(char **aDisplayname) {
  nsresult rv;
  NS_ENSURE_ARG_POINTER(aDisplayname);

  nsCString hostname;
  rv = mPrefBranch->GetCharPref("hostname", hostname);
  if (NS_FAILED(rv)) {
    *aDisplayname = nullptr;
    return NS_OK;
  }
  int32_t port;
  rv = mPrefBranch->GetIntPref("port", &port);
  if (NS_FAILED(rv)) port = 0;

  if (port) {
    hostname.Append(':');
    hostname.AppendInt(port);
  }

  *aDisplayname = ToNewCString(hostname);
  return NS_OK;
}

NS_IMETHODIMP
nsSmtpServer::GetSocketType(int32_t *socketType) {
  NS_ENSURE_ARG_POINTER(socketType);
  getIntPrefWithDefault("try_ssl", socketType, 0);
  return NS_OK;
}

NS_IMETHODIMP
nsSmtpServer::SetSocketType(int32_t socketType) {
  return mPrefBranch->SetIntPref("try_ssl", socketType);
}

NS_IMETHODIMP
nsSmtpServer::GetHelloArgument(nsACString &aHelloArgument) {
  nsresult rv;
  rv = mPrefBranch->GetCharPref("hello_argument", aHelloArgument);
  if (NS_FAILED(rv)) {
    rv = mDefPrefBranch->GetCharPref("hello_argument", aHelloArgument);
    if (NS_FAILED(rv)) aHelloArgument.Truncate();
  }
  return NS_OK;
}

NS_IMETHODIMP
nsSmtpServer::GetAuthMethod(int32_t *authMethod) {
  NS_ENSURE_ARG_POINTER(authMethod);
  getIntPrefWithDefault("authMethod", authMethod, 3);
  return NS_OK;
}

void nsSmtpServer::getIntPrefWithDefault(const char *prefName, int32_t *val,
                                         int32_t defVal) {
  nsresult rv = mPrefBranch->GetIntPref(prefName, val);
  if (NS_SUCCEEDED(rv)) return;

  rv = mDefPrefBranch->GetIntPref(prefName, val);
  if (NS_FAILED(rv))
    // last resort
    *val = defVal;
}

NS_IMETHODIMP
nsSmtpServer::SetAuthMethod(int32_t authMethod) {
  return mPrefBranch->SetIntPref("authMethod", authMethod);
}

NS_IMETHODIMP
nsSmtpServer::GetUsername(nsACString &aUsername) {
  nsCString result;
  nsresult rv = mPrefBranch->GetCharPref("username", result);
  if (NS_FAILED(rv))
    aUsername.Truncate();
  else
    aUsername = result;
  return NS_OK;
}

NS_IMETHODIMP
nsSmtpServer::SetUsername(const nsACString &aUsername) {
  // Need to take care of few things if we're changing the username.
  nsCString oldName;
  nsresult rv = GetUsername(oldName);
  NS_ENSURE_SUCCESS(rv, rv);

  if (!oldName.Equals(aUsername)) {
    rv = OnUserOrHostNameChanged(oldName, aUsername, false);
    NS_ENSURE_SUCCESS(rv, rv);
  }

  if (!aUsername.IsEmpty())
    return mPrefBranch->SetCharPref("username", aUsername);

  // If the pref value is already empty, ClearUserPref will return
  // NS_ERROR_UNEXPECTED, so don't check the rv here.
  (void)mPrefBranch->ClearUserPref("username");
  return NS_OK;
}

NS_IMETHODIMP
nsSmtpServer::GetClientid(nsACString &aClientid) {
  nsresult rv;
  rv = mPrefBranch->GetCharPref("clientid", aClientid);
  if (NS_FAILED(rv)) {
    rv = mDefPrefBranch->GetCharPref("clientid", aClientid);
    if (NS_FAILED(rv)) aClientid.Truncate();
  }
  return NS_OK;
}

NS_IMETHODIMP
nsSmtpServer::SetClientid(const nsACString &aClientid) {
  if (!aClientid.IsEmpty())
    return mPrefBranch->SetCharPref("clientid", aClientid);

  // If the pref value is already empty, ClearUserPref will return
  // NS_ERROR_UNEXPECTED, so don't check the rv here.
  mPrefBranch->ClearUserPref("clientid");
  return NS_OK;
}

NS_IMETHODIMP nsSmtpServer::GetClientidEnabled(bool *aClientidEnabled) {
  NS_ENSURE_ARG_POINTER(aClientidEnabled);
  nsresult rv;
  rv = mPrefBranch->GetBoolPref("clientidEnabled", aClientidEnabled);
  if (NS_FAILED(rv)) {
    rv = mDefPrefBranch->GetBoolPref("clientidEnabled", aClientidEnabled);
    if (NS_FAILED(rv)) *aClientidEnabled = false;
  }
  return NS_OK;
}

NS_IMETHODIMP nsSmtpServer::SetClientidEnabled(bool aClientidEnabled) {
  return mPrefBranch->SetBoolPref("clientidEnabled", aClientidEnabled);
}

NS_IMETHODIMP
nsSmtpServer::GetPassword(nsAString &aPassword) {
  if (m_password.IsEmpty() && !m_logonFailed) {
    // try to avoid prompting the user for another password. If the user has set
    // the appropriate pref, we'll use the password from an incoming server, if
    // the user has already logged onto that server.

    // if this is set, we'll only use this, and not the other prefs
    // user_pref("mail.smtpserver.smtp1.incomingAccount", "server1");

    // if this is set, we'll accept an exact match of user name and server
    // user_pref("mail.smtp.useMatchingHostNameServer", true);

    // if this is set, and we don't find an exact match of user and host name,
    // we'll accept a match of username and domain, where domain
    // is everything after the first '.'
    // user_pref("mail.smtp.useMatchingDomainServer", true);

    nsCString accountKey;
    bool useMatchingHostNameServer = false;
    bool useMatchingDomainServer = false;
    mPrefBranch->GetCharPref("incomingAccount", accountKey);

    nsCOMPtr<nsIMsgAccountManager> accountManager =
        do_GetService(NS_MSGACCOUNTMANAGER_CONTRACTID);
    nsCOMPtr<nsIMsgIncomingServer> incomingServerToUse;
    if (accountManager) {
      if (!accountKey.IsEmpty())
        accountManager->GetIncomingServer(accountKey,
                                          getter_AddRefs(incomingServerToUse));
      else {
        nsresult rv;
        nsCOMPtr<nsIPrefBranch> prefBranch(
            do_GetService(NS_PREFSERVICE_CONTRACTID, &rv));
        NS_ENSURE_SUCCESS(rv, rv);
        prefBranch->GetBoolPref("mail.smtp.useMatchingHostNameServer",
                                &useMatchingHostNameServer);
        prefBranch->GetBoolPref("mail.smtp.useMatchingDomainServer",
                                &useMatchingDomainServer);
        if (useMatchingHostNameServer || useMatchingDomainServer) {
          nsCString userName;
          nsCString hostName;
          GetHostname(hostName);
          GetUsername(userName);
          if (useMatchingHostNameServer)
            // pass in empty type and port=0, to match imap and pop3.
            accountManager->FindRealServer(userName, hostName, EmptyCString(),
                                           0,
                                           getter_AddRefs(incomingServerToUse));
          int32_t dotPos = -1;
          if (!incomingServerToUse && useMatchingDomainServer &&
              (dotPos = hostName.FindChar('.')) != kNotFound) {
            hostName.Cut(0, dotPos);
            nsTArray<RefPtr<nsIMsgIncomingServer>> allServers;
            accountManager->GetAllServers(allServers);
            for (auto server : allServers) {
              if (server) {
                nsCString serverUserName;
                nsCString serverHostName;
                server->GetRealUsername(serverUserName);
                server->GetRealHostName(serverHostName);
                if (serverUserName.Equals(userName)) {
                  int32_t serverDotPos = serverHostName.FindChar('.');
                  if (serverDotPos != kNotFound) {
                    serverHostName.Cut(0, serverDotPos);
                    if (serverHostName.Equals(hostName)) {
                      incomingServerToUse = server;
                      break;
                    }
                  }
                }
              }
            }
          }
        }
      }
    }
    if (incomingServerToUse) return incomingServerToUse->GetPassword(aPassword);
  }
  aPassword = m_password;
  return NS_OK;
}

NS_IMETHODIMP
nsSmtpServer::VerifyLogon(nsIUrlListener *aUrlListener,
                          nsIMsgWindow *aMsgWindow, nsIURI **aURL) {
  nsresult rv;
  nsCOMPtr<nsISmtpService> smtpService(
      do_GetService(NS_SMTPSERVICE_CONTRACTID, &rv));
  NS_ENSURE_SUCCESS(rv, rv);
  return smtpService->VerifyLogon(this, aUrlListener, aMsgWindow, aURL);
}

NS_IMETHODIMP
nsSmtpServer::SetPassword(const nsAString &aPassword) {
  m_password = aPassword;
  return NS_OK;
}

nsresult nsSmtpServer::GetPasswordWithoutUI() {
  nsresult rv;
  nsCOMPtr<nsILoginManager> loginMgr(
      do_GetService(NS_LOGINMANAGER_CONTRACTID, &rv));
  NS_ENSURE_SUCCESS(rv, rv);

  NS_ConvertASCIItoUTF16 serverUri(GetServerURIInternal(false));

  nsTArray<RefPtr<nsILoginInfo>> logins;
  rv = loginMgr->FindLogins(serverUri, EmptyString(), serverUri, logins);
  // Login manager can produce valid fails, e.g. NS_ERROR_ABORT when a user
  // cancels the master password dialog. Therefore handle that here, but don't
  // warn about it.
  if (NS_FAILED(rv)) return rv;
  uint32_t numLogins = logins.Length();

  // Don't abort here, if we didn't find any or failed, then we'll just have
  // to prompt.
  if (numLogins > 0) {
    nsCString serverCUsername;
    rv = GetUsername(serverCUsername);
    NS_ConvertASCIItoUTF16 serverUsername(serverCUsername);

    nsString username;
    for (uint32_t i = 0; i < numLogins; ++i) {
      rv = logins[i]->GetUsername(username);
      NS_ENSURE_SUCCESS(rv, rv);

      if (username.Equals(serverUsername)) {
        nsString password;
        rv = logins[i]->GetPassword(password);
        NS_ENSURE_SUCCESS(rv, rv);

        m_password = password;
        break;
      }
    }
  }
  return NS_OK;
}

NS_IMETHODIMP
nsSmtpServer::GetPasswordWithUI(const char16_t *aPromptMessage,
                                const char16_t *aPromptTitle,
                                nsIAuthPrompt *aDialog, nsAString &aPassword) {
  if (!m_password.IsEmpty()) return GetPassword(aPassword);

  // We need to get a password, but see if we can get it from the password
  // manager without requiring a prompt.
  nsresult rv = GetPasswordWithoutUI();
  if (rv == NS_ERROR_ABORT) return NS_MSG_PASSWORD_PROMPT_CANCELLED;

  // Now re-check if we've got a password or not, if we have, then we
  // don't need to prompt the user.
  if (!m_password.IsEmpty()) {
    aPassword = m_password;
    return NS_OK;
  }

  NS_ENSURE_ARG_POINTER(aDialog);

  // PromptPassword needs the username as well.
  nsCString serverUri(GetServerURIInternal(true));

  bool okayValue = true;

  rv = aDialog->PromptPassword(aPromptTitle, aPromptMessage,
                               NS_ConvertASCIItoUTF16(serverUri).get(),
                               nsIAuthPrompt::SAVE_PASSWORD_PERMANENTLY,
                               getter_Copies(aPassword), &okayValue);
  NS_ENSURE_SUCCESS(rv, rv);

  // If the user pressed cancel, just return an empty string.
  if (!okayValue) {
    aPassword.Truncate();
    return NS_MSG_PASSWORD_PROMPT_CANCELLED;
  }
  rv = SetPassword(aPassword);
  NS_ENSURE_SUCCESS(rv, rv);

  return NS_OK;
}

NS_IMETHODIMP
nsSmtpServer::GetUsernamePasswordWithUI(const char16_t *aPromptMessage,
                                        const char16_t *aPromptTitle,
                                        nsIAuthPrompt *aDialog,
                                        nsACString &aUsername,
                                        nsAString &aPassword) {
  nsresult rv;
  if (!m_password.IsEmpty()) {
    rv = GetUsername(aUsername);
    NS_ENSURE_SUCCESS(rv, rv);

    return GetPassword(aPassword);
  }

  NS_ENSURE_ARG_POINTER(aDialog);

  nsCString serverUri;
  rv = GetServerURI(serverUri);
  NS_ENSURE_SUCCESS(rv, rv);

  nsString uniUsername;
  bool okayValue = true;

  rv = aDialog->PromptUsernameAndPassword(
      aPromptTitle, aPromptMessage, NS_ConvertASCIItoUTF16(serverUri).get(),
      nsIAuthPrompt::SAVE_PASSWORD_PERMANENTLY, getter_Copies(uniUsername),
      getter_Copies(aPassword), &okayValue);
  NS_ENSURE_SUCCESS(rv, rv);

  // If the user pressed cancel, just return empty strings.
  if (!okayValue) {
    aUsername.Truncate();
    aPassword.Truncate();
    return rv;
  }

  // We got a username and password back...so remember them.
  NS_LossyConvertUTF16toASCII username(uniUsername);

  rv = SetUsername(username);
  NS_ENSURE_SUCCESS(rv, rv);

  rv = SetPassword(aPassword);
  NS_ENSURE_SUCCESS(rv, rv);

  aUsername = username;
  return NS_OK;
}

NS_IMETHODIMP
nsSmtpServer::ForgetPassword() {
  nsresult rv;
  nsCOMPtr<nsILoginManager> loginMgr =
      do_GetService(NS_LOGINMANAGER_CONTRACTID, &rv);
  NS_ENSURE_SUCCESS(rv, rv);

  // Get the current server URI without the username
  NS_ConvertASCIItoUTF16 serverUri(GetServerURIInternal(false));

  nsCString serverCUsername;
  rv = GetUsername(serverCUsername);
  NS_ENSURE_SUCCESS(rv, rv);

  NS_ConvertUTF8toUTF16 serverUsername(serverCUsername);

  nsTArray<RefPtr<nsILoginInfo>> logins;
  rv = loginMgr->FindLogins(serverUri, EmptyString(), serverUri, logins);
  NS_ENSURE_SUCCESS(rv, rv);

  // There should only be one-login stored for this url, however just in case
  // there isn't.
  nsString username;
  for (uint32_t i = 0; i < logins.Length(); ++i) {
    if (NS_SUCCEEDED(logins[i]->GetUsername(username)) &&
        username.Equals(serverUsername)) {
      // If this fails, just continue, we'll still want to remove the password
      // from our local cache.
      loginMgr->RemoveLogin(logins[i]);
    }
  }

  rv = SetPassword(EmptyString());
  m_logonFailed = true;
  return rv;
}

NS_IMETHODIMP
nsSmtpServer::GetServerURI(nsACString &aResult) {
  aResult = GetServerURIInternal(true);
  return NS_OK;
}

nsCString nsSmtpServer::GetServerURIInternal(const bool aIncludeUsername) {
  nsCString uri(NS_LITERAL_CSTRING("smtp://"));
  nsresult rv;

  if (aIncludeUsername) {
    nsCString username;
    rv = GetUsername(username);

    if (NS_SUCCEEDED(rv) && !username.IsEmpty()) {
      nsCString escapedUsername;
      MsgEscapeString(username, nsINetUtil::ESCAPE_XALPHAS, escapedUsername);
      // not all servers have a username
      uri.Append(escapedUsername);
      uri.Append('@');
    }
  }

  nsCString hostname;
  rv = GetHostname(hostname);

  if (NS_SUCCEEDED(rv) && !hostname.IsEmpty()) {
    nsCString escapedHostname;
    MsgEscapeString(hostname, nsINetUtil::ESCAPE_URL_PATH, escapedHostname);
    // not all servers have a hostname
    uri.Append(escapedHostname);
  }

  return uri;
}

NS_IMETHODIMP
nsSmtpServer::ClearAllValues() { return mPrefBranch->DeleteBranch(""); }
