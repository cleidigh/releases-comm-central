/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

var { Services } = ChromeUtils.import("resource://gre/modules/Services.jsm");
var { cal } = ChromeUtils.import("resource:///modules/calendar/calUtils.jsm");
var { setTimeout } = ChromeUtils.import("resource://gre/modules/Timer.jsm");

var { CalDavLegacySAXRequest } = ChromeUtils.import("resource:///modules/caldav/CalDavRequest.jsm");

/* exported CalDavEtagsHandler, CalDavWebDavSyncHandler, CalDavMultigetSyncHandler */

const EXPORTED_SYMBOLS = [
  "CalDavEtagsHandler",
  "CalDavWebDavSyncHandler",
  "CalDavMultigetSyncHandler",
];

const XML_HEADER = '<?xml version="1.0" encoding="UTF-8"?>\n';
const MIME_TEXT_XML = "text/xml; charset=utf-8";

/**
 * This is a handler for the etag request in calDavCalendar.js' getUpdatedItem.
 * It uses the SAX parser to incrementally parse the items and compose the
 * resulting multiget.
 *
 * @param {calDavCalendar} aCalendar - The (unwrapped) calendar this request belongs to.
 * @param {nsIURI} aBaseUri - The URI requested (i.e inbox or collection).
 * @param {*=} aChangeLogListener - (optional) for cached calendars, the listener to notify.
 */
function CalDavEtagsHandler(aCalendar, aBaseUri, aChangeLogListener) {
  this.calendar = aCalendar;
  this.baseUri = aBaseUri;
  this.changeLogListener = aChangeLogListener;
  this._reader = Cc["@mozilla.org/saxparser/xmlreader;1"].createInstance(Ci.nsISAXXMLReader);
  this._reader.contentHandler = this;
  this._reader.errorHandler = this;
  this._reader.parseAsync(null);

  this.itemsReported = {};
  this.itemsNeedFetching = [];
}

CalDavEtagsHandler.prototype = {
  skipIndex: -1,
  currentResponse: null,
  tag: null,
  calendar: null,
  baseUri: null,
  changeLogListener: null,
  logXML: "",

  itemsReported: null,
  itemsNeedFetching: null,

  QueryInterface: cal.generateQI([
    Ci.nsISAXContentHandler,
    Ci.nsISAXErrorHandler,
    Ci.nsIRequestObserver,
    Ci.nsIStreamListener,
  ]),

  /**
   * @see nsIRequestObserver
   */
  onStartRequest(request) {
    let httpchannel = request.QueryInterface(Ci.nsIHttpChannel);

    let responseStatus;
    try {
      responseStatus = httpchannel.responseStatus;
    } catch (ex) {
      cal.WARN("CalDAV: No response status getting etags for calendar " + this.calendar.name);
    }

    if (responseStatus == 207) {
      // We only need to parse 207's, anything else is probably a
      // server error (i.e 50x).
      httpchannel.contentType = "application/xml";
      this._reader.onStartRequest(request);
    } else {
      cal.LOG("CalDAV: Error fetching item etags");
      this.calendar.reportDavError(Ci.calIErrors.DAV_REPORT_ERROR);
      if (this.calendar.isCached && this.changeLogListener) {
        this.changeLogListener.onResult({ status: Cr.NS_ERROR_FAILURE }, Cr.NS_ERROR_FAILURE);
      }
      this._reader = null;
    }
  },

  async onStopRequest(request, statusCode) {
    if (this.calendar.verboseLogging()) {
      cal.LOG("CalDAV: recv: " + this.logXML);
    }
    if (!this._reader) {
      // No reader means there was a request error
      return;
    }
    try {
      this._reader.onStopRequest(request, statusCode);
    } finally {
      this._reader = null;
    }

    // Now that we are done, check which items need fetching.
    this.calendar.superCalendar.startBatch();

    let needsRefresh = false;
    try {
      for (let path in this.calendar.mHrefIndex) {
        if (path in this.itemsReported || path.substr(0, this.baseUri.length) == this.baseUri) {
          // If the item is also on the server, check the next.
          continue;
        }
        // If an item has been deleted from the server, delete it here too.
        // Since the target calendar's operations are synchronous, we can
        // safely set variables from this function.
        let pcal = cal.async.promisifyCalendar(this.calendar.mOfflineStorage);
        let foundItem = (await pcal.getItem(this.calendar.mHrefIndex[path]))[0];

        if (foundItem) {
          let wasInboxItem = this.calendar.mItemInfoCache[foundItem.id].isInboxItem;
          if (
            (wasInboxItem && this.calendar.isInbox(this.baseUri.spec)) ||
            (wasInboxItem === false && !this.calendar.isInbox(this.baseUri.spec))
          ) {
            cal.LOG("Deleting local href: " + path);
            delete this.calendar.mHrefIndex[path];
            await pcal.deleteItem(foundItem);
            needsRefresh = true;
          }
        }
      }
    } finally {
      this.calendar.superCalendar.endBatch();
    }

    // Avoid sending empty multiget requests update views if something has
    // been deleted server-side.
    if (this.itemsNeedFetching.length) {
      let multiget = new CalDavMultigetSyncHandler(
        this.itemsNeedFetching,
        this.calendar,
        this.baseUri,
        null,
        false,
        null,
        this.changeLogListener
      );
      multiget.doMultiGet();
    } else {
      if (this.calendar.isCached && this.changeLogListener) {
        this.changeLogListener.onResult({ status: Cr.NS_OK }, Cr.NS_OK);
      }

      if (needsRefresh) {
        this.calendar.mObservers.notify("onLoad", [this.calendar]);
      }

      // but do poll the inbox
      if (this.calendar.mShouldPollInbox && !this.calendar.isInbox(this.baseUri.spec)) {
        this.calendar.pollInbox();
      }
    }
  },

  /**
   * @see nsIStreamListener
   */
  onDataAvailable(request, inputStream, offset, count) {
    if (this._reader) {
      // No reader means request error
      this._reader.onDataAvailable(request, inputStream, offset, count);
    }
  },

  /**
   * @see nsISAXErrorHandler
   */
  fatalError() {
    cal.WARN("CalDAV: Fatal Error parsing etags for " + this.calendar.name);
  },

  /**
   * @see nsISAXContentHandler
   */
  characters(aValue) {
    if (this.calendar.verboseLogging()) {
      this.logXML += aValue;
    }
    if (this.tag) {
      this.currentResponse[this.tag] += aValue;
    }
  },

  startDocument() {
    this.hrefMap = {};
    this.currentResponse = {};
    this.tag = null;
  },

  endDocument() {},

  startElement(aUri, aLocalName, aQName, aAttributes) {
    switch (aLocalName) {
      case "response":
        this.currentResponse = {};
        this.currentResponse.isCollection = false;
        this.tag = null;
        break;
      case "collection":
        this.currentResponse.isCollection = true;
      // falls through
      case "href":
      case "getetag":
      case "getcontenttype":
        this.tag = aLocalName;
        this.currentResponse[aLocalName] = "";
        break;
    }
    if (this.calendar.verboseLogging()) {
      this.logXML += "<" + aQName + ">";
    }
  },

  endElement(aUri, aLocalName, aQName) {
    switch (aLocalName) {
      case "response": {
        this.tag = null;
        let resp = this.currentResponse;
        if (
          resp.getetag &&
          resp.getetag.length &&
          resp.href &&
          resp.href.length &&
          resp.getcontenttype &&
          resp.getcontenttype.length &&
          !resp.isCollection
        ) {
          resp.href = this.calendar.ensureDecodedPath(resp.href);

          if (resp.getcontenttype.substr(0, 14) == "message/rfc822") {
            // workaround for a Scalix bug which causes incorrect
            // contenttype to be returned.
            resp.getcontenttype = "text/calendar";
          }
          if (resp.getcontenttype == "text/vtodo") {
            // workaround Kerio wierdness
            resp.getcontenttype = "text/calendar";
          }

          // Only handle calendar items
          if (resp.getcontenttype.substr(0, 13) == "text/calendar") {
            if (resp.href && resp.href.length) {
              this.itemsReported[resp.href] = resp.getetag;

              let itemUid = this.calendar.mHrefIndex[resp.href];
              if (!itemUid || resp.getetag != this.calendar.mItemInfoCache[itemUid].etag) {
                this.itemsNeedFetching.push(resp.href);
              }
            }
          }
        }
        break;
      }
      case "href":
      case "getetag":
      case "getcontenttype": {
        this.tag = null;
        break;
      }
    }
    if (this.calendar.verboseLogging()) {
      this.logXML += "</" + aQName + ">";
    }
  },

  processingInstruction(aTarget, aData) {},
};

/**
 * This is a handler for the webdav sync request in calDavCalendar.js' getUpdatedItem.
 * It uses the SAX parser to incrementally parse the items and compose the
 * resulting multiget.
 *
 * @param {calDavCalendar} aCalendar - The (unwrapped) calendar this request belongs to.
 * @param {nsIURI} aBaseUri - The URI requested (i.e inbox or collection).
 * @param {*=} aChangeLogListener - (optional) for cached calendars, the listener to notify.
 */
function CalDavWebDavSyncHandler(aCalendar, aBaseUri, aChangeLogListener) {
  this.calendar = aCalendar;
  this.baseUri = aBaseUri;
  this.changeLogListener = aChangeLogListener;
  this._reader = Cc["@mozilla.org/saxparser/xmlreader;1"].createInstance(Ci.nsISAXXMLReader);
  this._reader.contentHandler = this;
  this._reader.errorHandler = this;
  this._reader.parseAsync(null);

  this.itemsReported = {};
  this.itemsNeedFetching = [];
}

CalDavWebDavSyncHandler.prototype = {
  currentResponse: null,
  tag: null,
  calendar: null,
  baseUri: null,
  newSyncToken: null,
  changeLogListener: null,
  logXML: "",
  isInPropStat: false,
  changeCount: 0,
  unhandledErrors: 0,
  itemsReported: null,
  itemsNeedFetching: null,
  additionalSyncNeeded: false,

  QueryInterface: cal.generateQI([
    Ci.nsISAXContentHandler,
    Ci.nsISAXErrorHandler,
    Ci.nsIRequestObserver,
    Ci.nsIStreamListener,
  ]),

  doWebDAVSync() {
    if (this.calendar.mDisabled) {
      // check if maybe our calendar has become available
      this.calendar.checkDavResourceType(this.changeLogListener);
      return;
    }

    let syncTokenString = "<sync-token/>";
    if (this.calendar.mWebdavSyncToken && this.calendar.mWebdavSyncToken.length > 0) {
      let syncToken = cal.xml.escapeString(this.calendar.mWebdavSyncToken);
      syncTokenString = "<sync-token>" + syncToken + "</sync-token>";
    }

    let queryXml =
      XML_HEADER +
      '<sync-collection xmlns="DAV:">' +
      syncTokenString +
      "<sync-level>1</sync-level>" +
      "<prop>" +
      "<getcontenttype/>" +
      "<getetag/>" +
      "</prop>" +
      "</sync-collection>";

    let requestUri = this.calendar.makeUri(null, this.baseUri);

    if (this.calendar.verboseLogging()) {
      cal.LOG("CalDAV: send(" + requestUri.spec + "): " + queryXml);
    }
    cal.LOG("CalDAV: webdav-sync Token: " + this.calendar.mWebdavSyncToken);

    let onSetupChannel = channel => {
      // The depth header adheres to an older version of the webdav-sync
      // spec and has been replaced by the <sync-level> tag above.
      // Unfortunately some servers still depend on the depth header,
      // therefore we send both (yuck).
      channel.setRequestHeader("Depth", "1", false);
      channel.requestMethod = "REPORT";
    };
    let request = new CalDavLegacySAXRequest(
      this.calendar.session,
      this.calendar,
      requestUri,
      queryXml,
      MIME_TEXT_XML,
      this,
      onSetupChannel
    );

    request.commit().catch(() => {
      // Something went wrong with the OAuth token, notify failure
      if (this.calendar.isCached && this.changeLogListener) {
        this.changeLogListener.onResult(
          { status: Cr.NS_ERROR_NOT_AVAILABLE },
          Cr.NS_ERROR_NOT_AVAILABLE
        );
      }
    });
  },

  /**
   * @see nsIRequestObserver
   */
  onStartRequest(request) {
    let httpchannel = request.QueryInterface(Ci.nsIHttpChannel);

    let responseStatus;
    try {
      responseStatus = httpchannel.responseStatus;
    } catch (ex) {
      cal.WARN("CalDAV: No response status doing webdav sync for calendar " + this.calendar.name);
    }

    if (responseStatus == 207) {
      // We only need to parse 207's, anything else is probably a
      // server error (i.e 50x).
      httpchannel.contentType = "application/xml";
      this._reader.onStartRequest(request);
    } else if (
      this.calendar.mWebdavSyncToken != null &&
      responseStatus >= 400 &&
      responseStatus <= 499
    ) {
      // Invalidate sync token with 4xx errors that could indicate the
      // sync token has become invalid and do a refresh
      cal.LOG(
        "CalDAV: Resetting sync token because server returned status code: " + responseStatus
      );
      this._reader = null;
      this.calendar.mWebdavSyncToken = null;
      this.calendar.saveCalendarProperties();
      this.calendar.safeRefresh(this.changeLogListener);
    } else {
      cal.WARN("CalDAV: Error doing webdav sync: " + responseStatus);
      this.calendar.reportDavError(Ci.calIErrors.DAV_REPORT_ERROR);
      if (this.calendar.isCached && this.changeLogListener) {
        this.changeLogListener.onResult({ status: Cr.NS_ERROR_FAILURE }, Cr.NS_ERROR_FAILURE);
      }
      this._reader = null;
    }
  },

  onStopRequest(request, statusCode) {
    if (this.calendar.verboseLogging()) {
      cal.LOG("CalDAV: recv: " + this.logXML);
    }
    if (!this._reader) {
      // No reader means there was a request error
      cal.LOG("CalDAV: onStopRequest: no reader");
      return;
    }
    try {
      this._reader.onStopRequest(request, statusCode);
    } finally {
      this._reader = null;
    }
  },

  /**
   * @see nsIStreamListener
   */
  onDataAvailable(request, inputStream, offset, count) {
    if (this._reader) {
      // No reader means request error
      this._reader.onDataAvailable(request, inputStream, offset, count);
    }
  },

  /**
   * @see nsISAXErrorHandler
   */
  fatalError() {
    cal.WARN("CalDAV: Fatal Error doing webdav sync for " + this.calendar.name);
  },

  /**
   * @see nsISAXContentHandler
   */
  characters(aValue) {
    if (this.calendar.verboseLogging()) {
      this.logXML += aValue;
    }
    this.currentResponse[this.tag] += aValue;
  },

  startDocument() {
    this.hrefMap = {};
    this.currentResponse = {};
    this.tag = null;
    this.calendar.superCalendar.startBatch();
  },

  endDocument() {
    if (this.unhandledErrors) {
      this.calendar.superCalendar.endBatch();
      this.calendar.reportDavError(Ci.calIErrors.DAV_REPORT_ERROR);
      if (this.calendar.isCached && this.changeLogListener) {
        this.changeLogListener.onResult({ status: Cr.NS_ERROR_FAILURE }, Cr.NS_ERROR_FAILURE);
      }
      return;
    }

    if (this.calendar.mWebdavSyncToken == null) {
      // null token means reset or first refresh indicating we did
      // a full sync; remove local items that were not returned in this full
      // sync
      for (let path in this.calendar.mHrefIndex) {
        if (!this.itemsReported[path]) {
          this.calendar.deleteTargetCalendarItem(path);
        }
      }
    }
    this.calendar.superCalendar.endBatch();

    if (this.itemsNeedFetching.length) {
      let multiget = new CalDavMultigetSyncHandler(
        this.itemsNeedFetching,
        this.calendar,
        this.baseUri,
        this.newSyncToken,
        this.additionalSyncNeeded,
        null,
        this.changeLogListener
      );
      multiget.doMultiGet();
    } else {
      if (this.newSyncToken) {
        this.calendar.mWebdavSyncToken = this.newSyncToken;
        this.calendar.saveCalendarProperties();
        cal.LOG("CalDAV: New webdav-sync Token: " + this.calendar.mWebdavSyncToken);
      }
      this.calendar.finalizeUpdatedItems(this.changeLogListener, this.baseUri);
    }
  },

  startElement(aUri, aLocalName, aQName, aAttributes) {
    switch (aLocalName) {
      case "response": // WebDAV Sync draft 3
        this.currentResponse = {};
        this.tag = null;
        this.isInPropStat = false;
        break;
      case "propstat":
        this.isInPropStat = true;
        break;
      case "status":
        if (this.isInPropStat) {
          this.tag = "propstat_" + aLocalName;
        } else {
          this.tag = aLocalName;
        }
        this.currentResponse[this.tag] = "";
        break;
      case "href":
      case "getetag":
      case "getcontenttype":
      case "sync-token":
        this.tag = aLocalName.replace(/-/g, "");
        this.currentResponse[this.tag] = "";
        break;
    }
    if (this.calendar.verboseLogging()) {
      this.logXML += "<" + aQName + ">";
    }
  },

  endElement(aUri, aLocalName, aQName) {
    switch (aLocalName) {
      case "response": // WebDAV Sync draft 3
      case "sync-response": {
        // WebDAV Sync draft 0,1,2
        let resp = this.currentResponse;
        if (resp.href && resp.href.length) {
          resp.href = this.calendar.ensureDecodedPath(resp.href);
        }

        if (
          (!resp.getcontenttype || resp.getcontenttype == "text/plain") &&
          resp.href &&
          resp.href.endsWith(".ics")
        ) {
          // If there is no content-type (iCloud) or text/plain was passed
          // (iCal Server) for the resource but its name ends with ".ics"
          // assume the content type to be text/calendar. Apple
          // iCloud/iCal Server interoperability fix.
          resp.getcontenttype = "text/calendar";
        }

        // Deleted item
        if (
          resp.href &&
          resp.href.length &&
          resp.status &&
          resp.status.length &&
          resp.status.indexOf(" 404") > 0
        ) {
          if (this.calendar.mHrefIndex[resp.href]) {
            this.changeCount++;
            this.calendar.deleteTargetCalendarItem(resp.href);
          } else {
            cal.LOG("CalDAV: skipping unfound deleted item : " + resp.href);
          }
          // Only handle Created or Updated calendar items
        } else if (
          resp.getcontenttype &&
          resp.getcontenttype.substr(0, 13) == "text/calendar" &&
          resp.getetag &&
          resp.getetag.length &&
          resp.href &&
          resp.href.length &&
          (!resp.status || // Draft 3 does not require
          resp.status.length == 0 || // a status for created or updated items but
          resp.status.indexOf(" 204") || // draft 0, 1 and 2 needed it so treat no status
          resp.status.indexOf(" 200") || // Apple iCloud returns 200 status for each item
            resp.status.indexOf(" 201"))
        ) {
          // and status 201 and 204 the same
          this.itemsReported[resp.href] = resp.getetag;
          let itemId = this.calendar.mHrefIndex[resp.href];
          let oldEtag = itemId && this.calendar.mItemInfoCache[itemId].etag;

          if (!oldEtag || oldEtag != resp.getetag) {
            // Etag mismatch, getting new/updated item.
            this.itemsNeedFetching.push(resp.href);
          }
        } else if (resp.status && resp.status.includes(" 507")) {
          // webdav-sync says that if a 507 is encountered and the
          // url matches the request, the current token should be
          // saved and another request should be made. We don't
          // actually compare the URL, its too easy to get this
          // wrong.

          // The 507 doesn't mean the data received is invalid, so
          // continue processing.
          this.additionalSyncNeeded = true;
        } else if (
          resp.status &&
          resp.status.indexOf(" 200") &&
          resp.href &&
          resp.href.endsWith("/")
        ) {
          // iCloud returns status responses for directories too
          // so we just ignore them if they have status code 200. We
          // want to make sure these are not counted as unhandled
          // errors in the next block
        } else if (
          (resp.getcontenttype && resp.getcontenttype.startsWith("text/calendar")) ||
          (resp.status && !resp.status.includes(" 404"))
        ) {
          // If the response element is still not handled, log an
          // error only if the content-type is text/calendar or the
          // response status is different than 404 not found.  We
          // don't care about response elements on non-calendar
          // resources or whose status is not indicating a deleted
          // resource.
          cal.WARN("CalDAV: Unexpected response, status: " + resp.status + ", href: " + resp.href);
          this.unhandledErrors++;
        } else {
          cal.LOG(
            "CalDAV: Unhandled response element, status: " +
              resp.status +
              ", href: " +
              resp.href +
              " contenttype:" +
              resp.getcontenttype
          );
        }
        break;
      }
      case "sync-token": {
        this.newSyncToken = this.currentResponse[this.tag];
        break;
      }
      case "propstat": {
        this.isInPropStat = false;
        break;
      }
    }
    this.tag = null;
    if (this.calendar.verboseLogging()) {
      this.logXML += "</" + aQName + ">";
    }
  },

  processingInstruction(aTarget, aData) {},
};

/**
 * This is a handler for the multiget request.
 * It uses the SAX parser to incrementally parse the items and compose the
 * resulting multiget.
 *
 * @param {String[]} aItemsNeedFetching - Array of items to fetch, an array of
 *                                        un-encoded paths.
 * @param {calDavCalendar} aCalendar - The (unwrapped) calendar this request belongs to.
 * @param {nsIURI} aBaseUri - The URI requested (i.e inbox or collection).
 * @param {*=} aNewSyncToken - (optional) New Sync token to set if operation successful.
 * @param {Boolean=} aAdditionalSyncNeeded - (optional) If true, the passed sync token is not the
 *                                           latest, another webdav sync run should be
 *                                           done after completion.
 * @param {*=} aListener - (optional) The listener to notify.
 * @param {*=} aChangeLogListener - (optional) For cached calendars, the listener to
 *                                  notify.
 */
function CalDavMultigetSyncHandler(
  aItemsNeedFetching,
  aCalendar,
  aBaseUri,
  aNewSyncToken,
  aAdditionalSyncNeeded,
  aListener,
  aChangeLogListener
) {
  this.calendar = aCalendar;
  this.baseUri = aBaseUri;
  this.listener = aListener;
  this.newSyncToken = aNewSyncToken;
  this.changeLogListener = aChangeLogListener;
  this._reader = Cc["@mozilla.org/saxparser/xmlreader;1"].createInstance(Ci.nsISAXXMLReader);
  this._reader.contentHandler = this;
  this._reader.errorHandler = this;
  this._reader.parseAsync(null);
  this.itemsNeedFetching = aItemsNeedFetching;
  this.additionalSyncNeeded = aAdditionalSyncNeeded;
}
CalDavMultigetSyncHandler.prototype = {
  currentResponse: null,
  tag: null,
  calendar: null,
  baseUri: null,
  newSyncToken: null,
  listener: null,
  changeLogListener: null,
  logXML: null,
  unhandledErrors: 0,
  itemsNeedFetching: null,
  additionalSyncNeeded: false,
  timer: null,

  QueryInterface: cal.generateQI([
    Ci.nsISAXContentHandler,
    Ci.nsISAXErrorHandler,
    Ci.nsIRequestObserver,
    Ci.nsIStreamListener,
  ]),

  doMultiGet() {
    if (this.calendar.mDisabled) {
      // check if maybe our calendar has become available
      this.calendar.checkDavResourceType(this.changeLogListener);
      return;
    }

    let batchSize = Services.prefs.getIntPref("calendar.caldav.multigetBatchSize", 100);
    let hrefString = "";
    while (this.itemsNeedFetching.length && batchSize > 0) {
      batchSize--;
      // ensureEncodedPath extracts only the path component of the item and
      // encodes it before it is sent to the server
      let locpath = this.calendar.ensureEncodedPath(this.itemsNeedFetching.pop());
      hrefString += "<D:href>" + cal.xml.escapeString(locpath) + "</D:href>";
    }

    let queryXml =
      XML_HEADER +
      '<C:calendar-multiget xmlns:D="DAV:" xmlns:C="urn:ietf:params:xml:ns:caldav">' +
      "<D:prop>" +
      "<D:getetag/>" +
      "<C:calendar-data/>" +
      "</D:prop>" +
      hrefString +
      "</C:calendar-multiget>";

    let requestUri = this.calendar.makeUri(null, this.baseUri);
    if (this.calendar.verboseLogging()) {
      cal.LOG("CalDAV: send(" + requestUri.spec + "): " + queryXml);
    }

    let onSetupChannel = channel => {
      channel.requestMethod = "REPORT";
      channel.setRequestHeader("Depth", "1", false);
    };
    let request = new CalDavLegacySAXRequest(
      this.calendar.session,
      this.calendar,
      requestUri,
      queryXml,
      MIME_TEXT_XML,
      this,
      onSetupChannel
    );

    request.commit().catch(() => {
      // Something went wrong with the OAuth token, notify failure
      if (this.calendar.isCached && this.changeLogListener) {
        this.changeLogListener.onResult(
          { status: Cr.NS_ERROR_NOT_AVAILABLE },
          Cr.NS_ERROR_NOT_AVAILABLE
        );
      }
    });
  },

  /**
   * @see nsIRequestObserver
   */
  onStartRequest(request) {
    let httpchannel = request.QueryInterface(Ci.nsIHttpChannel);

    let responseStatus;
    try {
      responseStatus = httpchannel.responseStatus;
    } catch (ex) {
      cal.WARN("CalDAV: No response status doing multiget for calendar " + this.calendar.name);
    }

    if (responseStatus == 207) {
      // We only need to parse 207's, anything else is probably a
      // server error (i.e 50x).
      httpchannel.contentType = "application/xml";
      this._reader.onStartRequest(request);
    } else {
      let errorMsg =
        "CalDAV: Error: got status " +
        responseStatus +
        " fetching calendar data for " +
        this.calendar.name +
        ", " +
        this.listener;
      this.calendar.notifyGetFailed(errorMsg, this.listener, this.changeLogListener);
      this._reader = null;
    }
  },

  onStopRequest(request, statusCode) {
    if (this.calendar.verboseLogging()) {
      cal.LOG("CalDAV: recv: " + this.logXML);
    }
    if (this.unhandledErrors) {
      this.calendar.superCalendar.endBatch();
      this.calendar.notifyGetFailed("multiget error", this.listener, this.changeLogListener);
      return;
    }
    if (this.itemsNeedFetching.length == 0) {
      if (this.newSyncToken) {
        this.calendar.mWebdavSyncToken = this.newSyncToken;
        this.calendar.saveCalendarProperties();
        cal.LOG("CalDAV: New webdav-sync Token: " + this.calendar.mWebdavSyncToken);
      }

      if (this.additionalSyncNeeded) {
        setTimeout(() => {
          let wds = new CalDavWebDavSyncHandler(
            this.calendar,
            this.baseUri,
            this.changeLogListener
          );
          wds.doWebDAVSync();
        }, 0);
      } else {
        this.calendar.finalizeUpdatedItems(this.changeLogListener, this.baseUri);
      }
    }
    if (!this._reader) {
      // No reader means there was a request error. The error is already
      // notified in onStartRequest, so no need to do it here.
      cal.LOG("CalDAV: onStopRequest: no reader");
      return;
    }
    try {
      this._reader.onStopRequest(request, statusCode);
    } finally {
      this._reader = null;
    }
    if (this.itemsNeedFetching.length > 0) {
      cal.LOG("CalDAV: Still need to fetch " + this.itemsNeedFetching.length + " elements.");
      this._reader = Cc["@mozilla.org/saxparser/xmlreader;1"].createInstance(Ci.nsISAXXMLReader);
      this._reader.contentHandler = this;
      this._reader.errorHandler = this;
      this._reader.parseAsync(null);
      let timerCallback = {
        requestHandler: this,
        notify(timer) {
          // Call multiget again to get another batch
          this.requestHandler.doMultiGet();
        },
      };
      this.timer = Cc["@mozilla.org/timer;1"].createInstance(Ci.nsITimer);
      this.timer.initWithCallback(timerCallback, 0, Ci.nsITimer.TYPE_ONE_SHOT);
    }
  },

  /**
   * @see nsIStreamListener
   */
  onDataAvailable(request, inputStream, offset, count) {
    if (this._reader) {
      // No reader means request error
      this._reader.onDataAvailable(request, inputStream, offset, count);
    }
  },

  /**
   * @see nsISAXErrorHandler
   */
  fatalError(error) {
    cal.WARN("CalDAV: Fatal Error doing multiget for " + this.calendar.name + ": " + error);
  },

  /**
   * @see nsISAXContentHandler
   */
  characters(aValue) {
    if (this.calendar.verboseLogging()) {
      this.logXML += aValue;
    }
    if (this.tag) {
      this.currentResponse[this.tag] += aValue;
    }
  },

  startDocument() {
    this.hrefMap = {};
    this.currentResponse = {};
    this.tag = null;
    this.logXML = "";
    this.calendar.superCalendar.startBatch();
  },

  endDocument() {
    this.calendar.superCalendar.endBatch();
  },

  startElement(aUri, aLocalName, aQName, aAttributes) {
    switch (aLocalName) {
      case "response":
        this.currentResponse = {};
        this.tag = null;
        this.isInPropStat = false;
        break;
      case "propstat":
        this.isInPropStat = true;
        break;
      case "status":
        if (this.isInPropStat) {
          this.tag = "propstat_" + aLocalName;
        } else {
          this.tag = aLocalName;
        }
        this.currentResponse[this.tag] = "";
        break;
      case "calendar-data":
      case "href":
      case "getetag":
        this.tag = aLocalName.replace(/-/g, "");
        this.currentResponse[this.tag] = "";
        break;
    }
    if (this.calendar.verboseLogging()) {
      this.logXML += "<" + aQName + ">";
    }
  },

  endElement(aUri, aLocalName, aQName) {
    switch (aLocalName) {
      case "response": {
        let resp = this.currentResponse;
        if (resp.href && resp.href.length) {
          resp.href = this.calendar.ensureDecodedPath(resp.href);
        }
        if (
          resp.href &&
          resp.href.length &&
          resp.status &&
          resp.status.length &&
          resp.status.indexOf(" 404") > 0
        ) {
          if (this.calendar.mHrefIndex[resp.href]) {
            this.calendar.deleteTargetCalendarItem(resp.href);
          } else {
            cal.LOG("CalDAV: skipping unfound deleted item : " + resp.href);
          }
          // Created or Updated item
        } else if (
          resp.getetag &&
          resp.getetag.length &&
          resp.href &&
          resp.href.length &&
          resp.calendardata &&
          resp.calendardata.length
        ) {
          let oldEtag;
          let itemId = this.calendar.mHrefIndex[resp.href];
          if (itemId) {
            oldEtag = this.calendar.mItemInfoCache[itemId].etag;
          } else {
            oldEtag = null;
          }
          if (!oldEtag || oldEtag != resp.getetag) {
            this.calendar.addTargetCalendarItem(
              resp.href,
              resp.calendardata,
              this.baseUri,
              resp.getetag,
              this.listener
            );
          } else {
            cal.LOG("CalDAV: skipping item with unmodified etag : " + oldEtag);
          }
        } else {
          cal.WARN(
            "CalDAV: Unexpected response, status: " +
              resp.status +
              ", href: " +
              resp.href +
              " calendar-data:\n" +
              resp.calendardata
          );
          this.unhandledErrors++;
        }
        break;
      }
      case "propstat": {
        this.isInPropStat = false;
        break;
      }
    }
    this.tag = null;
    if (this.calendar.verboseLogging()) {
      this.logXML += "</" + aQName + ">";
    }
  },

  processingInstruction(aTarget, aData) {},
};
