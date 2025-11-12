# Qubic Network REST API

This document provides an overview of the REST API endpoints for querying bob (`default: PORT 40420`)

---

### `GET /balance/{identity}`

* **Description:** Fetches the balance for a given identity.
* **Path Parameters:**
    * `{identity}`: The public identity (address) to query.

---

### `GET /tx/{tx_hash}`

* **Description:** Retrieves a specific transaction by its hash. To know more about how to deal with transactions and logging, [check here](DEAL_WITH_TX.MD) 
* **Path Parameters:**
    * `{tx_hash}`: The hash of the transaction to retrieve.

---

### `GET /log/{epoch}/{from_id}/{to_id}`

* **Description:** Gets logs for a specific epoch and within a given ID range.
* **Path Parameters:**
    * `{epoch}`: The epoch number.
    * `{from_id}`: The starting ID for the log range.
    * `{to_id}`: The ending ID for the log range.

---

### `GET /tick/{tick_number}`

* **Description:** Retrieves all data (votes and tick data) for a specific tick.
* **Path Parameters:**
    * `{tick_number}`: The tick number to query.

---

### `GET /findLog/{fromTick}/{toTick}/{scIndex}/{logType}/{topic1}/{topic2}/{topic3}`

* **Description:** Searches for logs within a specified tick range, filtered by smart contract index, log type, and up to three topics. Find more about `findLog` [here](FINDLOG.MD)
* **Path Parameters:**
    * `{fromTick}`: The starting tick for the search range.
    * `{toTick}`: The ending tick for the search range.
    * `{scIndex}`: The smart contract index.
    * `{logType}`: The type of log to filter for.
    * `{topic1}`: The first topic filter.
    * `{topic2}`: The second topic filter.
    * `{topic3}`: The third topic filter.

---

### `GET /status`

* **Description:** Returns node status information, such as the current epoch, tick, and other relevant metadata.
* **Path Parameters:** None.