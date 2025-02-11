/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

const EXPORTED_SYMBOLS = [
  "getCurrentTheme",
  "getThemeByName",
  "getHTMLForMessage",
  "getThemeVariants",
  "isNextMessage",
  "insertHTMLForMessage",
  "initHTMLDocument",
  "getMessagesForRange",
  "serializeSelection",
];

const { Services } = ChromeUtils.import("resource:///modules/imServices.jsm");
const { DownloadUtils } = ChromeUtils.import(
  "resource://gre/modules/DownloadUtils.jsm"
);
const { XPCOMUtils } = ChromeUtils.import(
  "resource://gre/modules/XPCOMUtils.jsm"
);

var kMessagesStylePrefBranch = "messenger.options.messagesStyle.";
var kThemePref = "theme";
var kVariantPref = "variant";
var kShowHeaderPref = "showHeader";
var kCombineConsecutivePref = "combineConsecutive";
var kCombineConsecutiveIntervalPref = "combineConsecutiveInterval";

var DEFAULT_THEME = "bubbles";
var DEFAULT_THEMES = ["bubbles", "dark", "mail", "papersheets", "simple"];

var kLineBreak = "@mozilla.org/windows-registry-key;1" in Cc ? "\r\n" : "\n";

XPCOMUtils.defineLazyGetter(this, "gPrefBranch", () =>
  Services.prefs.getBranch(kMessagesStylePrefBranch)
);

XPCOMUtils.defineLazyGetter(this, "TXTToHTML", function() {
  let cs = Cc["@mozilla.org/txttohtmlconv;1"].getService(Ci.mozITXTToHTMLConv);
  return aTXT => cs.scanTXT(aTXT, cs.kEntities);
});

XPCOMUtils.defineLazyGetter(this, "gTimeFormatter", () => {
  return new Services.intl.DateTimeFormat(undefined, {
    hour: "2-digit",
    minute: "2-digit",
    second: "2-digit",
  });
});

ChromeUtils.defineModuleGetter(
  this,
  "ToLocaleFormat",
  "resource:///modules/ToLocaleFormat.jsm"
);

var gCurrentTheme = null;

function getChromeFile(aURI) {
  try {
    let channel = Services.io.newChannel(
      aURI,
      null,
      null,
      null,
      Services.scriptSecurityManager.getSystemPrincipal(),
      null,
      Ci.nsILoadInfo.SEC_ALLOW_CROSS_ORIGIN_DATA_IS_NULL,
      Ci.nsIContentPolicy.TYPE_OTHER
    );
    let stream = channel.open();
    let sstream = Cc["@mozilla.org/scriptableinputstream;1"].createInstance(
      Ci.nsIScriptableInputStream
    );
    sstream.init(stream);
    let text = sstream.read(sstream.available());
    sstream.close();
    return text;
  } catch (e) {
    if (e.result != Cr.NS_ERROR_FILE_NOT_FOUND) {
      dump("Getting " + aURI + ": " + e + "\n");
    }
    return null;
  }
}

function HTMLTheme(aBaseURI) {
  let files = {
    footer: "Footer.html",
    header: "Header.html",
    status: "Status.html",
    statusNext: "NextStatus.html",
    incomingContent: "Incoming/Content.html",
    incomingContext: "Incoming/Context.html",
    incomingNextContent: "Incoming/NextContent.html",
    incomingNextContext: "Incoming/NextContext.html",
    outgoingContent: "Outgoing/Content.html",
    outgoingContext: "Outgoing/Context.html",
    outgoingNextContent: "Outgoing/NextContent.html",
    outgoingNextContext: "Outgoing/NextContext.html",
  };

  for (let id in files) {
    let html = getChromeFile(aBaseURI + files[id]);
    if (html) {
      Object.defineProperty(this, id, { value: html });
    }
  }

  if (!("incomingContent" in files)) {
    throw new Error("Invalid theme: Incoming/Content.html is missing!");
  }
}

HTMLTheme.prototype = {
  get footer() {
    return "";
  },
  get header() {
    return "";
  },
  get status() {
    return this.incomingContent;
  },
  get statusNext() {
    return this.status;
  },
  get incomingContent() {
    throw new Error("Incoming/Content.html is a required file");
  },
  get incomingNextContent() {
    return this.incomingContent;
  },
  get outgoingContent() {
    return this.incomingContent;
  },
  get outgoingNextContent() {
    return this.incomingNextContent;
  },
  get incomingContext() {
    return this.incomingContent;
  },
  get incomingNextContext() {
    return this.incomingNextContent;
  },
  get outgoingContext() {
    return this.hasOwnProperty("outgoingContent")
      ? this.outgoingContent
      : this.incomingContext;
  },
  get outgoingNextContext() {
    return this.hasOwnProperty("outgoingNextContent")
      ? this.outgoingNextContent
      : this.incomingNextContext;
  },
};

function plistToJSON(aElt) {
  switch (aElt.localName) {
    case "true":
      return true;
    case "false":
      return false;
    case "string":
    case "data":
      return aElt.textContent;
    case "real":
      return parseFloat(aElt.textContent);
    case "integer":
      return parseInt(aElt.textContent, 10);

    case "dict":
      let res = {};
      let nodes = aElt.children;
      for (let i = 0; i < nodes.length; ++i) {
        if (nodes[i].nodeName == "key") {
          let key = nodes[i].textContent;
          ++i;
          while (!Element.isInstance(nodes[i])) {
            ++i;
          }
          res[key] = plistToJSON(nodes[i]);
        }
      }
      return res;

    case "array":
      let array = [];
      nodes = aElt.children;
      for (let i = 0; i < nodes.length; ++i) {
        if (Element.isInstance(nodes[i])) {
          array.push(plistToJSON(nodes[i]));
        }
      }
      return array;

    default:
      throw new Error("Unknown tag in plist file");
  }
}

function getInfoPlistContent(aBaseURI) {
  try {
    let channel = Services.io.newChannel(
      aBaseURI + "Info.plist",
      null,
      null,
      null,
      Services.scriptSecurityManager.getSystemPrincipal(),
      null,
      Ci.nsILoadInfo.SEC_ALLOW_CROSS_ORIGIN_DATA_IS_NULL,
      Ci.nsIContentPolicy.TYPE_OTHER
    );
    let stream = channel.open();
    let parser = new DOMParser();
    let doc = parser.parseFromStream(
      stream,
      null,
      stream.available(),
      "text/xml"
    );
    if (doc.documentElement.localName != "plist") {
      throw new Error("Invalid Info.plist file");
    }
    let node = doc.documentElement.firstElementChild;
    while (node && !Element.isInstance(node)) {
      node = node.nextElementSibling;
    }
    if (!node || node.localName != "dict") {
      throw new Error("Empty or invalid Info.plist file");
    }
    return plistToJSON(node);
  } catch (e) {
    Cu.reportError(e);
    return null;
  }
}

function getChromeBaseURI(aThemeName) {
  if (DEFAULT_THEMES.includes(aThemeName)) {
    return "chrome://messenger-messagestyles/skin/" + aThemeName + "/";
  }
  return "chrome://" + aThemeName + "/skin/";
}

function getThemeByName(aName) {
  let baseURI = getChromeBaseURI(aName);
  let metadata = getInfoPlistContent(baseURI);
  if (!metadata) {
    throw new Error("Cannot load theme " + aName);
  }

  return {
    name: aName,
    variant: "default",
    baseURI,
    metadata,
    html: new HTMLTheme(baseURI),
    showHeader: gPrefBranch.getBoolPref(kShowHeaderPref),
    combineConsecutive: gPrefBranch.getBoolPref(kCombineConsecutivePref),
    combineConsecutiveInterval: gPrefBranch.getIntPref(
      kCombineConsecutiveIntervalPref
    ),
  };
}

function getCurrentTheme() {
  let name = gPrefBranch.getCharPref(kThemePref);
  let variant = gPrefBranch.getCharPref(kVariantPref);
  if (
    gCurrentTheme &&
    gCurrentTheme.name == name &&
    gCurrentTheme.variant == variant
  ) {
    return gCurrentTheme;
  }

  try {
    gCurrentTheme = getThemeByName(name);
    gCurrentTheme.variant = variant;
  } catch (e) {
    Cu.reportError(e);
    gCurrentTheme = getThemeByName(DEFAULT_THEME);
    gCurrentTheme.variant = "default";
  }

  return gCurrentTheme;
}

function getDirectoryEntries(aDir) {
  let ios = Services.io;
  let uri = ios.newURI(aDir);
  let cr = Cc["@mozilla.org/chrome/chrome-registry;1"].getService(
    Ci.nsIXULChromeRegistry
  );
  while (uri.scheme == "chrome") {
    uri = cr.convertChromeURL(uri);
  }

  // remove any trailing file name added by convertChromeURL
  let spec = uri.spec.replace(/[^\/]+$/, "");
  uri = ios.newURI(spec);

  let results = [];
  if (uri.scheme == "jar") {
    uri.QueryInterface(Ci.nsIJARURI);
    let strEntry = uri.JAREntry;
    if (!strEntry) {
      return [];
    }

    let zr = Cc["@mozilla.org/libjar/zip-reader;1"].createInstance(
      Ci.nsIZipReader
    );
    let jarFile = uri.JARFile;
    if (jarFile instanceof Ci.nsIJARURI) {
      let innerZr = Cc["@mozilla.org/libjar/zip-reader;1"].createInstance(
        Ci.nsIZipReader
      );
      innerZr.open(jarFile.JARFile.QueryInterface(Ci.nsIFileURL).file);
      zr.openInner(innerZr, jarFile.JAREntry);
    } else {
      zr.open(jarFile.QueryInterface(Ci.nsIFileURL).file);
    }

    if (!zr.hasEntry(strEntry) || !zr.getEntry(strEntry).isDirectory) {
      zr.close();
      return [];
    }

    let escapedEntry = strEntry.replace(/([*?$[\]^~()\\])/g, "\\$1");
    let filter = escapedEntry + "?*~" + escapedEntry + "?*/?*";
    let entries = zr.findEntries(filter);

    let parentLength = strEntry.length;
    for (let entry of entries) {
      results.push(entry.substring(parentLength));
    }
    zr.close();
  } else if (uri.scheme == "file") {
    uri.QueryInterface(Ci.nsIFileURL);
    let dir = uri.file;

    if (!dir.exists() || !dir.isDirectory()) {
      return [];
    }

    for (let file of dir.directoryEntries) {
      results.push(file.leafName);
    }
  }

  return results;
}

function getThemeVariants(aTheme) {
  let variants = getDirectoryEntries(aTheme.baseURI + "Variants/");
  return variants
    .filter(v => v.endsWith(".css"))
    .map(v => v.substring(0, v.length - 4));
}

/* helper function for replacements in messages */
function getBuddyFromMessage(aMsg) {
  if (aMsg.incoming) {
    let conv = aMsg.conversation;
    if (!conv.isChat) {
      return conv.buddy;
    }
  }

  return null;
}

function getStatusIconFromBuddy(aBuddy) {
  let status = "unknown";
  if (aBuddy) {
    if (!aBuddy.online) {
      status = "offline";
    } else if (aBuddy.idle) {
      status = "idle";
    } else if (!aBuddy.available) {
      status = "away";
    } else {
      status = "available";
    }
  }

  return "chrome://chat/skin/" + status + "-16.png";
}

var headerFooterReplacements = {
  chatName: aConv => TXTToHTML(aConv.title),
  sourceName: aConv => TXTToHTML(aConv.account.alias || aConv.account.name),
  destinationName: aConv => TXTToHTML(aConv.name),
  destinationDisplayName: aConv => TXTToHTML(aConv.title),
  incomingIconPath(aConv) {
    let buddy;
    return (
      (!aConv.isChat && (buddy = aConv.buddy) && buddy.buddyIconFilename) ||
      "incoming_icon.png"
    );
  },
  outgoingIconPath: aConv => "outgoing_icon.png",
  timeOpened(aConv, aFormat) {
    let date = new Date(aConv.startDate / 1000);
    if (aFormat) {
      return ToLocaleFormat(aFormat, date);
    }
    return gTimeFormatter.format(date);
  },
};

function formatAutoResponce(aTxt) {
  return Services.strings
    .createBundle("chrome://chat/locale/conversations.properties")
    .formatStringFromName("autoReply", [aTxt]);
}

var statusMessageReplacements = {
  message: aMsg =>
    '<span class="ib-msg-txt">' +
    (aMsg.autoResponse ? formatAutoResponce(aMsg.message) : aMsg.message) +
    "</span>",
  time(aMsg, aFormat) {
    let date = new Date(aMsg.time * 1000);
    if (aFormat) {
      return ToLocaleFormat(aFormat, date);
    }
    return gTimeFormatter.format(date);
  },
  timestamp: aMsg => aMsg.time,
  shortTime(aMsg) {
    return gTimeFormatter.format(new Date(aMsg.time * 1000));
  },
  messageClasses(aMsg) {
    let msgClass = [];

    if (aMsg.system) {
      msgClass.push("event");
    } else {
      msgClass.push("message");

      if (aMsg.incoming) {
        msgClass.push("incoming");
      } else if (aMsg.outgoing) {
        msgClass.push("outgoing");
      }

      if (/^(<[^>]+>)*\/me /.test(aMsg.displayMessage)) {
        msgClass.push("action");
      }

      if (aMsg.autoResponse) {
        msgClass.push("autoreply");
      }
    }

    if (aMsg.containsNick) {
      msgClass.push("nick");
    }
    if (aMsg.error) {
      msgClass.push("error");
    }
    if (aMsg.delayed) {
      msgClass.push("delayed");
    }
    if (aMsg.notification) {
      msgClass.push("notification");
    }
    if (aMsg.noFormat) {
      msgClass.push("monospaced");
    }
    if (aMsg.noCollapse) {
      msgClass.push("no-collapse");
    }

    return msgClass.join(" ");
  },
};

function formatSender(aName, isEncrypted = false) {
  let otr = isEncrypted ? " message-encrypted" : "";
  return `<span class="ib-sender${otr}">${TXTToHTML(aName)}</span>`;
}
var messageReplacements = {
  userIconPath(aMsg) {
    // If the protocol plugin provides an icon for the message, use it.
    let iconURL = aMsg.iconURL;
    if (iconURL) {
      return iconURL;
    }

    // For outgoing messages, use the current user icon.
    if (aMsg.outgoing) {
      iconURL = aMsg.conversation.account.statusInfo.getUserIcon();
      if (iconURL) {
        return iconURL.spec;
      }
    }

    // Fallback to the theme's default icons.
    return (aMsg.incoming ? "Incoming" : "Outgoing") + "/buddy_icon.svg";
  },
  senderScreenName: aMsg => formatSender(aMsg.who, aMsg.encrypted),
  sender: aMsg => formatSender(aMsg.alias || aMsg.who, aMsg.encrypted),
  senderColor: aMsg => aMsg.color,
  senderStatusIcon: aMsg => getStatusIconFromBuddy(getBuddyFromMessage(aMsg)),
  messageDirection: aMsg => "ltr",
  // no theme actually use this, don't bother making sure this is the real
  // serverside alias
  senderDisplayName: aMsg =>
    formatSender(aMsg.alias || aMsg.who, aMsg.encrypted),
  service: aMsg => aMsg.conversation.account.protocol.name,
  textbackgroundcolor: (aMsg, aFormat) => "transparent", // FIXME?
  __proto__: statusMessageReplacements,
};

var statusReplacements = {
  status: aMsg => "", // FIXME
  statusIcon(aMsg) {
    let conv = aMsg.conversation;
    let buddy = null;
    if (!conv.isChat) {
      buddy = conv.buddy;
    }
    return getStatusIconFromBuddy(buddy);
  },
  __proto__: statusMessageReplacements,
};

var kReplacementRegExp = /%([a-zA-Z]*)(\{([^\}]*)\})?%/g;

function replaceKeywordsInHTML(aHTML, aReplacements, aReplacementArg) {
  kReplacementRegExp.lastIndex = 0;
  let previousIndex = 0;
  let result = "";
  let match;
  while ((match = kReplacementRegExp.exec(aHTML))) {
    let content = "";
    if (match[1] in aReplacements) {
      content = aReplacements[match[1]](aReplacementArg, match[3]);
    } else {
      Cu.reportError(
        "Unknown replacement string %" + match[1] + "% in message styles."
      );
    }
    result += aHTML.substring(previousIndex, match.index) + content;
    previousIndex = kReplacementRegExp.lastIndex;
  }

  return result + aHTML.slice(previousIndex);
}

function isNextMessage(aTheme, aMsg, aPreviousMsg) {
  if (
    !aTheme.combineConsecutive ||
    (hasMetadataKey(aTheme, "DisableCombineConsecutive") &&
      getMetadata(aTheme, "DisableCombineConsecutive"))
  ) {
    return false;
  }

  if (!aPreviousMsg) {
    return false;
  }

  if (aMsg.system && aPreviousMsg.system) {
    return true;
  }

  if (
    aMsg.who != aPreviousMsg.who ||
    aMsg.outgoing != aPreviousMsg.outgoing ||
    aMsg.incoming != aPreviousMsg.incoming
  ) {
    return false;
  }

  let timeDifference = aMsg.time - aPreviousMsg.time;
  return (
    timeDifference >= 0 && timeDifference <= aTheme.combineConsecutiveInterval
  );
}

function getHTMLForMessage(aMsg, aTheme, aIsNext, aIsContext) {
  let html, replacements;
  if (aMsg.system) {
    html = aIsNext ? aTheme.html.statusNext : aTheme.html.status;
    replacements = statusReplacements;
  } else {
    html = aMsg.incoming ? "incoming" : "outgoing";
    if (aIsNext) {
      html += "Next";
    }
    html += aIsContext ? "Context" : "Content";
    html = aTheme.html[html];
    replacements = messageReplacements;
    let meRegExp = /^((<[^>]+>)*)\/me /;
    // We must test displayMessage here as aMsg.message loses its /me
    // in the following, so if getHTMLForMessage is called a second time for
    // the same aMsg (e.g. because it follows the unread ruler), the test
    // would fail otherwise.
    if (meRegExp.test(aMsg.displayMessage)) {
      aMsg.message = aMsg.message.replace(meRegExp, "$1");
      let actionMessageTemplate = "* %message% *";
      if (hasMetadataKey(aTheme, "ActionMessageTemplate")) {
        actionMessageTemplate = getMetadata(aTheme, "ActionMessageTemplate");
      }
      html = html.replace(/%message%/g, actionMessageTemplate);
    }
  }

  return replaceKeywordsInHTML(html, replacements, aMsg);
}

function insertHTMLForMessage(aMsg, aHTML, aDoc, aIsNext) {
  let insert = aDoc.getElementById("insert");
  if (insert && !aIsNext) {
    insert.remove();
    insert = null;
  }

  let range = aDoc.createRange();
  let parent = insert ? insert.parentNode : aDoc.getElementById("Chat");
  range.selectNode(parent);
  // eslint-disable-next-line no-unsanitized/method
  let documentFragment = range.createContextualFragment(aHTML);
  let result = documentFragment.firstElementChild;

  // store the prplIMessage object in each of the "root" node that
  // will be inserted into the document, so that selection code can
  // retrieve the message by just looking at the parent node until it
  // finds something.
  for (let root = result; root; root = root.nextElementSibling) {
    root._originalMsg = aMsg;
  }

  // make sure the result is an HTMLElement and not some text (whitespace)...
  while (
    result &&
    !(
      result.nodeType == result.ELEMENT_NODE &&
      result.namespaceURI == "http://www.w3.org/1999/xhtml"
    )
  ) {
    result = result.nextElementSibling;
  }
  if (insert) {
    parent.replaceChild(documentFragment, insert);
  } else {
    parent.appendChild(documentFragment);
  }
  return result;
}

function hasMetadataKey(aTheme, aKey) {
  return (
    aKey in aTheme.metadata ||
    (aTheme.variant != "default" &&
      aKey + ":" + aTheme.variant in aTheme.metadata) ||
    ("DefaultVariant" in aTheme.metadata &&
      aKey + ":" + aTheme.metadata.DefaultVariant in aTheme.metadata)
  );
}

function getMetadata(aTheme, aKey) {
  if (
    aTheme.variant != "default" &&
    aKey + ":" + aTheme.variant in aTheme.metadata
  ) {
    return aTheme.metadata[aKey + ":" + aTheme.variant];
  }

  if (
    "DefaultVariant" in aTheme.metadata &&
    aKey + ":" + aTheme.metadata.DefaultVariant in aTheme.metadata
  ) {
    return aTheme.metadata[aKey + ":" + aTheme.metadata.DefaultVariant];
  }

  return aTheme.metadata[aKey];
}

function initHTMLDocument(aConv, aTheme, aDoc) {
  let HTML = '<html><head><base href="' + aTheme.baseURI + '"/>';

  // Screen readers may read the title of the document, so provide one
  // to avoid an ugly fallback to the URL (see bug 1165).
  HTML += "<title>" + aConv.title + "</title>";

  function addCSS(aHref) {
    HTML += '<link rel="stylesheet" href="' + aHref + '" type="text/css"/>';
  }
  addCSS("chrome://chat/skin/conv.css");

  // add css to handle DefaultFontFamily and DefaultFontSize
  let cssText = "";
  if (hasMetadataKey(aTheme, "DefaultFontFamily")) {
    cssText += "font-family: " + getMetadata(aTheme, "DefaultFontFamily") + ";";
  }
  if (hasMetadataKey(aTheme, "DefaultFontSize")) {
    cssText += "font-size: " + getMetadata(aTheme, "DefaultFontSize") + ";";
  }
  if (cssText) {
    addCSS("data:text/css,*{ " + cssText + " }");
  }

  // add the main CSS file of the theme
  if (aTheme.metadata.MessageViewVersion >= 3 || aTheme.variant == "default") {
    addCSS("main.css");
  }

  // add the CSS file of the variant
  if (aTheme.variant != "default") {
    addCSS("Variants/" + aTheme.variant + ".css");
  } else if ("DefaultVariant" in aTheme.metadata) {
    addCSS("Variants/" + aTheme.metadata.DefaultVariant + ".css");
  }

  HTML += '</head><body id="ibcontent">';

  // We insert the whole content of body: header, chat div, footer
  if (aTheme.showHeader) {
    HTML += replaceKeywordsInHTML(
      aTheme.html.header,
      headerFooterReplacements,
      aConv
    );
  }
  HTML += '<div id="Chat" aria-live="polite"></div>';
  HTML += replaceKeywordsInHTML(
    aTheme.html.footer,
    headerFooterReplacements,
    aConv
  );
  aDoc.open();
  aDoc.write(HTML + "</body></html>");
  aDoc.close();
  aDoc.defaultView.convertTimeUnits = DownloadUtils.convertTimeUnits;
}

/* Selection stuff */
function getEllipsis() {
  let ellipsis = "[\u2026]";

  try {
    ellipsis = Services.prefs.getComplexValue(
      "messenger.conversations.selections.ellipsis",
      Ci.nsIPrefLocalizedString
    ).data;
  } catch (e) {}
  return ellipsis;
}

function _serializeDOMObject(aDocument, aInitFunction) {
  // This shouldn't really be a constant, as we want to support
  // text/html too in the future.
  const type = "text/plain";

  let encoder = Cu.createDocumentEncoder(type);
  encoder.init(aDocument, type, Ci.nsIDocumentEncoder.OutputPreformatted);
  aInitFunction(encoder);
  let result = encoder.encodeToString();
  return result;
}

function serializeRange(aRange) {
  return _serializeDOMObject(aRange.startContainer.ownerDocument, function(
    aEncoder
  ) {
    aEncoder.setRange(aRange);
  });
}

function serializeNode(aNode) {
  return _serializeDOMObject(aNode.ownerDocument, function(aEncoder) {
    aEncoder.setNode(aNode);
  });
}

/* This function is used to pretty print a selection inside a conversation area */
function serializeSelection(aSelection) {
  // We have two kinds of selection serialization:
  //  - The short version, used when only a part of message is
  //    selected, or if nothing interesting is selected
  let shortSelection = "";

  //  - The long version, which is used:
  //      * when both some of the message text and some of the context
  //        (sender, time, ...) is selected;
  //      * when several messages are selected at once
  //    This version uses an array, with each message formatted
  //    through the theme system.
  let longSelection = [];

  // We first assume that we are going to use the short version, but
  // while working on creating the short version, we prepare
  // everything to be able to switch to the long version if we later
  // discover that it is in fact needed.
  let shortVersionPossible = true;

  // Sometimes we need to know if a selection range is inside the same
  // message as the previous selection range, so we keep track of the
  // last message we have processed.
  let lastMessage = null;

  for (let i = 0; i < aSelection.rangeCount; ++i) {
    let range = aSelection.getRangeAt(i);
    let messages = getMessagesForRange(range);

    // If at least one selected message has some of its text selected,
    // remove from the selection all the messages that have no text
    // selected
    let testFunction = msg => msg.isTextSelected();
    if (messages.some(testFunction)) {
      messages = messages.filter(testFunction);
    }

    if (!messages.length) {
      // Do it only if it wouldn't override a better already found selection
      if (!shortSelection) {
        shortSelection = serializeRange(range);
      }
      continue;
    }

    if (
      shortVersionPossible &&
      messages.length == 1 &&
      (!messages[0].isTextSelected() || messages[0].onlyTextSelected()) &&
      (!lastMessage ||
        lastMessage.msg == messages[0].msg ||
        lastMessage.msg.who == messages[0].msg.who)
    ) {
      if (shortSelection) {
        if (lastMessage.msg != messages[0].msg) {
          // Add the ellipsis only if the previous message was cut
          if (lastMessage.cutEnd) {
            shortSelection += " " + getEllipsis();
          }
          shortSelection += kLineBreak;
        } else {
          shortSelection += " " + getEllipsis() + " ";
        }
      }
      shortSelection += serializeRange(range);
      longSelection.push(messages[0].getFormattedMessage());
    } else {
      shortVersionPossible = false;
      for (let m = 0; m < messages.length; ++m) {
        let message = messages[m];
        if (m == 0 && lastMessage && lastMessage.msg == message.msg) {
          let text = message.getSelectedText();
          if (message.cutEnd) {
            text += " " + getEllipsis();
          }
          longSelection[longSelection.length - 1] += " " + text;
        } else {
          longSelection.push(message.getFormattedMessage());
        }
      }
    }
    lastMessage = messages[messages.length - 1];
  }

  if (shortVersionPossible) {
    return shortSelection || aSelection.toString();
  }
  return longSelection.join(kLineBreak);
}

function SelectedMessage(aRootNode, aRange) {
  this._rootNodes = [aRootNode];
  this._range = aRange;
}

SelectedMessage.prototype = {
  get msg() {
    return this._rootNodes[0]._originalMsg;
  },
  addRoot(aRootNode) {
    this._rootNodes.push(aRootNode);
  },

  // Helper function that returns the first span node of class
  // ib-msg-text under the rootNodes of the selected message.
  _getSpanNode() {
    // first use the cached value if any
    if (this._spanNode) {
      return this._spanNode;
    }

    let spanNode = null;
    // If we could use NodeFilter.webidl, we wouldn't have to make up our own
    // object. FILTER_REJECT is not used here, but included for completeness.
    const NodeFilter = {
      SHOW_ELEMENT: 0x1,
      FILTER_ACCEPT: 1,
      FILTER_REJECT: 2,
      FILTER_SKIP: 3,
    };
    // helper filter function for the tree walker
    let filter = function(node) {
      return node.className == "ib-msg-txt"
        ? NodeFilter.FILTER_ACCEPT
        : NodeFilter.FILTER_SKIP;
    };
    // walk the DOM subtrees of each root, keep the first correct span node
    for (let i = 0; !spanNode && i < this._rootNodes.length; ++i) {
      let rootNode = this._rootNodes[i];
      // the TreeWalker doesn't test the root node, special case it first
      if (filter(rootNode) == NodeFilter.FILTER_ACCEPT) {
        spanNode = rootNode;
        break;
      }
      let treeWalker = rootNode.ownerDocument.createTreeWalker(
        rootNode,
        NodeFilter.SHOW_ELEMENT,
        { acceptNode: filter },
        false
      );
      spanNode = treeWalker.nextNode();
    }

    return (this._spanNode = spanNode);
  },

  // Initialize _textSelected and _otherSelected; if _textSelected is true,
  // also initialize _selectedText and _cutBegin/End.
  _initSelectedText() {
    if ("_textSelected" in this) {
      // Already initialized.
      return;
    }

    let spanNode = this._getSpanNode();
    if (!spanNode) {
      // can happen if the message text is under a separate root node
      // that isn't selected at all
      this._textSelected = false;
      this._otherSelected = true;
      return;
    }
    let startPoint = this._range.comparePoint(spanNode, 0);
    let endPoint = this._range.comparePoint(spanNode, spanNode.children.length);
    if (startPoint <= 0 && endPoint >= 0) {
      let range = this._range.cloneRange();
      if (startPoint >= 0) {
        range.setStart(spanNode, 0);
      }
      if (endPoint <= 0) {
        range.setEnd(spanNode, spanNode.children.length);
      }
      this._selectedText = serializeRange(range);

      // if the selected text is empty, set _selectedText to false
      // this happens if the carret is at the offset 0 in the span node
      this._textSelected = this._selectedText != "";
    } else {
      this._textSelected = false;
    }
    if (this._textSelected) {
      // to check if the start or end is cut, the result of
      // comparePoint is not enough because the selection range may
      // start or end in a text node instead of the span node

      if (startPoint == -1) {
        let range = spanNode.ownerDocument.createRange();
        range.setStart(spanNode, 0);
        range.setEnd(this._range.startContainer, this._range.startOffset);
        this._cutBegin = serializeRange(range) != "";
      } else {
        this._cutBegin = false;
      }

      if (endPoint == 1) {
        let range = spanNode.ownerDocument.createRange();
        range.setStart(this._range.endContainer, this._range.endOffset);
        range.setEnd(spanNode, spanNode.children.length);
        this._cutEnd = !/^(\r?\n)?$/.test(serializeRange(range));
      } else {
        this._cutEnd = false;
      }
    }
    this._otherSelected =
      (startPoint >= 0 || endPoint <= 0) && // eliminate most negative cases
      (!this._textSelected ||
        serializeRange(this._range).length > this._selectedText.length);
  },
  get cutBegin() {
    this._initSelectedText();
    return this._textSelected && this._cutBegin;
  },
  get cutEnd() {
    this._initSelectedText();
    return this._textSelected && this._cutEnd;
  },
  isTextSelected() {
    this._initSelectedText();
    return this._textSelected;
  },
  onlyTextSelected() {
    this._initSelectedText();
    return !this._otherSelected;
  },
  getSelectedText() {
    this._initSelectedText();
    return this._textSelected ? this._selectedText : "";
  },
  getFormattedMessage() {
    // First, get the selected text
    this._initSelectedText();
    let msg = this.msg;
    let text;
    if (this._textSelected) {
      // Add ellipsis is needed
      text =
        (this._cutBegin ? getEllipsis() + " " : "") +
        this._selectedText +
        (this._cutEnd ? " " + getEllipsis() : "");
    } else {
      let div = this._rootNodes[0].ownerDocument.createElement("div");
      // eslint-disable-next-line no-unsanitized/property
      div.innerHTML = msg.autoResponse
        ? formatAutoResponce(msg.message)
        : msg.message;
      text = serializeNode(div);
    }

    // then get the suitable replacements and templates for this message
    let getLocalizedPrefWithDefault = function(aName, aDefault) {
      try {
        let prefBranch = Services.prefs.getBranch(
          "messenger.conversations.selections."
        );
        return prefBranch.getComplexValue(aName, Ci.nsIPrefLocalizedString)
          .data;
      } catch (e) {
        return aDefault;
      }
    };
    let html, replacements;
    if (msg.system) {
      replacements = statusReplacements;
      html = getLocalizedPrefWithDefault(
        "systemMessagesTemplate",
        "%time% - %message%"
      );
    } else {
      replacements = messageReplacements;
      if (/^(<[^>]+>)*\/me /.test(msg.displayMessage)) {
        html = getLocalizedPrefWithDefault(
          "actionMessagesTemplate",
          "%time% * %sender% %message%"
        );
      } else {
        html = getLocalizedPrefWithDefault(
          "contentMessagesTemplate",
          "%time% - %sender%: %message%"
        );
      }
    }

    // Overrides default replacements so that they don't add a span node.
    // Also, this uses directly the text variable so that we don't
    // have to change the content of msg.message and revert it
    // afterwards.
    replacements = {
      message: aMsg => text,
      sender: aMsg => aMsg.alias || aMsg.who,
      __proto__: replacements,
    };

    // Finally, let the theme system do the magic!
    return replaceKeywordsInHTML(html, replacements, msg);
  },
};

function getMessagesForRange(aRange) {
  let result = []; // will hold the final result
  let messages = {}; // used to prevent duplicate messages in the result array

  // cache the range boundaries, they will be used a lot
  let endNode = aRange.endContainer;
  let startNode = aRange.startContainer;

  // Helper function to recursively look for _originalMsg JS
  // properties on DOM nodes, and stop when endNode is reached.
  // Found nodes are pushed into the rootNodes array.
  let processSubtree = function(aNode) {
    if (aNode._originalMsg) {
      // store the result
      if (!(aNode._originalMsg.id in messages)) {
        // we've found a new message!
        let newMessage = new SelectedMessage(aNode, aRange);
        messages[aNode._originalMsg.id] = newMessage;
        result.push(newMessage);
      } else {
        // we've found another root of an already known message
        messages[aNode._originalMsg.id].addRoot(aNode);
      }
    }

    // check if we have reached the end node
    if (aNode == endNode) {
      return true;
    }

    // recurse through children
    if (
      aNode.nodeType == aNode.ELEMENT_NODE &&
      aNode.namespaceURI == "http://www.w3.org/1999/xhtml"
    ) {
      for (let i = 0; i < aNode.children.length; ++i) {
        if (processSubtree(aNode.children[i])) {
          return true;
        }
      }
    }

    return false;
  };

  let currentNode = aRange.commonAncestorContainer;
  if (
    currentNode.nodeType == currentNode.ELEMENT_NODE &&
    currentNode.namespaceURI == "http://www.w3.org/1999/xhtml"
  ) {
    // Determine the index of the first and last children of currentNode
    // that we should process.
    let found = false;
    let start = 0;
    if (currentNode == startNode) {
      // we want to process all children
      found = true;
      start = aRange.startOffset;
    } else {
      // startNode needs to be a direct child of currentNode
      while (startNode.parentNode != currentNode) {
        startNode = startNode.parentNode;
      }
    }
    let end;
    if (currentNode == endNode) {
      end = aRange.endOffset;
    } else {
      end = currentNode.children.length;
    }

    for (let i = start; i < end; ++i) {
      let node = currentNode.children[i];

      // don't do anything until we find the startNode
      found = found || node == startNode;
      if (!found) {
        continue;
      }

      if (processSubtree(node)) {
        break;
      }
    }
  }

  // The selection may not include any root node of the first touched
  // message, in this case, the DOM traversal of the DOM range
  // couldn't give us the first message. Make sure we actually have
  // the message in which the range starts.
  let firstRoot = aRange.startContainer;
  while (firstRoot && !firstRoot._originalMsg) {
    firstRoot = firstRoot.parentNode;
  }
  if (firstRoot && !(firstRoot._originalMsg.id in messages)) {
    result.unshift(new SelectedMessage(firstRoot, aRange));
  }

  return result;
}
