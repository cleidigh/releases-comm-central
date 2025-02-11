/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, you can obtain one at http://mozilla.org/MPL/2.0/. */

const EXPORTED_SYMBOLS = ["CardDAVServer"];

const CARD_NS = "urn:ietf:params:xml:ns:carddav";
const DAV_NS = "DAV:";

const { Assert } = ChromeUtils.import("resource://testing-common/Assert.jsm");
const { CommonUtils } = ChromeUtils.import(
  "resource://services-common/utils.js"
);
const { HttpServer } = ChromeUtils.import("resource://testing-common/httpd.js");

var CardDAVServer = {
  cards: new Map(),
  deletedCards: new Map(),
  changeCount: 0,
  server: null,

  open() {
    this.server = new HttpServer();
    this.server.start(-1);
    this.server.registerPathHandler("/ping", this.ping);
    this.server.registerPathHandler(this.path, this.pathHandler.bind(this));
    this.server.registerPrefixHandler(this.path, this.prefixHandler.bind(this));
  },

  close() {
    return new Promise(resolve => this.server.stop({ onStopped: resolve }));
  },

  get path() {
    return "/addressbooks/me/test/";
  },

  get url() {
    return `http://localhost:${this.server.identity.primaryPort}${this.path}`;
  },

  setUsernameAndPassword(username, password) {
    this.username = username;
    this.password = password;
  },

  ping(request, response) {
    response.setStatusLine("1.1", 200, "OK");
    response.write("pong");
  },

  checkAuth(request, response) {
    if (!this.username || !this.password) {
      return true;
    }

    if (!request.hasHeader("Authorization")) {
      response.setStatusLine("1.1", 401, "Unauthorized");
      response.setHeader("WWW-Authenticate", `Basic realm="test"`);
      return false;
    }

    let value = request.getHeader("Authorization");
    if (!value.startsWith("Basic ")) {
      response.setStatusLine("1.1", 401, "Unauthorized");
      response.setHeader("WWW-Authenticate", `Basic realm="test"`);
      return false;
    }

    let [username, password] = atob(value.substring(6)).split(":");
    if (username != this.username || password != this.password) {
      response.setStatusLine("1.1", 401, "Unauthorized");
      return false;
    }

    return true;
  },

  pathHandler(request, response) {
    if (!this.checkAuth(request, response)) {
      return;
    }

    let input = new DOMParser().parseFromString(
      CommonUtils.readBytesFromInputStream(request.bodyInputStream),
      "text/xml"
    );

    switch (input.documentElement.localName) {
      case "addressbook-query":
        Assert.equal(request.method, "REPORT");
        this.addressBookQuery(input, response);
        return;
      case "addressbook-multiget":
        Assert.equal(request.method, "REPORT");
        this.addressBookMultiGet(input, response);
        return;
      case "propfind":
        Assert.equal(request.method, "PROPFIND");
        this.propFind(
          input,
          request.hasHeader("Depth") ? request.getHeader("Depth") : 0,
          response
        );
        return;
      case "sync-collection":
        Assert.equal(request.method, "REPORT");
        this.syncCollection(input, response);
        return;
    }

    Assert.report(true, undefined, undefined, "Should not have reached here");
    response.setStatusLine("1.1", 404, "Not Found");
    response.write(`No handler found for <${input.documentElement.localName}>`);
  },

  addressBookQuery(input, response) {
    let includeVCard = input.querySelector("address-data");
    let output = `<multistatus xmlns:card="${CARD_NS}" xmlns="${DAV_NS}">`;
    for (let [href, card] of this.cards) {
      output += this._cardResponse(href, card, includeVCard);
    }
    output += `</multistatus>`;

    response.setStatusLine("1.1", 200, "OK");
    response.write(output.replace(/>\s+</g, "><"));
  },

  addressBookMultiGet(input, response) {
    let output = `<multistatus xmlns:card="${CARD_NS}" xmlns="${DAV_NS}">`;
    for (let href of input.querySelectorAll("href")) {
      href = href.textContent;
      let card = this.cards.get(href);
      if (card) {
        output += this._cardResponse(href, card, true);
      }
    }
    output += `</multistatus>`;

    response.setStatusLine("1.1", 200, "OK");
    response.write(output.replace(/>\s+</g, "><"));
  },

  propFind(input, depth, response) {
    let output = `<multistatus xmlns="${DAV_NS}">
      <response>
        <href>${this.path}</href>
        <propstat>
          <prop>
            <displayname>test</displayname>
            <sync-token>http://mochi.test/sync/${this.changeCount}</sync-token>
          </prop>
          <status>HTTP/1.1 200 OK</status>
        </propstat>
      </response>`;
    if (depth == 1) {
      for (let [href, card] of this.cards) {
        output += this._cardResponse(href, card, false);
      }
    }
    output += `</multistatus>`;

    response.setStatusLine("1.1", 200, "OK");
    response.write(output.replace(/>\s+</g, "><"));
  },

  syncCollection(input, response) {
    let token = input
      .querySelector("sync-token")
      .textContent.replace(/\D/g, "");
    let includeVCard = input.querySelector("address-data");

    let output = `<multistatus xmlns:card="${CARD_NS}" xmlns="${DAV_NS}">`;
    for (let [href, card] of this.cards) {
      if (card.changed > token) {
        output += this._cardResponse(href, card, includeVCard);
      }
    }
    for (let [href, deleted] of this.deletedCards) {
      if (deleted > token) {
        output += `<response>
          <status>HTTP/1.1 404 Not Found</status>
          <href>${href}</href>
          <propstat>
            <prop/>
            <status>HTTP/1.1 418 I'm a teapot</status>
          </propstat>
        </response>`;
      }
    }
    output += `<sync-token>http://mochi.test/sync/${this.changeCount}</sync-token>
    </multistatus>`;

    response.setStatusLine("1.1", 200, "OK");
    response.write(output.replace(/>\s+</g, "><"));
  },

  _cardResponse(href, card, includeVCard) {
    let outString = `<response>
      <href>${href}</href>
      <propstat>
        <prop>
          <getetag>${card.etag}</getetag>`;
    if (includeVCard) {
      outString += `<card:address-data>${card.vCard}</card:address-data>`;
    }
    outString += `<status>HTTP/1.1 200 OK</status>
        </prop>
      </propstat>
    </response>`;
    return outString;
  },

  prefixHandler(request, response) {
    if (!this.checkAuth(request, response)) {
      return;
    }

    if (!/\/[\w-]+\.vcf$/.test(request.path)) {
      response.setStatusLine("1.1", 404, "Not Found");
      response.write(`Card not found at ${request.path}`);
      return;
    }

    switch (request.method) {
      case "GET":
        this.getCard(request, response);
        return;
      case "PUT":
        this.putCard(request, response);
        return;
      case "DELETE":
        this.deleteCard(request, response);
        return;
    }

    Assert.report(true, undefined, undefined, "Should not have reached here");
    response.setStatusLine("1.1", 405, "Method Not Allowed");
    response.write(`Method not allowed: ${request.method}`);
  },

  getCard(request, response) {
    let card = this.cards.get(request.path);
    if (!card) {
      response.setStatusLine("1.1", 404, "Not Found");
      response.write(`Card not found at ${request.path}`);
      return;
    }

    response.setStatusLine("1.1", 200, "OK");
    response.setHeader("ETag", card.etag);
    response.write(card.vCard);
  },

  putCard(request, response) {
    let vCard = CommonUtils.readBytesFromInputStream(request.bodyInputStream);
    this.putCardInternal(request.path, vCard);
    response.setStatusLine("1.1", 200, "OK");
  },

  putCardInternal(name, vCard) {
    if (!name.startsWith("/")) {
      name = this.path + name;
    }
    let etag = "" + vCard.length;
    this.cards.set(name, { etag, vCard, changed: ++this.changeCount });
    this.deletedCards.delete(name);
  },

  deleteCard(request, response) {
    this.deleteCardInternal(request.path);
    response.setStatusLine("1.1", 200, "OK");
  },

  deleteCardInternal(name) {
    if (!name.startsWith("/")) {
      name = this.path + name;
    }
    this.cards.delete(name);
    this.deletedCards.set(name, ++this.changeCount);
  },
};
