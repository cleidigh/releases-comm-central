/* -*- Mode: C++; tab-width: 2; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "msgCore.h"

#include "nsMailboxProtocol.h"
#include "nscore.h"
#include "nsIInputStreamPump.h"
#include "nsIMsgHdr.h"
#include "nsMsgLineBuffer.h"
#include "nsMsgDBCID.h"
#include "nsIMsgMailNewsUrl.h"
#include "nsIMsgFolder.h"
#include "nsICopyMsgStreamListener.h"
#include "prtime.h"
#include "mozilla/Logging.h"
#include "mozilla/SlicedInputStream.h"
#include "prerror.h"
#include "prprf.h"
#include "nspr.h"
#include "nsIStreamTransportService.h"
#include "nsIStreamConverterService.h"
#include "nsNetUtil.h"
#include "nsMsgUtils.h"
#include "nsIMsgWindow.h"
#include "nsISeekableStream.h"
#include "nsStreamUtils.h"

using namespace mozilla;

static LazyLogModule MAILBOX("Mailbox");

/* the output_buffer_size must be larger than the largest possible line
 * 2000 seems good for news
 *
 * jwz: I increased this to 4k since it must be big enough to hold the
 * entire button-bar HTML, and with the new "mailto" format, that can
 * contain arbitrarily long header fields like "references".
 *
 * fortezza: proxy auth is huge, buffer increased to 8k (sigh).
 */
#define OUTPUT_BUFFER_SIZE (4096 * 2)

nsMailboxProtocol::nsMailboxProtocol(nsIURI *aURI) : nsMsgProtocol(aURI) {}

nsMailboxProtocol::~nsMailboxProtocol() {}

nsresult nsMailboxProtocol::OpenMultipleMsgTransport(uint64_t offset,
                                                     int64_t size) {
  nsresult rv;

  nsCOMPtr<nsIStreamTransportService> serv =
      do_GetService(NS_STREAMTRANSPORTSERVICE_CONTRACTID, &rv);
  NS_ENSURE_SUCCESS(rv, rv);

  nsCOMPtr<nsIInputStream> clonedStream;
  nsCOMPtr<nsIInputStream> replacementStream;
  rv = NS_CloneInputStream(m_multipleMsgMoveCopyStream,
                           getter_AddRefs(clonedStream),
                           getter_AddRefs(replacementStream));
  NS_ENSURE_SUCCESS(rv, rv);
  if (replacementStream) {
    // If m_multipleMsgMoveCopyStream is not cloneable, NS_CloneInputStream
    // will clone it using a pipe. In order to keep the copy alive and working,
    // we have to replace the original stream with the replacement.
    m_multipleMsgMoveCopyStream = replacementStream.forget();
  }
  // XXX 64-bit
  // This can be called with size == -1 which means "read as much as we can".
  // We pass this on as UINT64_MAX, which is in fact uint64_t(-1).
  RefPtr<SlicedInputStream> slicedStream = new SlicedInputStream(
      clonedStream.forget(), offset, size == -1 ? UINT64_MAX : uint64_t(size));
  // Always close the sliced stream when done, we still have the original.
  rv = serv->CreateInputTransport(slicedStream, true,
                                  getter_AddRefs(m_transport));

  return rv;
}

nsresult nsMailboxProtocol::Initialize(nsIURI *aURL) {
  NS_ASSERTION(aURL, "invalid URL passed into MAILBOX Protocol");
  nsresult rv = NS_OK;
  if (aURL) {
    m_runningUrl = do_QueryInterface(aURL, &rv);
    if (NS_SUCCEEDED(rv) && m_runningUrl) {
      nsCOMPtr<nsIMsgWindow> window;
      rv = m_runningUrl->GetMailboxAction(&m_mailboxAction);
      // clear stopped flag on msg window, because we care.
      nsCOMPtr<nsIMsgMailNewsUrl> mailnewsUrl = do_QueryInterface(m_runningUrl);
      if (mailnewsUrl) {
        mailnewsUrl->GetMsgWindow(getter_AddRefs(window));
        if (window) window->SetStopped(false);
      }

      if (m_mailboxAction == nsIMailboxUrl::ActionParseMailbox) {
        // Set the length of the file equal to the max progress
        nsCOMPtr<nsIFile> file;
        GetFileFromURL(aURL, getter_AddRefs(file));
        if (file) {
          int64_t fileSize = 0;
          file->GetFileSize(&fileSize);
          mailnewsUrl->SetMaxProgress(fileSize);
        }

        rv =
            OpenFileSocket(aURL, 0, -1 /* read in all the bytes in the file */);
      } else {
        // we need to specify a byte range to read in so we read in JUST the
        // message we want.
        rv = SetupMessageExtraction();
        if (NS_FAILED(rv)) return rv;
        uint32_t msgSize = 0;
        rv = m_runningUrl->GetMessageSize(&msgSize);
        NS_ASSERTION(NS_SUCCEEDED(rv), "oops....i messed something up");
        SetContentLength(msgSize);
        mailnewsUrl->SetMaxProgress(msgSize);

        if (RunningMultipleMsgUrl()) {
          // if we're running multiple msg url, we clear the event sink because
          // the multiple msg urls will handle setting the progress.
          mProgressEventSink = nullptr;
        }

        nsCOMPtr<nsIMsgMessageUrl> msgUrl =
            do_QueryInterface(m_runningUrl, &rv);
        if (NS_SUCCEEDED(rv)) {
          nsCOMPtr<nsIMsgFolder> folder;
          nsCOMPtr<nsIMsgDBHdr> msgHdr;
          rv = msgUrl->GetMessageHeader(getter_AddRefs(msgHdr));
          if (NS_SUCCEEDED(rv) && msgHdr) {
            rv = msgHdr->GetFolder(getter_AddRefs(folder));
            if (NS_SUCCEEDED(rv) && folder) {
              nsCOMPtr<nsIInputStream> stream;
              int64_t offset = 0;
              bool reusable = false;

              rv = folder->GetMsgInputStream(msgHdr, &reusable,
                                             getter_AddRefs(stream));
              NS_ENSURE_SUCCESS(rv, rv);
              nsCOMPtr<nsISeekableStream> seekableStream(
                  do_QueryInterface(stream, &rv));
              NS_ENSURE_SUCCESS(rv, rv);
              seekableStream->Tell(&offset);
              // create input stream transport
              nsCOMPtr<nsIStreamTransportService> sts =
                  do_GetService(NS_STREAMTRANSPORTSERVICE_CONTRACTID, &rv);
              if (NS_FAILED(rv)) return rv;
              m_readCount = msgSize;
              // Save the stream for reuse, but only for multiple URLs.
              if (reusable && RunningMultipleMsgUrl()) {
                nsCOMPtr<nsIInputStream> clonedStream;
                nsCOMPtr<nsIInputStream> replacementStream;
                rv = NS_CloneInputStream(stream, getter_AddRefs(clonedStream),
                                         getter_AddRefs(replacementStream));
                NS_ENSURE_SUCCESS(rv, rv);
                if (replacementStream) {
                  // If stream is not cloneable, NS_CloneInputStream
                  // will clone it using a pipe. In order to keep the copy alive
                  // and working, we have to replace the original stream with
                  // the replacement.
                  stream = replacementStream.forget();
                }
                // Keep the original and use the clone for the next operation.
                m_multipleMsgMoveCopyStream = stream;
                stream = clonedStream;
              } else {
                reusable = false;
              }
              RefPtr<SlicedInputStream> slicedStream = new SlicedInputStream(
                  stream.forget(), offset, uint64_t(msgSize));
              // Always close the sliced stream when done, we still have the
              // original.
              rv = sts->CreateInputTransport(slicedStream, true,
                                             getter_AddRefs(m_transport));

              m_socketIsOpen = false;
            }
          }
          if (!folder)  // must be a .eml file
            rv = OpenFileSocket(aURL, 0, int64_t(msgSize));
        }
        NS_ASSERTION(NS_SUCCEEDED(rv), "oops....i messed something up");
      }
    }
  }

  m_lineStreamBuffer = new nsMsgLineStreamBuffer(OUTPUT_BUFFER_SIZE, true);

  m_nextState = MAILBOX_READ_FOLDER;
  m_initialState = MAILBOX_READ_FOLDER;
  mCurrentProgress = 0;

  // do we really need both?
  m_tempMessageFile = m_tempMsgFile;
  return rv;
}

/////////////////////////////////////////////////////////////////////////////////////////////
// we support the nsIStreamListener interface
////////////////////////////////////////////////////////////////////////////////////////////

NS_IMETHODIMP nsMailboxProtocol::OnStartRequest(nsIRequest *request) {
  // extract the appropriate event sinks from the url and initialize them in our
  // protocol data the URL should be queried for a nsINewsURL. If it doesn't
  // support a news URL interface then we have an error.
  if (m_nextState == MAILBOX_READ_FOLDER && m_mailboxParser) {
    // we need to inform our mailbox parser that it's time to start...
    // NOTE: `request` here will be an nsInputStreamPump, but our callbacks
    // are expecting to be able to QI to a `nsIChannel` to get the URI.
    // So we pass `this`. See Bug 1528662.
    m_mailboxParser->OnStartRequest(this);
  }
  return nsMsgProtocol::OnStartRequest(request);
}

bool nsMailboxProtocol::RunningMultipleMsgUrl() {
  if (m_mailboxAction == nsIMailboxUrl::ActionCopyMessage ||
      m_mailboxAction == nsIMailboxUrl::ActionMoveMessage) {
    uint32_t numMoveCopyMsgs;
    nsresult rv = m_runningUrl->GetNumMoveCopyMsgs(&numMoveCopyMsgs);
    if (NS_SUCCEEDED(rv) && numMoveCopyMsgs > 1) return true;
  }
  return false;
}

// stop binding is a "notification" informing us that the stream associated with
// aURL is going away.
NS_IMETHODIMP nsMailboxProtocol::OnStopRequest(nsIRequest *request,
                                               nsresult aStatus) {
  nsresult rv;
  if (m_nextState == MAILBOX_READ_FOLDER && m_mailboxParser) {
    // we need to inform our mailbox parser that there is no more incoming
    // data... NOTE: `request` here will be an nsInputStreamPump, but our
    // callbacks are expecting to be able to QI to a `nsIChannel` to get the
    // URI. So we pass `this`. See Bug 1528662.
    m_mailboxParser->OnStopRequest(this, aStatus);
  } else if (m_nextState == MAILBOX_READ_MESSAGE) {
    DoneReadingMessage();
  }
  // I'm not getting cancel status - maybe the load group still has the status.
  bool stopped = false;
  if (m_runningUrl) {
    nsCOMPtr<nsIMsgMailNewsUrl> mailnewsUrl = do_QueryInterface(m_runningUrl);
    if (mailnewsUrl) {
      nsCOMPtr<nsIMsgWindow> window;
      mailnewsUrl->GetMsgWindow(getter_AddRefs(window));
      if (window) window->GetStopped(&stopped);
    }

    if (!stopped && NS_SUCCEEDED(aStatus) &&
        (m_mailboxAction == nsIMailboxUrl::ActionCopyMessage ||
         m_mailboxAction == nsIMailboxUrl::ActionMoveMessage)) {
      uint32_t numMoveCopyMsgs;
      uint32_t curMoveCopyMsgIndex;
      rv = m_runningUrl->GetNumMoveCopyMsgs(&numMoveCopyMsgs);
      if (NS_SUCCEEDED(rv) && numMoveCopyMsgs > 0) {
        m_runningUrl->GetCurMoveCopyMsgIndex(&curMoveCopyMsgIndex);
        if (++curMoveCopyMsgIndex < numMoveCopyMsgs) {
          if (!mSuppressListenerNotifications && m_channelListener) {
            nsCOMPtr<nsICopyMessageStreamListener> listener =
                do_QueryInterface(m_channelListener, &rv);
            if (listener) {
              listener->EndCopy(m_runningUrl, aStatus);
              listener->StartMessage();  // start next message.
            }
          }
          m_runningUrl->SetCurMoveCopyMsgIndex(curMoveCopyMsgIndex);
          nsCOMPtr<nsIMsgDBHdr> nextMsg;
          rv = m_runningUrl->GetMoveCopyMsgHdrForIndex(curMoveCopyMsgIndex,
                                                       getter_AddRefs(nextMsg));
          if (NS_SUCCEEDED(rv) && nextMsg) {
            uint32_t msgSize = 0;
            nsCOMPtr<nsIMsgFolder> msgFolder;
            nextMsg->GetFolder(getter_AddRefs(msgFolder));
            NS_ASSERTION(
                msgFolder,
                "couldn't get folder for next msg in multiple msg local copy");
            if (msgFolder) {
              nsCString uri;
              msgFolder->GetUriForMsg(nextMsg, uri);
              nsCOMPtr<nsIMsgMessageUrl> msgUrl =
                  do_QueryInterface(m_runningUrl);
              if (msgUrl) {
                msgUrl->SetOriginalSpec(uri.get());
                msgUrl->SetUri(uri.get());

                uint64_t msgOffset;
                nextMsg->GetMessageOffset(&msgOffset);
                nextMsg->GetMessageSize(&msgSize);
                // now we have to seek to the right position in the file and
                // basically re-initialize the transport with the correct
                // message size. then, we have to make sure the url keeps
                // running somehow.
                //
                // put us in a state where we are always notified of incoming
                // data
                //
                m_transport = nullptr;  // open new stream transport
                m_outputStream = nullptr;

                if (m_multipleMsgMoveCopyStream) {
                  rv = OpenMultipleMsgTransport(msgOffset, msgSize);
                } else {
                  nsCOMPtr<nsIInputStream> stream;
                  bool reusable = false;
                  rv = msgFolder->GetMsgInputStream(nextMsg, &reusable,
                                                    getter_AddRefs(stream));
                  NS_ASSERTION(!reusable,
                               "We thought streams were not reusable!");

                  if (NS_SUCCEEDED(rv)) {
                    // create input stream transport
                    nsCOMPtr<nsIStreamTransportService> sts = do_GetService(
                        NS_STREAMTRANSPORTSERVICE_CONTRACTID, &rv);

                    if (NS_SUCCEEDED(rv)) {
                      m_readCount = msgSize;
                      RefPtr<SlicedInputStream> slicedStream =
                          new SlicedInputStream(stream.forget(), msgOffset,
                                                uint64_t(msgSize));
                      rv = sts->CreateInputTransport(
                          slicedStream, true, getter_AddRefs(m_transport));
                    }
                  }
                }

                if (NS_SUCCEEDED(rv)) {
                  nsCOMPtr<nsIInputStream> stream;
                  rv = m_transport->OpenInputStream(0, 0, 0,
                                                    getter_AddRefs(stream));

                  if (NS_SUCCEEDED(rv)) {
                    nsCOMPtr<nsIInputStreamPump> pump;
                    rv = NS_NewInputStreamPump(getter_AddRefs(pump),
                                               stream.forget());
                    if (NS_SUCCEEDED(rv)) {
                      rv = pump->AsyncRead(this, nullptr);
                      if (NS_SUCCEEDED(rv)) m_request = pump;
                    }
                  }
                }

                NS_ASSERTION(NS_SUCCEEDED(rv), "AsyncRead failed");
                if (m_loadGroup)
                  m_loadGroup->RemoveRequest(static_cast<nsIRequest *>(this),
                                             nullptr, aStatus);
                m_socketIsOpen = true;  // mark the channel as open
                return aStatus;
              }
            }
          }
        } else {
        }
      }
    }
  }
  // and we want to mark ourselves for deletion or some how inform our protocol
  // manager that we are available for another url if there is one.

  // mscott --> maybe we should set our state to done because we don't run
  // multiple urls in a mailbox protocol connection....
  m_nextState = MAILBOX_DONE;

  // the following is for smoke test purposes. QA is looking at this "Mailbox
  // Done" string which is printed out to the console and determining if the
  // mail app loaded up correctly...obviously this solution is not very good so
  // we should look at something better, but don't remove this line before
  // talking to me (mscott) and mailnews QA....

  MOZ_LOG(MAILBOX, LogLevel::Info, ("Mailbox Done"));

  // when on stop binding is called, we as the protocol are done...let's close
  // down the connection releasing all of our interfaces. It's important to
  // remember that this on stop binding call is coming from netlib so they are
  // never going to ping us again with on data available. This means we'll never
  // be going through the Process loop...

  if (m_multipleMsgMoveCopyStream) {
    m_multipleMsgMoveCopyStream->Close();
    m_multipleMsgMoveCopyStream = nullptr;
  }
  nsMsgProtocol::OnStopRequest(request, aStatus);
  return CloseSocket();
}

/////////////////////////////////////////////////////////////////////////////////////////////
// End of nsIStreamListenerSupport
//////////////////////////////////////////////////////////////////////////////////////////////

nsresult nsMailboxProtocol::DoneReadingMessage() {
  nsresult rv = NS_OK;
  // and close the article file if it was open....

  if (m_mailboxAction == nsIMailboxUrl::ActionSaveMessageToDisk &&
      m_msgFileOutputStream)
    rv = m_msgFileOutputStream->Close();

  return rv;
}

nsresult nsMailboxProtocol::SetupMessageExtraction() {
  // Determine the number of bytes we are going to need to read out of the
  // mailbox url....
  nsCOMPtr<nsIMsgDBHdr> msgHdr;
  nsresult rv = NS_OK;

  NS_ASSERTION(m_runningUrl, "Not running a url");
  if (m_runningUrl) {
    uint32_t messageSize = 0;
    m_runningUrl->GetMessageSize(&messageSize);
    if (!messageSize) {
      nsCOMPtr<nsIMsgMessageUrl> msgUrl = do_QueryInterface(m_runningUrl, &rv);
      NS_ENSURE_SUCCESS(rv, rv);
      rv = msgUrl->GetMessageHeader(getter_AddRefs(msgHdr));
      if (NS_SUCCEEDED(rv) && msgHdr) {
        msgHdr->GetMessageSize(&messageSize);
        m_runningUrl->SetMessageSize(messageSize);
        msgHdr->GetMessageOffset(&m_msgOffset);
      } else
        NS_ASSERTION(false, "couldn't get message header");
    }
  }
  return rv;
}

/////////////////////////////////////////////////////////////////////////////////////////////
// Begin protocol state machine functions...
//////////////////////////////////////////////////////////////////////////////////////////////

nsresult nsMailboxProtocol::LoadUrl(nsIURI *aURL, nsISupports *aConsumer) {
  nsresult rv = NS_OK;
  // if we were already initialized with a consumer, use it...
  nsCOMPtr<nsIStreamListener> consumer = do_QueryInterface(aConsumer);
  if (consumer) m_channelListener = consumer;

  if (aURL) {
    m_runningUrl = do_QueryInterface(aURL);
    if (m_runningUrl) {
      // find out from the url what action we are supposed to perform...
      rv = m_runningUrl->GetMailboxAction(&m_mailboxAction);

      bool convertData = false;

      // need to check if we're fetching an rfc822 part in order to
      // quote a message.
      if (m_mailboxAction == nsIMailboxUrl::ActionFetchMessage) {
        nsCOMPtr<nsIMsgMailNewsUrl> msgUrl =
            do_QueryInterface(m_runningUrl, &rv);
        NS_ENSURE_SUCCESS(rv, rv);

        nsAutoCString queryStr;
        rv = msgUrl->GetQuery(queryStr);
        NS_ENSURE_SUCCESS(rv, rv);

        // check if this is a filter plugin requesting the message.
        // in that case, set up a text converter
        convertData = (queryStr.Find("header=filter") != -1 ||
                       queryStr.Find("header=attach") != -1);
      } else if (m_mailboxAction == nsIMailboxUrl::ActionFetchPart) {
        // when fetching a part, we need to insert a converter into the listener
        // chain order to force just the part out of the message. Our channel
        // listener is the consumer we'll pass in to AsyncConvertData.
        convertData = true;
        consumer = m_channelListener;
      }
      if (convertData) {
        nsCOMPtr<nsIStreamConverterService> streamConverter =
            do_GetService("@mozilla.org/streamConverters;1", &rv);
        NS_ENSURE_SUCCESS(rv, rv);
        nsCOMPtr<nsIStreamListener> conversionListener;
        nsCOMPtr<nsIChannel> channel;
        QueryInterface(NS_GET_IID(nsIChannel), getter_AddRefs(channel));

        rv = streamConverter->AsyncConvertData(
            "message/rfc822", "*/*", consumer, channel,
            getter_AddRefs(m_channelListener));
      }

      if (NS_SUCCEEDED(rv)) {
        switch (m_mailboxAction) {
          case nsIMailboxUrl::ActionParseMailbox:
            // extract the mailbox parser..
            rv =
                m_runningUrl->GetMailboxParser(getter_AddRefs(m_mailboxParser));
            m_nextState = MAILBOX_READ_FOLDER;
            break;
          case nsIMailboxUrl::ActionSaveMessageToDisk:
            // ohhh, display message already writes a msg to disk (as part of a
            // hack) so we can piggy back off of that!! We just need to change
            // m_tempMessageFile to be the name of our save message to disk
            // file. Since save message to disk urls are run without a docshell
            // to display the msg into, we won't be trying to display the
            // message after we write it to disk...
            {
              nsCOMPtr<nsIMsgMessageUrl> messageUrl =
                  do_QueryInterface(m_runningUrl, &rv);
              if (NS_SUCCEEDED(rv)) {
                messageUrl->GetMessageFile(getter_AddRefs(m_tempMessageFile));
                rv = MsgNewBufferedFileOutputStream(
                    getter_AddRefs(m_msgFileOutputStream), m_tempMessageFile,
                    -1, 00600);
                NS_ENSURE_SUCCESS(rv, rv);

                bool addDummyEnvelope = false;
                messageUrl->GetAddDummyEnvelope(&addDummyEnvelope);
                if (addDummyEnvelope)
                  SetFlag(MAILBOX_MSG_PARSE_FIRST_LINE);
                else
                  ClearFlag(MAILBOX_MSG_PARSE_FIRST_LINE);
              }
            }
            m_nextState = MAILBOX_READ_MESSAGE;
            break;
          case nsIMailboxUrl::ActionCopyMessage:
          case nsIMailboxUrl::ActionMoveMessage:
          case nsIMailboxUrl::ActionFetchMessage:
            ClearFlag(MAILBOX_MSG_PARSE_FIRST_LINE);
            m_nextState = MAILBOX_READ_MESSAGE;
            break;
          case nsIMailboxUrl::ActionFetchPart:
            m_nextState = MAILBOX_READ_MESSAGE;
            break;
          default:
            break;
        }
      }

      rv = nsMsgProtocol::LoadUrl(aURL, m_channelListener);

    }  // if we received an MAILBOX url...
  }    // if we received a url!

  return rv;
}

int32_t nsMailboxProtocol::ReadFolderResponse(nsIInputStream *inputStream,
                                              uint64_t sourceOffset,
                                              uint32_t length) {
  // okay we are doing a folder read in 8K chunks of a mail folder....
  // this is almost too easy....we can just forward the data in this stream on
  // to our folder parser object!!!

  nsresult rv = NS_OK;
  mCurrentProgress += length;

  if (m_mailboxParser) {
    rv = m_mailboxParser->OnDataAvailable(
        nullptr, inputStream, sourceOffset,
        length);  // let the parser deal with it...
  }
  if (NS_FAILED(rv)) {
    m_nextState = MAILBOX_ERROR_DONE;  // drop out of the loop....
    return -1;
  }

  // now wait for the next 8K chunk to come in.....
  SetFlag(MAILBOX_PAUSE_FOR_READ);

  // leave our state alone so when the next chunk of the mailbox comes in we
  // jump to this state and repeat....how does this process end? Well when the
  // file is done being read in, core net lib will issue an ::OnStopRequest to
  // us...we'll use that as our sign to drop out of this state and to close the
  // protocol instance...

  return 0;
}

int32_t nsMailboxProtocol::ReadMessageResponse(nsIInputStream *inputStream,
                                               uint64_t sourceOffset,
                                               uint32_t length) {
  char *line = nullptr;
  uint32_t status = 0;
  nsresult rv = NS_OK;
  mCurrentProgress += length;

  // if we are doing a move or a copy, forward the data onto the copy handler...
  // if we want to display the message then parse the incoming data...

  if (m_channelListener) {
    // just forward the data we read in to the listener...
    rv = m_channelListener->OnDataAvailable(this, inputStream, sourceOffset,
                                            length);
  } else {
    bool pauseForMoreData = false;
    bool canonicalLineEnding = false;
    nsCOMPtr<nsIMsgMessageUrl> msgurl = do_QueryInterface(m_runningUrl);

    if (msgurl) msgurl->GetCanonicalLineEnding(&canonicalLineEnding);

    while ((line = m_lineStreamBuffer->ReadNextLine(inputStream, status,
                                                    pauseForMoreData)) &&
           !pauseForMoreData) {
      /* When we're sending this line to a converter (ie,
      it's a message/rfc822) use the local line termination
      convention, not CRLF.  This makes text articles get
      saved with the local line terminators.  Since SMTP
      and NNTP mandate the use of CRLF, it is expected that
      the local system will convert that to the local line
      terminator as it is read.
      */
      // mscott - the firstline hack is aimed at making sure we don't write
      // out the dummy header when we are trying to display the message.
      // The dummy header is the From line with the date tag on it.
      if (m_msgFileOutputStream && TestFlag(MAILBOX_MSG_PARSE_FIRST_LINE)) {
        uint32_t count = 0;
        rv = m_msgFileOutputStream->Write(line, PL_strlen(line), &count);
        if (NS_FAILED(rv)) break;

        if (canonicalLineEnding)
          rv = m_msgFileOutputStream->Write(CRLF, 2, &count);
        else
          rv = m_msgFileOutputStream->Write(MSG_LINEBREAK, MSG_LINEBREAK_LEN,
                                            &count);

        if (NS_FAILED(rv)) break;
      } else
        SetFlag(MAILBOX_MSG_PARSE_FIRST_LINE);
      PR_Free(line);
    }
    PR_Free(line);
  }

  SetFlag(MAILBOX_PAUSE_FOR_READ);  // wait for more data to become available...
  if (mProgressEventSink && m_runningUrl) {
    int64_t maxProgress;
    nsCOMPtr<nsIMsgMailNewsUrl> mailnewsUrl(do_QueryInterface(m_runningUrl));
    mailnewsUrl->GetMaxProgress(&maxProgress);
    mProgressEventSink->OnProgress(this, mCurrentProgress, maxProgress);
  }

  if (NS_FAILED(rv)) return -1;

  return 0;
}

/*
 * returns negative if the transfer is finished or error'd out
 *
 * returns zero or more if the transfer needs to be continued.
 */
nsresult nsMailboxProtocol::ProcessProtocolState(nsIURI *url,
                                                 nsIInputStream *inputStream,
                                                 uint64_t offset,
                                                 uint32_t length) {
  nsresult rv = NS_OK;
  int32_t status = 0;
  ClearFlag(MAILBOX_PAUSE_FOR_READ); /* already paused; reset */

  while (!TestFlag(MAILBOX_PAUSE_FOR_READ)) {
    switch (m_nextState) {
      case MAILBOX_READ_MESSAGE:
        if (inputStream == nullptr)
          SetFlag(MAILBOX_PAUSE_FOR_READ);
        else
          status = ReadMessageResponse(inputStream, offset, length);
        break;
      case MAILBOX_READ_FOLDER:
        if (inputStream == nullptr)
          SetFlag(MAILBOX_PAUSE_FOR_READ);  // wait for file socket to read in
                                            // the next chunk...
        else
          status = ReadFolderResponse(inputStream, offset, length);
        break;
      case MAILBOX_DONE:
      case MAILBOX_ERROR_DONE: {
        nsCOMPtr<nsIMsgMailNewsUrl> anotherUrl =
            do_QueryInterface(m_runningUrl);
        rv = m_nextState == MAILBOX_DONE ? NS_OK : NS_ERROR_FAILURE;
        anotherUrl->SetUrlState(false, rv);
        m_nextState = MAILBOX_FREE;
      } break;

      case MAILBOX_FREE:
        // MAILBOX is a one time use connection so kill it if we get here...
        CloseSocket();
        return rv; /* final end */

      default: /* should never happen !!! */
        m_nextState = MAILBOX_ERROR_DONE;
        break;
    }

    /* check for errors during load and call error
     * state if found
     */
    if (status < 0 && m_nextState != MAILBOX_FREE) {
      m_nextState = MAILBOX_ERROR_DONE;
      /* don't exit! loop around again and do the free case */
      ClearFlag(MAILBOX_PAUSE_FOR_READ);
    }
  } /* while(!MAILBOX_PAUSE_FOR_READ) */

  return rv;
}

nsresult nsMailboxProtocol::CloseSocket() {
  // how do you force a release when closing the connection??
  nsMsgProtocol::CloseSocket();
  m_runningUrl = nullptr;
  m_mailboxParser = nullptr;
  return NS_OK;
}

// vim: ts=2 sw=2
