/* -*- Mode: C++; tab-width: 4; indent-tabs-mode: nil; c-basic-offset: 4 -*- */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef _nsMsgFolderCompactor_h
#define _nsMsgFolderCompactor_h

#include "mozilla/Attributes.h"
#include "nsCOMPtr.h"
#include "nsIMsgFolder.h"
#include "nsIStreamListener.h"
#include "nsIMsgFolderCompactor.h"
#include "nsICopyMsgStreamListener.h"
#include "nsMsgKeyArray.h"
#include "nsIMsgWindow.h"
#include "nsIStringBundle.h"
#include "nsIMsgMessageService.h"

#define COMPACTOR_READ_BUFF_SIZE 16384

class nsFolderCompactState : public nsIMsgFolderCompactor,
                             public nsIStreamListener,
                             public nsICopyMessageStreamListener,
                             public nsIUrlListener {
 public:
  NS_DECL_ISUPPORTS
  NS_DECL_NSIREQUESTOBSERVER
  NS_DECL_NSISTREAMLISTENER
  NS_DECL_NSICOPYMESSAGESTREAMLISTENER
  NS_DECL_NSIURLLISTENER
  NS_DECL_NSIMSGFOLDERCOMPACTOR

  nsFolderCompactState(void);

 protected:
  virtual ~nsFolderCompactState(void);

  virtual nsresult InitDB(nsIMsgDatabase *db);
  virtual nsresult StartCompacting();
  virtual nsresult FinishCompact();
  void CloseOutputStream();
  void CleanupTempFilesAfterError();

  nsresult Init(nsIMsgFolder *aFolder, const char *aBaseMsgUri,
                nsIMsgDatabase *aDb, nsIFile *aPath, nsIMsgWindow *aMsgWindow);
  nsresult GetMessage(nsIMsgDBHdr **message);
  nsresult BuildMessageURI(const char *baseURI, nsMsgKey key, nsCString &uri);
  nsresult ShowStatusMsg(const nsString &aMsg);
  nsresult ReleaseFolderLock();
  void ShowCompactingStatusMsg();
  void CompactCompleted(nsresult exitCode);
  void ShowDoneStatus();
  nsresult CompactNextFolder();

  nsCString m_baseMessageUri;       // base message uri
  nsCString m_messageUri;           // current message uri being copy
  nsCOMPtr<nsIMsgFolder> m_folder;  // current folder being compact
  nsCOMPtr<nsIMsgDatabase> m_db;    // new database for the compact folder
  nsCOMPtr<nsIFile> m_file;         // new mailbox for the compact folder
  nsCOMPtr<nsIOutputStream> m_fileStream;  // output file stream for writing
  // all message keys that need to be copied over
  RefPtr<nsMsgKeyArray> m_keyArray;
  uint32_t m_size;

  // sum of the sizes of the messages, accumulated as we visit each msg.
  uint64_t m_totalMsgSize;
  // number of bytes that can be expunged while compacting.
  uint64_t m_totalExpungedBytes;

  uint32_t m_curIndex;  // index of the current copied message key in key array
  uint64_t m_startOfNewMsg;  // offset in mailbox of new message
  char m_dataBuffer[COMPACTOR_READ_BUFF_SIZE + 1];  // temp data buffer for
                                                    // copying message
  nsresult m_status;  // the status of the copying operation
  nsCOMPtr<nsIMsgMessageService> m_messageService;  // message service for
                                                    // copying
  nsTArray<RefPtr<nsIMsgFolder>> m_folderArray;  // folders we are compacting,
                                                 // if compacting multiple.
  nsCOMPtr<nsIMsgWindow> m_window;
  nsCOMPtr<nsIMsgDBHdr> m_curSrcHdr;
  uint32_t m_folderIndex;           // tells which folder to compact in case of
                                    // compact all
  bool m_compactAll;                // flag for compact all
  bool m_compactOfflineAlso;        // whether to compact offline also
  bool m_compactingOfflineFolders;  // are we in offline folder compact phase
  bool m_parsingFolder;             // flag for parsing local folders;
  // these members are used to add missing status lines to compacted messages.
  bool m_needStatusLine;
  bool m_startOfMsg;
  int32_t m_statusOffset;
  uint32_t m_addedHeaderSize;
  nsTArray<RefPtr<nsIMsgFolder>> m_offlineFolderArray;
  nsCOMPtr<nsIUrlListener> m_listener;
  bool m_alreadyWarnedDiskSpace;
};

class nsOfflineStoreCompactState : public nsFolderCompactState {
 public:
  nsOfflineStoreCompactState(void);
  virtual ~nsOfflineStoreCompactState(void);
  NS_IMETHOD OnStopRequest(nsIRequest *request, nsresult status) override;
  NS_IMETHODIMP OnDataAvailable(nsIRequest *request, nsIInputStream *inStr,
                                uint64_t sourceOffset, uint32_t count) override;

 protected:
  nsresult CopyNextMessage(bool &done);
  virtual nsresult InitDB(nsIMsgDatabase *db) override;
  virtual nsresult StartCompacting() override;
  virtual nsresult FinishCompact() override;

  uint32_t m_offlineMsgSize;
};

#endif
