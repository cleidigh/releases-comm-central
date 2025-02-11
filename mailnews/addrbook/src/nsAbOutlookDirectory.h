/* -*- Mode: C++; tab-width: 2; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */
#ifndef nsAbOutlookDirectory_h___
#define nsAbOutlookDirectory_h___

#include "mozilla/Attributes.h"
#include "nsAbDirProperty.h"
#include "nsIAbDirectoryQuery.h"
#include "nsIAbDirSearchListener.h"
#include "nsDataHashtable.h"
#include "nsInterfaceHashtable.h"
#include "nsIMutableArray.h"
#include "nsAbWinHelper.h"
#include "prlock.h"

struct nsMapiEntry;

class nsAbOutlookDirectory : public nsAbDirProperty,  // nsIAbDirectory
                             public nsIAbDirectoryQuery,
                             public nsIAbDirSearchListener,
                             public nsIAbDirectoryQueryResultListener {
 public:
  NS_DECL_ISUPPORTS_INHERITED
  NS_DECL_NSIABDIRSEARCHLISTENER
  NS_DECL_NSIABDIRECTORYQUERYRESULTLISTENER

  nsAbOutlookDirectory(void);

  // nsAbDirProperty methods
  NS_IMETHOD GetDirType(int32_t *aDirType) override;
  NS_IMETHOD GetURI(nsACString &aURI) override;
  NS_IMETHOD GetChildCards(nsISimpleEnumerator **aCards) override;
  NS_IMETHOD GetChildNodes(nsISimpleEnumerator **aNodes) override;
  NS_IMETHOD GetIsQuery(bool *aResult) override;
  NS_IMETHOD HasCard(nsIAbCard *aCard, bool *aHasCard) override;
  NS_IMETHOD HasDirectory(nsIAbDirectory *aDirectory,
                          bool *aHasDirectory) override;
  NS_IMETHOD DeleteCards(const nsTArray<RefPtr<nsIAbCard>> &aCards) override;
  NS_IMETHOD DeleteDirectory(nsIAbDirectory *aDirectory) override;
  NS_IMETHOD UseForAutocomplete(const nsACString &aIdentityKey,
                                bool *aResult) override;
  NS_IMETHOD AddCard(nsIAbCard *aData, nsIAbCard **addedCard) override;
  NS_IMETHOD ModifyCard(nsIAbCard *aModifiedCard) override;
  NS_IMETHOD DropCard(nsIAbCard *aData, bool needToCopyCard) override;
  NS_IMETHOD AddMailList(nsIAbDirectory *aMailList,
                         nsIAbDirectory **addedList) override;
  NS_IMETHOD EditMailListToDatabase(nsIAbCard *listCard) override;

  // nsAbDirProperty method
  NS_IMETHOD Init(const char *aUri) override;
  // nsIAbDirectoryQuery methods
  NS_DECL_NSIABDIRECTORYQUERY
  // Perform a MAPI query (function executed in a separate thread)
  nsresult ExecuteQuery(SRestriction &aRestriction,
                        nsIAbDirSearchListener *aListener, int32_t aResultLimit,
                        int32_t aTimeout, int32_t aThreadId);

 protected:
  nsresult StartSearch();
  nsresult StopSearch();

  // Retrieve hierarchy as cards, with an optional restriction
  nsresult GetChildCards(nsIMutableArray *aCards, void *aRestriction);
  // Retrieve hierarchy as directories
  nsresult GetChildNodes(nsIMutableArray *aNodes);
  // Create a new card
  nsresult CreateCard(nsIAbCard *aData, nsIAbCard **aNewCard);
  // Notification for the UI
  nsresult NotifyItemDeletion(nsISupports *aItem);
  nsresult NotifyItemAddition(nsISupports *aItem);
  // Force update of MAPI repository for mailing list
  nsresult CommitAddressList(void);
  // Read MAPI repository
  nsresult UpdateAddressList(void);

  nsMapiEntry *mMapiData;
  // Container for the query threads
  nsDataHashtable<nsUint32HashKey, PRThread *> mQueryThreads;
  int32_t mCurrentQueryId;
  PRLock *mProtector;
  // Data for the search interfaces
  nsInterfaceHashtable<nsISupportsHashKey, nsIAbCard> mCardList;
  int32_t mSearchContext;
  // Windows AB type
  uint32_t mAbWinType;

 private:
  virtual ~nsAbOutlookDirectory(void);
};

enum {
  index_DisplayName = 0,
  index_EmailAddress,
  index_FirstName,
  index_LastName,
  index_NickName,
  index_WorkPhoneNumber,
  index_HomePhoneNumber,
  index_WorkFaxNumber,
  index_PagerNumber,
  index_MobileNumber,
  index_HomeCity,
  index_HomeState,
  index_HomeZip,
  index_HomeCountry,
  index_WorkCity,
  index_WorkState,
  index_WorkZip,
  index_WorkCountry,
  index_JobTitle,
  index_Department,
  index_Company,
  index_WorkWebPage,
  index_HomeWebPage,
  index_Comments,
  index_LastProp
};

static const ULONG OutlookCardMAPIProps[] = {
    PR_DISPLAY_NAME_W,
    PR_EMAIL_ADDRESS_W,
    PR_GIVEN_NAME_W,
    PR_SURNAME_W,
    PR_NICKNAME_W,
    PR_BUSINESS_TELEPHONE_NUMBER_W,
    PR_HOME_TELEPHONE_NUMBER_W,
    PR_BUSINESS_FAX_NUMBER_W,
    PR_PAGER_TELEPHONE_NUMBER_W,
    PR_MOBILE_TELEPHONE_NUMBER_W,
    PR_HOME_ADDRESS_CITY_W,
    PR_HOME_ADDRESS_STATE_OR_PROVINCE_W,
    PR_HOME_ADDRESS_POSTAL_CODE_W,
    PR_HOME_ADDRESS_COUNTRY_W,
    PR_BUSINESS_ADDRESS_CITY_W,
    PR_BUSINESS_ADDRESS_STATE_OR_PROVINCE_W,
    PR_BUSINESS_ADDRESS_POSTAL_CODE_W,
    PR_BUSINESS_ADDRESS_COUNTRY_W,
    PR_TITLE_W,
    PR_DEPARTMENT_NAME_W,
    PR_COMPANY_NAME_W,
    PR_BUSINESS_HOME_PAGE_W,
    PR_PERSONAL_HOME_PAGE_W,
    PR_COMMENT_W};

nsresult OutlookCardForURI(const nsACString &aUri, nsIAbCard **card);

#endif  // nsAbOutlookDirectory_h___
