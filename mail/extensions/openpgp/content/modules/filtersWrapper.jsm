/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 */

"use strict";

var EXPORTED_SYMBOLS = ["EnigmailFiltersWrapper"];

var gEnigmailFilters = null;

let { EnigmailConstants } = ChromeUtils.import(
  "chrome://openpgp/content/modules/constants.jsm"
);
let { EnigmailLocale } = ChromeUtils.import(
  "chrome://openpgp/content/modules/locale.jsm"
);

/**
 * filter action for creating a decrypted version of the mail and
 * deleting the original mail at the same time
 */

const filterActionMoveDecrypt = {
  id: EnigmailConstants.FILTER_MOVE_DECRYPT,
  name: EnigmailLocale.getString("filter.decryptMove2.label"),
  value: "movemessage",
  apply(aMsgHdrs, aActionValue, aListener, aType, aMsgWindow) {
    if (gEnigmailFilters) {
      gEnigmailFilters.moveDecrypt.apply(
        aMsgHdrs,
        aActionValue,
        aListener,
        aType,
        aMsgWindow
      );
    } else {
      aListener.OnStartCopy();
      aListener.OnStopCopy(0);
    }
  },

  isValidForType(type, scope) {
    return gEnigmailFilters
      ? gEnigmailFilters.moveDecrypt.isValidForType(type, scope)
      : false;
  },

  validateActionValue(value, folder, type) {
    if (gEnigmailFilters) {
      return gEnigmailFilters.moveDecrypt.validateActionValue(
        value,
        folder,
        type
      );
    }
    return null;
  },

  allowDuplicates: false,
  isAsync: true,
  needsBody: true,
};

/**
 * filter action for creating a decrypted copy of the mail, leaving the original
 * message untouched
 */
const filterActionCopyDecrypt = {
  id: EnigmailConstants.FILTER_COPY_DECRYPT,
  name: EnigmailLocale.getString("filter.decryptCopy2.label"),
  value: "copymessage",
  apply(aMsgHdrs, aActionValue, aListener, aType, aMsgWindow) {
    if (gEnigmailFilters) {
      gEnigmailFilters.copyDecrypt.apply(
        aMsgHdrs,
        aActionValue,
        aListener,
        aType,
        aMsgWindow
      );
    } else {
      aListener.OnStartCopy();
      aListener.OnStopCopy(0);
    }
  },

  isValidForType(type, scope) {
    return gEnigmailFilters
      ? gEnigmailFilters.copyDecrypt.isValidForType(type, scope)
      : false;
  },

  validateActionValue(value, folder, type) {
    if (gEnigmailFilters) {
      return gEnigmailFilters.copyDecrypt.validateActionValue(
        value,
        folder,
        type
      );
    }
    return null;
  },

  allowDuplicates: false,
  isAsync: true,
  needsBody: true,
};

/**
 * filter action for to encrypt a mail to a specific key
 */
const filterActionEncrypt = {
  id: EnigmailConstants.FILTER_ENCRYPT,
  name: EnigmailLocale.getString("filter.encrypt2.label"),
  value: "encryptto",
  apply(aMsgHdrs, aActionValue, aListener, aType, aMsgWindow) {
    if (gEnigmailFilters) {
      gEnigmailFilters.encrypt.apply(
        aMsgHdrs,
        aActionValue,
        aListener,
        aType,
        aMsgWindow
      );
    } else {
      aListener.OnStartCopy();
      aListener.OnStopCopy(0);
    }
  },

  isValidForType(type, scope) {
    return gEnigmailFilters ? gEnigmailFilters.encrypt.isValidForType() : false;
  },

  validateActionValue(value, folder, type) {
    if (gEnigmailFilters) {
      return gEnigmailFilters.encrypt.validateActionValue(value, folder, type);
    }
    return null;
  },

  allowDuplicates: false,
  isAsync: true,
  needsBody: true,
};

/**
 * Add a custom filter action. If the filter already exists, do nothing
 * (for example, if addon is disabled and re-enabled)
 *
 * @param filterObj - nsIMsgFilterCustomAction
 */
function addFilterIfNotExists(filterObj) {
  let filterService = Cc[
    "@mozilla.org/messenger/services/filters;1"
  ].getService(Ci.nsIMsgFilterService);

  let foundFilter = null;
  try {
    foundFilter = filterService.getCustomAction(filterObj.id);
  } catch (ex) {}

  if (!foundFilter) {
    filterService.addCustomAction(filterObj);
  }
}

var EnigmailFiltersWrapper = {
  onStartup() {
    let { EnigmailFilters } = ChromeUtils.import(
      "chrome://openpgp/content/modules/filters.jsm"
    );
    gEnigmailFilters = EnigmailFilters;

    addFilterIfNotExists(filterActionMoveDecrypt);
    addFilterIfNotExists(filterActionCopyDecrypt);
    addFilterIfNotExists(filterActionEncrypt);

    gEnigmailFilters.onStartup();
  },

  onShutdown() {
    gEnigmailFilters.onShutdown();
    gEnigmailFilters = null;
  },
};
