# RSS Reader

The RSS mini-app downloads articles from a FreshRSS server and stores them on the SD card as ordinary HTML books. Reading uses the built-in reader, so offline articles get the same pagination, fonts, reading progress, and bookmarks as any other book.

Open it from **Apps → RSS Reader**.

## Requirements

- A FreshRSS server reachable from the device's WiFi network.
- FreshRSS API access enabled, with an **API password** set. Find the address under **Settings → Account → API** in FreshRSS; it looks like `https://freshrss.example.net/api/greader.php`. The API password is separate from your login password.
- A saved WiFi network on the device (the app reuses the credentials from **Settings → WiFi**).

## Configuration

The app reads `rss.json` from the root of the SD card. `/.papyrix/apps/rss.json` is also accepted. If neither file exists, the app offers to write a template; you can also upload the file with the [web UI](webserver.md) or copy it on a computer.

```json
{
  "server": "https://freshrss.example.net/api/greader.php",
  "user": "alice",
  "apiPassword": "your-api-password",
  "verifyTls": true,
  "caCert": "",
  "folder": "/RSS",
  "articlesPerFeed": 15,
  "maxFeeds": 8,
  "syncIntervalMinutes": 60,
  "keepPerFeed": 40,
  "maxArticleBytes": 98304,
  "markReadOnOpen": true,
  "feeds": ""
}
```

| Key | Default | Meaning |
|---|---|---|
| `server` | — | API address. The trailing `/api/greader.php` may be omitted. |
| `user` | — | FreshRSS user name. |
| `apiPassword` | — | API password, not the login password. |
| `verifyTls` | `true` | Validate the server certificate against the built-in root bundle. Set `false` for a self-signed server, or point `caCert` at its certificate. |
| `caCert` | `""` | SD path to a PEM file to validate against, for a private certificate authority. |
| `folder` | `"/RSS"` | Folder for downloaded articles. It must be an absolute path and is created when missing. |
| `articlesPerFeed` | `15` | Newest items fetched per feed per sync (1–50). |
| `maxFeeds` | `8` | Feeds visited per sync (1–64). A longer subscription list is covered over successive syncs, continuing where the last one stopped. |
| `syncIntervalMinutes` | `60` | How long a sync stays fresh (5–43200). |
| `keepPerFeed` | `40` | Articles kept per feed. Older ones are deleted after a sync. |
| `maxArticleBytes` | `98304` | Largest article file written (4 KB–512 KB). A larger article is skipped, not truncated. |
| `markReadOnOpen` | `true` | Mark an article read on the server after you open it. |
| `includeRead` | `false` | Off: mirror unread state and fetch only unread items. On: also fetch the newest items the server already reports as read, which builds a shelf of recent articles. |
| `feeds` | `""` | Optional filter: comma-separated feed ids (`feed/12`), ids (`12`), or titles. Empty means every feed. |

Unknown keys are ignored, so a file written for a newer firmware still loads. Invalid values are clamped to the ranges above. Numbers must be JSON numbers and strings must be quoted; a syntax error is reported on the app's screen together with the byte offset.

After editing the file, use **Menu → Reload config** instead of rebooting.

## What a sync does

1. Signs in with the API password and reads the subscription list.
2. For each feed, downloads items published since the last successful sync (`articlesPerFeed` at most). Unread items only, unless `includeRead` is set; an item whose file is already on the card is skipped either way.
3. Writes one HTML file per item into `<folder>/<feed>/`, named `<date>-<title>-<hash>.html`. The name is derived from the item id, so a repeated download lands on the same file and is skipped instead of duplicated.
4. Uploads the read markers queued by opening articles.
5. Deletes the oldest articles beyond `keepPerFeed` for each feed.

Feeds rotate: a sync visits at most `maxFeeds` subscriptions and the next one starts where it left off, so a long list is covered over several syncs. The incremental window (items published since the last sync) only advances after a sync has swept the whole list; until then each run re-lists the same window and already-stored articles are skipped by name, so a feed that waits for a later run never misses anything. The menu shows the slice the last sync covered (`Feeds 17–32 of 33`). The optional `feeds` filter narrows the rotation to the feeds it allows.

A sync runs when the app opens if the last one is older than `syncIntervalMinutes`, and again on that interval while the app stays open. **Menu → Sync now** or the **Sync** button starts one immediately; when no network is connected, that opens the WiFi picker and the app returns to the sync once you connect. **Stop** (Back) aborts the transfer; articles already written stay on the card, and the next sync continues from the last successful one.

The app keeps the connection open across requests and reconnects when the server closes it. It does not request compressed responses, and it stops before writing when the card has less free space than the largest allowed article plus slack.

## Reading offline

Articles are plain `.html` files in a visible folder, so they also appear in the normal file browser. Opening one from the app hands off to the reader, which returns to the article's folder afterwards, so the next article is one step away.

Opening an article queues its read marker when `markReadOnOpen` is set. Up to 32 markers are kept; the newest ones survive when the queue is full, and each marker is dropped only after the server accepts it.

Because the file name contains a date, the article list is sorted newest first. Deleting an article by hand is safe: it is not re-downloaded unless the server still reports it as unread and it is newer than the last sync.

## Screen reference

| Screen | Buttons |
|---|---|
| Feeds | Up/Down select, Right opens the feed, Left syncs, Back leaves the app |
| Articles | Up/Down select, Right opens in the reader, Left syncs, Back returns to the feed list |
| Sync | Back stops the transfer |
| Menu | Up/Down select, Left/Right activates, Back closes |

The menu shows the last sync time, how many articles the last run stored or skipped, how many read markers are waiting, the TLS mode, and the free heap.

## Troubleshooting

- **"No rss.json on the card"** — the file is missing, or it could not be read. Press **Create** to write a template, then edit it.
- **"API password rejected"** — the API password is wrong or API access is disabled. Reset it in FreshRSS under **Settings → Account → API**.
- **"API address not found"** — `server` does not point at `api/greader.php` on your instance.
- **"TLS handshake failed"** — the certificate is not trusted. Set `"verifyTls": false`, or save the certificate as a PEM file on the card and set `"caCert"` to its path.
- **"not enough memory for TLS"** — the device is low on RAM for an HTTPS session. Plain `http://` uses no TLS memory, and closing other apps frees memory.
- **Folders appear but stay empty** — the server has no items matching the sync. Every feed is already marked read upstream, so set `"includeRead": true` (recent-articles shelf) or mark items unread in FreshRSS.
- **"card full"** — free space is below the largest allowed article plus slack. Lower `maxArticleBytes` or `keepPerFeed`.
- **"Offline: no WiFi"** — no saved network accepted the connection. Add one in **Settings → WiFi**; browsing stored articles still works.

## Notes for developers

`lib/FreshRss` holds the client core: configuration decoding, a streaming JSON scanner, HTTP/1.1 request and response framing, the offline article writer with its HTML sanitizer, and the sync orchestration. It has no Arduino or SD dependencies, so it builds and runs in the host test harness (`test/unit/rss/`).

The client speaks the Google Reader compatible API (`/api/greader.php`) that FreshRSS documents in its [API reference](https://freshrss.github.io/FreshRSS/en/developers/06_GoogleReader_API.html):

- `accounts/ClientLogin` returns the `Auth` token used in the `Authorization: GoogleLogin auth=...` header.
- `reader/api/0/subscription/list?output=json` lists feeds.
- `reader/api/0/stream/contents/feed/<id>` fetches items. `n` bounds the count, `xt=user/-/state/com.google/read` excludes read items, and `ot` is the lower date bound used for incremental syncs.
- `reader/api/0/token` and `reader/api/0/edit-tag` mark items read.

Responses stream through a fixed 256-byte token buffer: an article body of any size is sanitized and written to the card without ever being held in RAM. The sanitizer drops `script`, `style`, `iframe`, `svg`, and similar elements with their content, keeps a small set of text tags, keeps `href` on links only, and escapes any character or entity that the reader's XML pipeline cannot parse. `docs/architecture.md` describes how the app fits the mini-app host.
