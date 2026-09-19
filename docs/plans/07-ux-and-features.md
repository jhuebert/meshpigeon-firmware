# 07 — UX & Feature Specification

The interface is a **messaging app first**. Everything mesh-flavored is either
invisible or one tap deep. Material 3, Compose, ≥48 dp targets, sp-based type
(accessibility for young and old on all device sizes).

## 1. Information architecture

Bottom navigation (3 tabs) + drawer:

| Tab | Contents |
|---|---|
| **Chats** | list of conversations (channels + DMs) |
| **Contacts** | people & repeaters seen; pending/discovered; blocked list |
| **Map** | contacts/repeaters with positions, offline tiles (03 §map goal) |

- Left drawer: active identity switcher (avatars), settings, advanced.
- Top app bar on Chats: search icon + overflow (**Mark all as read**, Settings).

## 2. First-run onboarding (the only required flow)

1. **Welcome** — one screen, one primary button: "Get started". One-liner:
   "Message anywhere without internet."
2. **Name yourself** — text field, pre-filled suggestion. ("This is the name
   others will see.")
3. **Pick your region** — short list of radio regions ("Choose where you are —
   this tunes your radio"). Advanced users can expand "custom settings".
4. **Connect a radio** — scan sheet with friendly radio names; "Skip for now"
   is always visible (offline mode works, 06 §5).
5. **Land on Chats** — containing exactly one conversation: **Public**.
   A one-time coach mark points at the ✏️/"Start chat" button.

## 3. Chats list (channel list requirements)

Each row: **avatar (channel hash color / contact photo-initial), channel or
contact name, last-message snippet (small text), last-message timestamp,
unread badge**, pin indicator. Rows: pinned first, then by recency; muted
chats show a mute glyph. Long-press row: **Pin/Unpin, Mark read, Mute,
Notifications…, Delete/Rename (channels)**. FAB (or top-right per platform
convention): **Start chat** → sheet with: *Direct message* (pick contact),
*Private channel* (name+key), *Shared channel* (join by name/QR), *Join with
QR/link*.

## 4. Conversation view

- Messages: mine right-aligned in a tinted bubble; others left with **sender
  name in group chats** (color-consistent per contact).
- **Delivery states (product rule: user must see the mesh heard them):**
  ⏳ queued → ✓ sent → **✓ heard (a repeater relayed it)** → ✓✓ confirmed by
  recipient (DMs). Failed shows retry affordance (**long-press → Retry**;
  a 30 s grace window still accepts late ACKs — 03 §4). One message in flight
  per conversation; queued messages show ⏳ in place.
- **Sub info line** under each message (small, optional setting "show signal
  details", default: delivery state only): time, hops, SNR bars, region.
  Long-press → *Message details* sheet with everything (raw time, RSSI, tag,
  path) — requirement "information accessible somewhere".
- **Reply:** long-press → Reply → composer shows a quoted chip; sending
  prepends the sender's tag (`@name `) + body — plain-message interop
  compatible (requirement).
- **React:** long-press → emoji row (👍❤️😂😮😢➕). MeshPigeon encodes a targeted
  reaction via GRP_DATA; for non-MeshPigeon peers the app *also* offers
  "@name emoji" fall-back text which lands near the user's last message
  (best-effort requirement). Received reactions render under the target
  bubble; unmatched ones fall back to a normal message so nothing is lost.
- **Copy:** long-press → Copy text. Context actions also include
  **Mark as unread** (from that message onward) and **Delete locally** (with
  confirm; never affects the mesh).
- **List behavior:** jump-to-bottom floating button, lazy load of older
  messages on scroll-up, bubble width capped ≈ 72 % of screen.
- **Link preview:** if a message contains a URL, an overflow action
  "Preview link" fetches title/image **only on tap** (no automatic internet
  traffic; requires connectivity, gracefully absent off-grid). GIF URLs get an
  inline preview card.
- **Location message:** renders a static mini-map preview (offline tiles) in
  the bubble + a chip "Open in maps" that dispatches to the device's geo:/
  mapping app.
- **Character counter:** composer shows `123 / 160` (or protocol budget for
  current channel/DM with path), turning amber near the limit; over-budget
  send is blocked with explanation. Budget derives from payload size math in
  `:core-protocol` (path size aware).
- **Plus (+) button** in the composer: *Attach my contact card · Attach my
  location · Attach location (pick on map) · Share channel · Share contact ·
  Photo (stretch)*. Shares render as rich cards in-bubble.

## 5. Start chat / DMs from strangers

- Incoming DMs from unknown contacts create a **request** conversation with
  accept/block actions (never silently dropped, never auto-opened). A
  **Pending contacts** screen (Contacts → "Discovered") lists passively heard
  nodes: tap to add, long-press for options, overflow → "Clear all" (confirm).
  Contacts overflow also offers **Import contact from clipboard** (meshcore://
-style URI) and **Say hi nearby** (zero-hop advert).
- "Start chat → Direct message" lists known contacts + any pending contacts
  (from adverts) with a search field; also "share my contact" reminder for
  people who can't see you yet.

## 6. Channels

- **Public** is the only conversation on first use (hard requirement).
- Create private channel (random or typed key), shared channel (name-derived
  key). Rename/delete with confirmation (delete = local leave; also offers
  "also delete history").
- Channel sharing: QR + `https://meshpigeon.app/c/<base64>` deep link carrying
  name+key; **key paste is tolerant** (spaces/dashes stripped from 32-hex
pasted keys); importing shows a preview ("Join channel **Trail Talk**?") before
  commit. Also works from the composer plus-menu ("Share channel") as a rich
  card.

## 7. Contacts & blocking

- Contact row: name, pubkey-hash avatar, last-seen; actions: message, share
  contact card, block, rename (local alias), copy pubkey.
- **Block** in group chats: long-press message → "Block @name" or from the
  contact sheet; blocked = messages never render, adverts ignored, reversible
  in Contacts → Blocked.
- Repeaters appear under a "Radios" section (read-only, with map pin + last
  heard) — no chat.

## 8. Notifications

- Per conversation: **Default / Important only / Muted**, plus optional
  per-conversation sound/LED override. Default for Public: muted-ish
  ("mentions only") to keep the mesh calm for newcomers; DMs default: notify.
- Notification group "MeshPigeon"; reply action inline where safe; no message
  content on lock screen unless enabled.
- **Storm control:** ≥ 3 s minimum interval between notifications; bursts
  collapse into one batch summary ("3 messages, 2 new contacts") so adverts
  and busy channels can't spam the shade.
- **Tap navigates directly to the conversation** (never just to the app root).

## 9. Search

- Chats tab search field: filters conversations by name **and** last-message
  text, live.
- Overflow → **Search messages**: FTS across all conversations with
  jump-to-message (highlights match, scrolls context).
- Contacts tab has its own filter field.

## 10. Map

- OSM-style offline map view showing contacts and repeaters with positions
  (from adverts/location shares); tap → contact card with "message" and
  "last seen here". No internet tile server required (bundled low-zoom tiles +
  downloaded region packs in Advanced). No official MeshCore internet map.

## 11. QR & links everywhere

| Object | QR/link |
|---|---|
| Channel join | name+key QR / `…/c/…` link |
| Contact card | pubkey+name QR / `…/u/…` link |
| App radio/setup profile | `…/s/…` (region + settings) — "set up my radio" QR for helpers helping newcomers |
| Identity backup | passphrase-protected export QR (Advanced, 08) |

QR payloads follow a versioned TLV so MeshCore QR formats can be imported
(read-only compatibility) without being emitted.

## 12. Regions, presented stupidly simply

Onboarding + Settings: "Region" list (e.g., US-915, EU-868, Oceania-915,
Custom) — a bundled preset table at least as broad as the ecosystem's
(per-country incl. narrow variants); custom stays Advanced; presets are
importable via QR/link. Choosing a region tunes the radio (05 §4 keeps this
separate from connection prefs). One line of help text under the picker.
That's the whole UI.

## 13. Visual system

- Type scale: 16 sp base, list titles 16 sp medium, metadata 12–13 sp but
  never below 12 sp; high-contrast dark & light themes.
- Empty states teach: "No messages yet — say hello" with the exact button.
- No icon-only actions in critical flows; destructive actions always confirm.
- Everything Discoverability-first: primary actions are labeled buttons, not
  buried gestures; gestures exist as accelerators, never the only path.
