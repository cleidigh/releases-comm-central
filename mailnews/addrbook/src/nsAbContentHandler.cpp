/* -*- Mode: C++; tab-width: 2; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "nsAbContentHandler.h"
#include "nsAbBaseCID.h"
#include "nsNetUtil.h"
#include "nsCOMPtr.h"
#include "mozilla/NullPrincipal.h"
#include "mozilla/dom/BrowsingContext.h"
#include "mozilla/UniquePtr.h"
#include "nsISupportsPrimitives.h"
#include "plstr.h"
#include "nsPIDOMWindow.h"
#include "mozIDOMWindow.h"
#include "nsIDocShell.h"
#include "nsIDocShellTreeItem.h"
#include "nsMsgUtils.h"
#include "nsIMsgVCardService.h"
#include "nsIAbCard.h"
#include "nsIChannel.h"
//
// nsAbContentHandler
//
nsAbContentHandler::nsAbContentHandler() {}

nsAbContentHandler::~nsAbContentHandler() {}

NS_IMPL_ISUPPORTS(nsAbContentHandler, nsIContentHandler,
                  nsIStreamLoaderObserver)

NS_IMETHODIMP
nsAbContentHandler::HandleContent(const char *aContentType,
                                  nsIInterfaceRequestor *aWindowContext,
                                  nsIRequest *request) {
  NS_ENSURE_ARG_POINTER(request);

  nsresult rv = NS_OK;

  // First of all, get the content type and make sure it is a content type we
  // know how to handle!
  if (PL_strcasecmp(aContentType, "application/x-addvcard") == 0) {
    nsCOMPtr<nsIURI> uri;
    nsCOMPtr<nsIChannel> aChannel = do_QueryInterface(request);
    if (!aChannel) return NS_ERROR_FAILURE;

    rv = aChannel->GetURI(getter_AddRefs(uri));
    if (uri) {
      nsAutoCString path;
      rv = uri->GetPathQueryRef(path);
      NS_ENSURE_SUCCESS(rv, rv);

      const char *startOfVCard = strstr(path.get(), "add?vcard=");
      if (startOfVCard) {
        nsCString unescapedData;

        // XXX todo, explain why we is escaped twice
        MsgUnescapeString(
            nsDependentCString(startOfVCard + strlen("add?vcard=")), 0,
            unescapedData);

        if (!aWindowContext) return NS_ERROR_FAILURE;

        nsCOMPtr<mozIDOMWindowProxy> domWindow =
            do_GetInterface(aWindowContext);
        NS_ENSURE_TRUE(domWindow, NS_ERROR_FAILURE);
        nsCOMPtr<nsPIDOMWindowOuter> parentWindow =
            nsPIDOMWindowOuter::From(domWindow);

        nsCOMPtr<nsIMsgVCardService> vCardService =
            do_GetService(NS_MSGVCARDSERVICE_CONTRACTID, &rv);
        NS_ENSURE_SUCCESS(rv, rv);

        nsCOMPtr<nsIAbCard> cardFromVCard;
        rv = vCardService->EscapedVCardToAbCard(unescapedData.get(),
                                                getter_AddRefs(cardFromVCard));
        NS_ENSURE_SUCCESS(rv, rv);

        nsCOMPtr<nsISupportsInterfacePointer> ifptr =
            do_CreateInstance(NS_SUPPORTS_INTERFACE_POINTER_CONTRACTID, &rv);
        NS_ENSURE_SUCCESS(rv, rv);

        ifptr->SetData(cardFromVCard);
        ifptr->SetDataIID(&NS_GET_IID(nsIAbCard));

        // Find a privileged chrome window to open the dialog from.
        nsCOMPtr<nsIDocShell> docShell(parentWindow->GetDocShell());
        nsCOMPtr<nsIDocShellTreeItem> root;
        docShell->GetInProcessRootTreeItem(getter_AddRefs(root));
        nsCOMPtr<nsPIDOMWindowOuter> window(do_GetInterface(root));

        RefPtr<mozilla::dom::BrowsingContext> dialogWindow;
        rv = window->OpenDialog(
            NS_LITERAL_STRING(
                "chrome://messenger/content/addressbook/abNewCardDialog.xhtml"),
            EmptyString(),
            NS_LITERAL_STRING(
                "chrome,resizable=no,titlebar,modal,centerscreen"),
            ifptr, getter_AddRefs(dialogWindow));
        NS_ENSURE_SUCCESS(rv, rv);
      }
      rv = NS_OK;
    }
  } else if (PL_strcasecmp(aContentType, "text/x-vcard") == 0) {
    // create a vcard stream listener that can parse the data stream
    // and bring up the appropriate UI

    // (1) cancel the current load operation. We'll restart it
    request->Cancel(NS_ERROR_ABORT);
    // get the url we were trying to open
    nsCOMPtr<nsIURI> uri;
    nsCOMPtr<nsIChannel> channel = do_QueryInterface(request);
    NS_ENSURE_TRUE(channel, NS_ERROR_FAILURE);

    rv = channel->GetURI(getter_AddRefs(uri));
    NS_ENSURE_SUCCESS(rv, rv);

    nsCOMPtr<nsIPrincipal> nullPrincipal =
        do_CreateInstance("@mozilla.org/nullprincipal;1", &rv);
    NS_ENSURE_SUCCESS(rv, rv);

    // create a stream loader to handle the v-card data
    nsCOMPtr<nsIStreamLoader> streamLoader;
    rv = NS_NewStreamLoader(getter_AddRefs(streamLoader), uri, this,
                            nullPrincipal,
                            nsILoadInfo::SEC_ALLOW_CROSS_ORIGIN_DATA_IS_NULL,
                            nsIContentPolicy::TYPE_OTHER);
    NS_ENSURE_SUCCESS(rv, rv);

  } else  // The content-type was not application/x-addvcard...
    return NS_ERROR_WONT_HANDLE_CONTENT;

  return rv;
}

NS_IMETHODIMP
nsAbContentHandler::OnStreamComplete(nsIStreamLoader *aLoader,
                                     nsISupports *aContext, nsresult aStatus,
                                     uint32_t datalen, const uint8_t *data) {
  NS_ENSURE_ARG_POINTER(aContext);
  NS_ENSURE_SUCCESS(
      aStatus, aStatus);  // don't process the vcard if we got a status error
  nsresult rv = NS_OK;

  // take our vCard string and open up an address book window based on it
  nsCOMPtr<nsIMsgVCardService> vCardService =
      do_GetService(NS_MSGVCARDSERVICE_CONTRACTID, &rv);
  NS_ENSURE_SUCCESS(rv, rv);

  nsCOMPtr<nsIAbCard> cardFromVCard;
  rv = vCardService->EscapedVCardToAbCard((const char *)data,
                                          getter_AddRefs(cardFromVCard));
  NS_ENSURE_SUCCESS(rv, rv);

  nsCOMPtr<mozIDOMWindowProxy> domWindow = do_GetInterface(aContext);
  NS_ENSURE_TRUE(domWindow, NS_ERROR_FAILURE);
  nsCOMPtr<nsPIDOMWindowOuter> parentWindow =
      nsPIDOMWindowOuter::From(domWindow);

  RefPtr<mozilla::dom::BrowsingContext> dialogWindow;
  return parentWindow->OpenDialog(
      NS_LITERAL_STRING(
          "chrome://messenger/content/addressbook/abNewCardDialog.xhtml"),
      EmptyString(),
      NS_LITERAL_STRING("chrome,resizable=no,titlebar,modal,centerscreen"),
      cardFromVCard, getter_AddRefs(dialogWindow));
}
