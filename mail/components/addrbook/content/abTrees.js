/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

/* import-globals-from ../../../../mailnews/addrbook/content/abDragDrop.js */
/* import-globals-from abCommon.js */

/**
 * This file contains our implementation for various addressbook trees.  It
 * depends on jsTreeView.js being loaded before this script is loaded.
 */

var { Services } = ChromeUtils.import("resource://gre/modules/Services.jsm");
var { MailServices } = ChromeUtils.import(
  "resource:///modules/MailServices.jsm"
);
var { IOUtils } = ChromeUtils.import("resource:///modules/IOUtils.jsm");

// Tree Sort helper methods.
var AB_ORDER = [
  "aab",
  "pab",
  "js",
  "carddav",
  "ldap",
  "mapi+other",
  "anyab",
  "cab",
];

function getDirectoryValue(aDir, aKey) {
  if (aKey == "ab_type") {
    if (aDir._directory.URI == kAllDirectoryRoot + "?") {
      return "aab";
    }
    if (aDir._directory.URI == kPersonalAddressbookURI) {
      return "pab";
    }
    if (aDir._directory.URI == kCollectedAddressbookURI) {
      return "cab";
    }
    if (aDir._directory.URI.startsWith("jsaddrbook://")) {
      return "js";
    }
    if (aDir._directory.URI.startsWith("jscarddav://")) {
      return "carddav";
    }
    if (aDir._directory instanceof Ci.nsIAbLDAPDirectory) {
      return "ldap";
    }

    // If there is any other AB type.
    return "mapi+other";
  } else if (aKey == "ab_name") {
    return aDir._directory.dirName;
  }

  // This should never happen.
  return null;
}

function abNameCompare(a, b) {
  return a.localeCompare(b);
}

function abTypeCompare(a, b) {
  return AB_ORDER.indexOf(a) - AB_ORDER.indexOf(b);
}

var SORT_PRIORITY = ["ab_type", "ab_name"];
var SORT_FUNCS = [abTypeCompare, abNameCompare];

function abSort(a, b) {
  for (let i = 0; i < SORT_FUNCS.length; i++) {
    let sortBy = SORT_PRIORITY[i];
    let aValue = getDirectoryValue(a, sortBy);
    let bValue = getDirectoryValue(b, sortBy);

    if (!aValue && !bValue) {
      return 0;
    }
    if (!aValue) {
      return -1;
    }
    if (!bValue) {
      return 1;
    }
    if (aValue != bValue) {
      let result = SORT_FUNCS[i](aValue, bValue);

      if (result != 0) {
        return result;
      }
    }
  }
  return 0;
}

/**
 * Each abDirTreeItem corresponds to one row in the tree view.
 */
function abDirTreeItem(aDirectory) {
  this._directory = aDirectory;
}

abDirTreeItem.prototype = {
  getText() {
    return this._directory.dirName;
  },

  get id() {
    return this._directory.URI;
  },

  _open: false,
  get open() {
    return this._open;
  },

  _level: 0,
  get level() {
    return this._level;
  },

  _children: null,
  get children() {
    if (!this._children) {
      this._children = [];
      let myEnum;
      if (this._directory.URI == kAllDirectoryRoot + "?") {
        myEnum = MailServices.ab.directories;
      } else {
        myEnum = this._directory.childNodes;
      }

      for (let dir of myEnum) {
        var abItem = new abDirTreeItem(dir);
        if (
          gDirectoryTreeView &&
          this.id == kAllDirectoryRoot + "?" &&
          getDirectoryValue(abItem, "ab_type") == "ldap"
        ) {
          gDirectoryTreeView.hasRemoteAB = true;
        }

        this._children.push(abItem);
        this._children[this._children.length - 1]._level = this._level + 1;
        this._children[this._children.length - 1]._parent = this;
      }

      this._children.sort(abSort);
    }
    return this._children;
  },

  getProperties() {
    let properties = [];
    if (this._directory.isMailList) {
      properties.push("IsMailList-true");
    }
    if (this._directory.isRemote) {
      properties.push("IsRemote-true");
    }
    if (this._directory.isSecure) {
      properties.push("IsSecure-true");
    }
    return properties.join(" ");
  },
};

/**
 * Our actual implementation of nsITreeView.
 */
function directoryTreeView() {}
directoryTreeView.prototype = {
  __proto__: new PROTO_TREE_VIEW(),
  QueryInterface: ChromeUtils.generateQI([Ci.nsITreeView, Ci.nsIAbListener]),

  hasRemoteAB: false,

  init(aTree, aJSONFile) {
    if (aJSONFile) {
      // Parse our persistent-open-state json file
      let data = IOUtils.loadFileToString(aJSONFile);
      if (data) {
        this._persistOpenMap = JSON.parse(data);
      }
    }

    this._rebuild();
    aTree.view = this;
  },

  shutdown(aJSONFile) {
    // Write out the persistOpenMap to our JSON file
    if (aJSONFile) {
      // Write out our json file...
      let data = JSON.stringify(this._persistOpenMap);
      IOUtils.saveStringToFile(aJSONFile, data);
    }
  },

  // Override the dnd methods for those functions in abDragDrop.js
  canDrop(aIndex, aOrientation, dataTransfer) {
    return abDirTreeObserver.canDrop(aIndex, aOrientation, dataTransfer);
  },

  drop(aRow, aOrientation, dataTransfer) {
    abDirTreeObserver.onDrop(aRow, aOrientation, dataTransfer);
  },

  getDirectoryAtIndex(aIndex) {
    return this._rowMap[aIndex]._directory;
  },

  // Override jsTreeView's isContainer, since we want to be able
  // to react to drag-drop events for all items in the directory
  // tree.
  isContainer(aIndex) {
    return true;
  },

  /**
   * Returns true if the selected directory is inline-editable.
   * @param {int} aRow - the row index of the selected dir/mail list.
   * @param {int} aCol - not used a directories are represented as rows only.
   */
  isEditable(aRow, aCol) {
    let selectedDirectory = this.getDirectoryAtIndex(aRow);

    // Prevent the renaming of Personal Address Book, Collected Addressses
    // and All Address Books directories.
    if (
      !selectedDirectory ||
      selectedDirectory.URI == kAllDirectoryRoot + "?" ||
      selectedDirectory.URI == kPersonalAddressbookURI ||
      selectedDirectory.URI == kCollectedAddressbookURI
    ) {
      return false;
    }
    return true;
  },

  /**
   * Saves the new name  dir/mail list.
   * @param {int} aRow - the row index of the selected  dir/mail list.
   * @param {int} aCol - not used a directories are represented as rows only.
   * @param {string} aValue - the new name of dir/mail list to be saved.
   */
  setCellText(aRow, aCol, aValue) {
    let selectedDirectory = this.getDirectoryAtIndex(aRow);
    let newName = aValue.trim();

    // Check if the new name is empty.
    if (newName.length == 0) {
      return;
    }

    // Mailists requires to call the backend to update its name.
    if (selectedDirectory.isMailList) {
      // Check if the new name contains 2 spaces.
      if (newName.match("  ")) {
        return;
      }

      // Check if the new name contains the following special characters.
      for (let char of ',;"<>') {
        if (newName.includes(char)) {
          return;
        }
      }

      // Prevent duplicate names but allow case change in case canonical name is the same.
      let canonicalNewListName = newName.toLowerCase();
      let canonicalOldListName = selectedDirectory.dirName.toLowerCase();
      if (
        canonicalNewListName != canonicalOldListName &&
        MailServices.ab.mailListNameExists(newName)
      ) {
        return;
      }

      selectedDirectory.dirName = newName;
      selectedDirectory.editMailListToDatabase(null);
    } else {
      // Do not allow an already existing name.
      if (MailServices.ab.directoryNameExists(newName)) {
        return;
      }

      /* Name is unique, set value */
      selectedDirectory.dirName = newName;
    }
  },

  /**
   * NOTE: This function will result in indeterminate rows being selected.
   *       Callers should take care to re-select a desired row after calling
   *       this function.
   */
  _rebuild() {
    var oldCount = this._rowMap.length;
    this._rowMap = [];

    // Make an entry for All Address Books.
    let rootAB = {
      QueryInterface: ChromeUtils.generateQI([Ci.nsIAbDirectory]),

      dirName: gAddressBookBundle.getString("allAddressBooks"),
      isMailList: false,
      isRemote: false,
      isSecure: false,
      URI: kAllDirectoryRoot + "?",

      get childNodes() {
        return MailServices.ab.directories;
      },

      get propertiesChromeURI() {
        return "chrome://messenger/content/addressbook/abAddressBookNameDialog.xhtml";
      },
    };
    this._rowMap.push(new abDirTreeItem(rootAB));

    // Sort our addressbooks now
    this._rowMap.sort(abSort);

    if (this._tree) {
      this._tree.rowCountChanged(0, this._rowMap.length - oldCount);
    }

    this._restoreOpenStates();
  },

  getIndexForId(aId) {
    for (let i = 0; i < this._rowMap.length; i++) {
      if (this._rowMap[i].id == aId) {
        return i;
      }
    }

    return -1;
  },

  // nsIAbListener interfaces
  onItemAdded(aParent, aItem) {
    try {
      aItem.QueryInterface(Ci.nsIAbDirectory);
    } catch (ex) {
      return;
    }
    // XXX we can optimize this later
    this._rebuild();

    if (!this._tree) {
      return;
    }

    // Now select this new item
    for (var [i, row] of this._rowMap.entries()) {
      if (row.id == aItem.URI) {
        this.selection.select(i);
        break;
      }
    }
  },

  onItemRemoved(aParent, aItem) {
    try {
      aItem.QueryInterface(Ci.nsIAbDirectory);
    } catch (ex) {
      return;
    }
    // XXX we can optimize this later
    this._rebuild();

    if (!this._tree) {
      return;
    }

    // If we're deleting a top-level address-book, just select the first book
    if (
      !aParent ||
      aParent.URI == kAllDirectoryRoot ||
      aParent.URI == kAllDirectoryRoot + "?"
    ) {
      this.selection.select(0);
      return;
    }

    // Now select this parent item
    for (var [i, row] of this._rowMap.entries()) {
      if (row.id == aParent.URI) {
        this.selection.select(i);
        break;
      }
    }
  },

  onItemPropertyChanged(aItem, aProp, aOld, aNew) {
    try {
      aItem.QueryInterface(Ci.nsIAbDirectory);
    } catch (ex) {
      return;
    }

    for (let i in this._rowMap) {
      if (this._rowMap[i]._directory == aItem) {
        this._tree.invalidateRow(i);
        break;
      }
    }
  },
};

var gDirectoryTreeView = new directoryTreeView();
